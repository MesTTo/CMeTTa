/* Purpose: the C half of the C binding. Boot SWI-Prolog in this process,
 *   consult the engine, and move values between C structures and engine terms
 *   directly, with no wire encoding in between.
 *
 * Assumes:
 *   - SWI-Prolog 10 with threads
 *     [source: /usr/lib/swi-prolog/include/SWI-Prolog.h, PLVERSION 100113]
 *   - extensions/cmetta/bridge.pl is loaded by extensions/cmetta/extension.pl, which
 *     the engine globs at boot, and which finds this file because mt_open()
 *     registers '$mt_present'/0 BEFORE it consults engine/metta.pl
 *   - a term handed out by the bridge is valid only inside the foreign frame
 *     the call opened, so every decode completes before the frame is discarded
 *     [source: SWI-Prolog.h:432-435; C1 in ai-cmetta-c-constraints.md]
 *
 * Guarantees:
 *   - no Prolog exception crosses into a caller: every query runs under
 *     PL_Q_CATCH_EXCEPTION, and the ball is rendered by the bridge into the
 *     thread-local error text
 *   - an engine term with no MeTTa reading is REFUSED by name rather than
 *     stringified into something that cannot go home again
 *   - an ampersand-prefixed atom becomes MT_SPACE only when the engine
 *     says it is a space, which is a question this seat can ask and the
 *     out-of-process seats cannot [C5 in ai-cmetta-c-constraints.md]
 *   - a door that reaches the engine before mt_open(), or after mt_close(),
 *     REFUSES with MT_MISUSE naming mt_open() instead of dereferencing a
 *     thread environment that is not there
 *     [tested: tests/test_cmetta.c, test_a_door_before_the_runtime_refuses;
 *     commit=c530ccb8fb7d0a5b2aa53df6e9f981ada9f81be8]
 *   - no term crosses this file on the C stack: reading, writing, comparing,
 *     releasing and binding all walk with their own frame stack, so nesting
 *     costs heap rather than the 8 MB the thread was given
 *     [tested: tests/test_cmetta.c, test_a_deep_term_does_not_overrun_the_stack;
 *     commit=c530ccb8fb7d0a5b2aa53df6e9f981ada9f81be8]
 *
 * Owns resources: the process's Prolog runtime, released by mt_close(); the
 *   op table; one malloc'ed box per live mt_object, released when both the
 *   C atom and the engine blob have let go.
 *
 * Guarded by: nothing, and cmetta.h's "Guarded by" says why: an atom is
 *   immutable after construction and its refcount is atomic, the error state
 *   is thread-local, and the runtime singleton and the op table are written
 *   at boot and at mt_def() time, which the header requires a host to do
 *   before the threads that evaluate start. This field claimed a g_lock for
 *   years; there has never been one in this file.
 *
 * Decides:
 *   - variables decode to their SOURCE names when the engine supplies a name
 *     state for them, and to SWI's written form otherwise, because a caller
 *     who wrote $x wants $x back and a caller reading an internal variable
 *     wants something stable rather than a lie.
 *   - every walk over a term is ITERATIVE, over the MT_STACK below. A
 *     recursive walk is shorter to read and this file had five of them, and
 *     all five die on data: decode at 80,000 levels of nesting, encode and
 *     mt_bound at 80,000, mt_eq at 200,000 and mt_drop at 400,000, each a
 *     SIGSEGV that no host can catch [measured 2026-08-31; C35 in
 *     ai-cmetta-c-constraints.md].
 *
 * Open Obligations:
 *   To Do: None
 *   Hacks: None
 *   Future Enhancements: None
 */

/* strdup is POSIX 2008 rather than C11, and -std=c11 hides it. */
#define _POSIX_C_SOURCE 200809L

#include "cmetta.h"

#include <SWI-Prolog.h>
#include <SWI-Stream.h>

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Invariants the code below relies on, checked by the compiler rather than
   trusted to a comment. Each one is a thing that would fail quietly. */
static_assert(MT_NONE == -1,
              "mt_kind_of(NULL) answers MT_NONE, and a kind of 0 would be a "
              "real kind");
static_assert(MT_HANDLE == MT_SYMBOL + 11,
              "the kind enum is contiguous and twelve long, which is what "
              "makes mt_kind_str's switch total and its missing-case warning "
              "meaningful");
static_assert(MT_SHOW_SLOTS > 1,
              "one slot cannot survive two mt_show() calls in one printf, "
              "which is the whole reason the buffer rotates");
static_assert(MT_OK == 0,
              "mt_ok() is a comparison against MT_OK, and cmetta_clear() zeroes "
              "the status to mean success");

/* SWI takes a foreign predicate as pl_function_t, which is void *. ISO C does
   not guarantee a function pointer converts to an object pointer, so the two
   are punned through a union: POSIX requires the conversion to work (dlsym
   returns void * for code), and a union member read is the one spelling that
   says so without a diagnostic. */
typedef void (*mt_anyfn)(void);

static pl_function_t as_pl_function(mt_anyfn fn)
{ union { mt_anyfn code; pl_function_t data; } u;
  u.code = fn;
  return u.data;
}

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && \
    !defined(__STDC_NO_ATOMICS__)
#include <stdatomic.h>
#define MT_ATOMIC _Atomic
#define MT_INC(p) atomic_fetch_add_explicit((p), 1u, memory_order_relaxed)
#define MT_DEC(p) atomic_fetch_sub_explicit((p), 1u, memory_order_acq_rel)
#else
#define MT_ATOMIC
#define MT_INC(p) ((*(p))++)
#define MT_DEC(p) ((*(p))--)
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && \
    !defined(__STDC_NO_THREADS__)
#define MT_TLS _Thread_local
#elif defined(__GNUC__)
#define MT_TLS __thread
#else
#define MT_TLS
#endif

/* ================================================================== *
 * Error state
 * ================================================================== */

/* errno's contract: SET on failure, NOT cleared on success, so a run of calls
   is checked once at the end rather than one `if` per call. The state is
   thread-local, which is what errno itself became once threads existed. */
#define MT_ERR_MAX 2048
static MT_TLS char g_err[MT_ERR_MAX];
static MT_TLS mt_status g_status = MT_OK;
/* A callback needs to tell a failure raised DURING this application from the
   errno-shaped failure that was already present when it began. Comparing the
   status is not enough: two consecutive failures may have the same kind. */
static MT_TLS uint64_t g_error_generation;

static mt_status err_set(mt_status status, const char *fmt, ...)
{ va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_err, sizeof(g_err), fmt, ap);
  va_end(ap);
  g_status = status;
  g_error_generation++;
  return status;
}

/* The failing constructors answer NULL, so this spelling lets them set the
   reason and return in one line. */
static void *err_null(mt_status status, const char *fmt, ...)
{ va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_err, sizeof(g_err), fmt, ap);
  va_end(ap);
  g_status = status;
  g_error_generation++;
  return NULL;
}

static mt_status err_copy(mt_status status, const char *text)
{ snprintf(g_err, sizeof(g_err), "%s", text);
  g_status = status;
  g_error_generation++;
  return status;
}

static void err_reclassify(mt_status status)
{ g_status = status;
  g_error_generation++;
}

void mt_clear(void)
{ g_err[0] = '\0';
  g_status = MT_OK;
}

mt_status mt_error(void)
{ return g_status;
}

bool mt_ok(void)
{ return g_status == MT_OK;
}

const char *mt_errmsg(void)
{ return g_status == MT_OK ? NULL : g_err;
}

const char *mt_status_str(mt_status status)
{ switch ( status )
  { case MT_OK:          return "ok";
    case MT_ROW:         return "row";
    case MT_DONE:        return "done";
    case MT_FAIL:        return "no answer";
    case MT_ERROR:       return "engine error";
    case MT_NOMEM:       return "out of memory";
    case MT_MISUSE:      return "misuse";
    case MT_UNSUPPORTED: return "unsupported value";
    case MT_LIMIT:       return "stopped by a bound";
  }
  return "unknown status";
}

const char *mt_version(void)
{ return "0.1.0";
}

const char *mt_kind_str(mt_kind kind)
{ switch ( kind )
  { case MT_NONE:     return "None";
    case MT_SYMBOL:   return "Symbol";
    case MT_TEXT:     return "String";
    case MT_INT:      return "Number";
    case MT_FLOAT:    return "Number";
    case MT_BIGINT:   return "BigInt";
    case MT_RATIONAL: return "Rational";
    case MT_BOOL:     return "Bool";
    case MT_VARIABLE: return "Variable";
    case MT_EXPR:     return "Expression";
    case MT_SPACE:    return "Space";
    case MT_OBJECT:   return "Grounded";
    case MT_HANDLE:   return "Grounded";
  }
  return "unknown kind";
}

const char *mt_effect_str(mt_effect effect)
{ switch ( effect )
  { case MT_PURE:            return "pureStructural";
    case MT_LOOKUP:           return "readOnlyLookup";
    case MT_NONDET: return "nondeterministicReadOnly";
    case MT_WRITES:               return "writesState";
    case MT_IO:                  return "oracleIO";
  }
  return NULL;
}

/* A foreign frame, or 0 with the reason recorded. SWI answers 0 when the
   stacks cannot hold another frame and when atom garbage collection is
   running in this thread, which a blob release callback reaches, and a 0
   handed to PL_discard_foreign_frame is undefined behaviour
   [source: SWI-Prolog manual, PL_open_foreign_frame, "Returns (fid_t)0 on
   failure"]. Every site in this file that opens a frame asks here. */
static fid_t frame_open(const char *door)
{ fid_t f = PL_open_foreign_frame();
  if ( !f )
    err_set(MT_NOMEM,
            "%s could not open a Prolog foreign frame: the engine's stacks "
            "are full, or this thread is inside atom garbage collection",
            door);
  return f;
}

/* Its pair, which accepts the frame that was never opened. A caller holding a
   frame handed out by another function can then discard unconditionally. */
static void frame_close(fid_t f)
{ if ( f ) PL_discard_foreign_frame(f);
}

/* ================================================================== *
 * The walk stack
 * ================================================================== */

/* Every walk over a term in this file uses one of these instead of calling
   itself. A term is a tree and the obvious walk is recursive, but the depth
   is the DATA's, not the program's: a thread gets 8 MB by default and this
   file's five recursive walks died between 80,000 and 400,000 levels of
   nesting, each one a SIGSEGV rather than a refusal [measured 2026-08-31,
   ai-tmp/cseat-probe-d4.c on an 8 MB stack]. RapidJSON reached the same
   conclusion and its kParseIterativeFlag is "constant complexity in terms of
   function call stack size"; its issue #2217 is the other half of the lesson,
   that an iterative parser is undone by a recursive destructor, which is why
   mt_drop() walks with one of these too
   [source: https://github.com/Tencent/rapidjson/issues/2217].

   The stack starts in the caller's own frame and moves to the heap only when
   it outgrows it, so a term of ordinary depth allocates nothing at all. Each
   walk pushes ONE frame per level of nesting rather than one per child, so a
   wide expression costs nothing either. That is llvm::SmallVector's shape and
   Boehm's mark stack's shape, for the same two reasons.

   TYPED BY MACRO, one instantiation per walk. The first version of this held
   bytes and kept the element width in the struct, which is tidier to read and
   costs a memcpy CALL with a runtime size on every push and an imul on every
   read: +1.55% on term-out and +0.66% on cursor-step, both outside their
   bands [measured 2026-08-31, interleaved A/B against the recursive walks,
   perf stat -e instructions:u, min of 3]. A typed push is one struct store
   the compiler inlines. klib's kvec.h is the shape and the reason
   [source: https://github.com/attractivechaos/klib/blob/master/kvec.h]. */
#define MT_WALK_FRAMES 16

#define MT_STACK(type)                                                    \
  struct { type *items, *fixed; size_t n, cap; }

/* `base` is an array in the caller's own frame, and its LENGTH is the inline
   capacity, so the two cannot drift apart. */
#define stack_init(s, base)                                               \
  ( (s)->items = (s)->fixed = (base),                                     \
    (s)->cap = sizeof (base) / sizeof *(base),                            \
    (s)->n = 0 )

/* Push, or answer false having left the stack exactly as it was, so a walk
   that cannot grow can still unwind through the frames it already holds. */
#define stack_push(s, frame)                                              \
  ( ( (s)->n < (s)->cap ||                                                \
      ( (s)->items = stack_grow_((s)->items, (s)->fixed, &(s)->cap,       \
                                 sizeof *(s)->items),                     \
        (s)->n < (s)->cap ) )                                             \
    ? ( (s)->items[(s)->n++] = (frame), true )                            \
    : false )

/* The innermost frame, or NULL. Valid until the next push, which may move the
   block, so a walk reads what it needs out before it pushes again. */
#define stack_top(s)  ( (s)->n ? &(s)->items[(s)->n - 1] : NULL )
#define stack_pop(s)  ( (void)(s)->n-- )

#define stack_free(s)                                                     \
  do                                                                      \
  { if ( (s)->items != (s)->fixed ) free((s)->items);                     \
    (s)->items = (s)->fixed;                                              \
    (s)->n = 0;                                                           \
  } while (0)

/* Twice the room, or the block it was given back unchanged: growing is the
   only part of a push that is not a store, so it is the only part that is a
   function, and a failure leaves the caller's stack intact. */
static void *stack_grow_(void *items, void *fixed, size_t *cap, size_t width)
{ size_t bigger = *cap * 2;
  void *grown = ( items == fixed ) ? malloc(bigger * width)
                                   : realloc(items, bigger * width);
  if ( !grown ) return items;
  if ( items == fixed ) memcpy(grown, fixed, *cap * width);
  *cap = bigger;
  return grown;
}

/* ================================================================== *
 * Atoms
 * ================================================================== */

/* A live C value the language carries by reference. Two owners share one box:
   the C atom that names it and, once it has crossed, the engine blob. Each
   drops a reference; the last one out runs the caller's release. */
typedef struct mt_box
{ MT_ATOMIC unsigned refs;
  void                 *value;
  char                 *type;
  mt_free_fn  release;
  mt_fn           apply;
  void                 *user;
} mt_box_t;

struct mt_atom
{ MT_ATOMIC unsigned refs;
  mt_kind          kind;
  union
  { struct { char *text; size_t len; }        t;  /* sym var str space bigint */
    int64_t                                   i;
    double                                    f;
    bool                                      b;
    struct { int64_t num, den; }               r;
    struct { mt_atom **kids; size_t n; }  e;
    mt_box_t                               *box;
  } u;
};

static mt_atom *atom_alloc(mt_kind kind)
{ mt_atom *a = calloc(1, sizeof(*a));
  if ( !a )
  { err_set(MT_NOMEM, "out of memory allocating an atom");
    return NULL;
  }
  a->refs = 1;
  a->kind = kind;
  return a;
}

static mt_atom *atom_text(mt_kind kind, const char *text, size_t len)
{ mt_atom *a;
  if ( !text )
  { err_set(MT_MISUSE, "%s needs text, not NULL", mt_kind_str(kind));
    return NULL;
  }
  if ( !(a = atom_alloc(kind)) ) return NULL;
  if ( !(a->u.t.text = malloc(len + 1)) )
  { free(a);
    err_set(MT_NOMEM, "out of memory copying %zu bytes of text", len);
    return NULL;
  }
  memcpy(a->u.t.text, text, len);
  a->u.t.text[len] = '\0';
  a->u.t.len = len;
  return a;
}

static void box_release(mt_box_t *box)
{ if ( !box ) return;
  if ( MT_DEC(&box->refs) == 1 )
  { if ( box->release ) box->release(box->value);
    free(box->type);
    free(box);
  }
}

mt_atom *mt_keep(const mt_atom *atom)
{ mt_atom *a = (mt_atom *)atom;
  if ( a ) MT_INC(&a->refs);
  return a;
}

/* One atom's own memory. An expression's CHILDREN are not released here:
   drop_expr() dismantles those with a stack of its own. */
static inline void free_leaf(mt_atom *a)
{ switch ( a->kind )
  { case MT_SYMBOL:
    case MT_VARIABLE:
    case MT_TEXT:
    case MT_SPACE:
    case MT_BIGINT:
    case MT_HANDLE:
      free(a->u.t.text);
      break;
    case MT_OBJECT:
      box_release(a->u.box);
      break;
    default:
      break;
  }
  free(a);
}

/* One level of an expression being dismantled: the children still to release
   and how far along them the walk is. */
typedef struct drop_frame
{ mt_atom **kids;
  size_t    n, at;
} drop_frame;

typedef MT_STACK(drop_frame) drop_stack;

/* Take one more level, or, when the machine cannot spare the frame, leave
   that subtree allocated and say so. Releasing is the one job that cannot be
   refused, and the recursion this replaced would answer an 8 MB stack with a
   SIGSEGV; memory this walk cannot reach is recoverable and that is not. */
static inline void drop_push(drop_stack *frames, mt_atom **kids,
                             size_t n)
{ drop_frame frame;
  frame.kids = kids;
  frame.n = n;
  frame.at = 0;
  if ( stack_push(frames, frame) ) return;
  err_set(MT_NOMEM, "out of memory releasing a nested expression; the %zu "
                    "children below it are still held", n);
  free(kids);
}

/* An expression whose last reference has gone, released one frame per level
   of nesting rather than one C call per level. */
static void drop_expr(mt_atom *root)
{ drop_frame fixed[MT_WALK_FRAMES];
  drop_stack frames;
  drop_frame *f;

  stack_init(&frames, fixed);
  drop_push(&frames, root->u.e.kids, root->u.e.n);
  free(root);

  while ( (f = stack_top(&frames)) != NULL )
  { mt_atom *kid;
    if ( f->at == f->n )
    { free(f->kids);
      stack_pop(&frames);
      continue;
    }
    kid = f->kids[f->at++];
    if ( MT_DEC(&kid->refs) != 1 ) continue;   /* another owner still holds it */
    if ( kid->kind != MT_EXPR )
    { free_leaf(kid);
      continue;
    }
    /* The node's own memory goes now and its children become a new level.
       `f` is not touched afterwards: the push may move the block. */
    drop_push(&frames, kid->u.e.kids, kid->u.e.n);
    free(kid);
  }
  stack_free(&frames);
}

void mt_drop(const mt_atom *atom)
{ mt_atom *a = (mt_atom *)atom;
  if ( !a || MT_DEC(&a->refs) != 1 ) return;
  if ( a->kind == MT_EXPR ) drop_expr(a);
  else free_leaf(a);
}

/* atom_text() refuses NULL by name, which is why these hand it the pointer
   rather than testing it first: a ternary that answered NULL on its own left
   mt_error() saying `ok` after a constructor had failed, and cmetta.h's rule 2
   is that every function that can fail says so
   [tested: tests/test_cmetta.c, test_a_failed_constructor_says_so;
   commit=c530ccb8fb7d0a5b2aa53df6e9f981ada9f81be8]. */
mt_atom *mt_sym(const char *name)
{ return atom_text(MT_SYMBOL, name, name ? strlen(name) : 0);
}

mt_atom *mt_var(const char *name)
{ return atom_text(MT_VARIABLE, name, name ? strlen(name) : 0);
}

mt_atom *mt_text(const char *text)
{ return atom_text(MT_TEXT, text, text ? strlen(text) : 0);
}

mt_atom *mt_textn(const char *text, size_t length)
{ return atom_text(MT_TEXT, text, length);
}

mt_atom *mt_num(int64_t value)
{ mt_atom *a = atom_alloc(MT_INT);
  if ( a ) a->u.i = value;
  return a;
}

mt_atom *mt_real(double value)
{ mt_atom *a = atom_alloc(MT_FLOAT);
  if ( a ) a->u.f = value;
  return a;
}

mt_atom *mt_bool(bool value)
{ mt_atom *a = atom_alloc(MT_BOOL);
  if ( a ) a->u.b = value;
  return a;
}

mt_atom *mt_bigint(const char *decimal)
{ const char *p = decimal;
  if ( !decimal )
  { err_set(MT_MISUSE, "mt_bigint needs decimal digits, not NULL");
    return NULL;
  }
  if ( *p == '-' ) p++;
  if ( !*p )
  { err_set(MT_MISUSE, "%s is not an integer", decimal);
    return NULL;
  }
  for (; *p; p++)
  { if ( *p < '0' || *p > '9' )
    { err_set(MT_MISUSE,
              "%s is not an integer: only decimal digits and a leading "
              "minus are read here", decimal);
      return NULL;
    }
  }
  return atom_text(MT_BIGINT, decimal, strlen(decimal));
}

/* Magnitude as an unsigned, so INT64_MIN does not overflow on the way. */
static uint64_t magnitude(int64_t value)
{ return value < 0 ? (uint64_t)-(value + 1) + 1 : (uint64_t)value;
}

static uint64_t gcd_u64(uint64_t a, uint64_t b)
{ while ( b )
  { uint64_t t = a % b;
    a = b;
    b = t;
  }
  return a;
}

/* The pair is stored in CANONICAL form: lowest terms, sign on the numerator.
   That is what Python's fractions.Fraction and Boost.Rational both do, and
   more to the point it is what the engine does, so a ratio built here is one
   the engine can read: SWI writes -1r2 and its reader refuses 1r-2, and a
   negative denominator therefore built an atom that could not cross at all
   [measured 2026-08-31: mt_del(space, mt_rational(1, -2)) refused inside
   encode(); C32 in ai-cmetta-c-constraints.md]. */
mt_atom *mt_rational(int64_t numerator, int64_t denominator)
{ mt_atom *a;
  uint64_t divisor;

  if ( denominator == 0 )
  { err_set(MT_MISUSE, "a rational cannot have a zero denominator");
    return NULL;
  }
  /* Equal halves are the ratio 1, taken first because it is the one case
     whose common divisor does not fit int64_t: gcd(|n|, |d|) reaches 2^63
     only when both are INT64_MIN. */
  if ( numerator == denominator )
  { numerator = denominator = 1;
  } else
  { divisor = gcd_u64(magnitude(numerator), magnitude(denominator));
    if ( divisor > 1 )
    { numerator /= (int64_t)divisor;
      denominator /= (int64_t)divisor;
    }
  }
  if ( denominator < 0 )
  { if ( numerator == INT64_MIN || denominator == INT64_MIN )
    { err_set(MT_UNSUPPORTED,
              "%lld/%lld is exact but its canonical form is not: moving the "
              "sign to the numerator overflows int64_t",
              (long long)numerator, (long long)denominator);
      return NULL;
    }
    numerator = -numerator;
    denominator = -denominator;
  }
  /* A canonical denominator of 1 is an INTEGER, which is the last half of the
     same canonicalisation and the half the engine decides: SWI evaluates
     `3 rdiv 1` to 3, so a whole-number ratio stored in a space came back as
     MT_INT and mt_eq() then answered false against the atom that was stored
     [measured 2026-08-31: mt_rational(3, 1) built a Rational printing as 3;
     adding it and matching it back read Number, equal to mt_num(3) and NOT
     equal to what went in; tested: test_a_ratio_is_canonical_in_both_halves;
     commit=c530ccb8fb7d0a5b2aa53df6e9f981ada9f81be8]. */
  if ( denominator == 1 ) return mt_num(numerator);
  if ( !(a = atom_alloc(MT_RATIONAL)) ) return NULL;
  a->u.r.num = numerator;
  a->u.r.den = denominator;
  return a;
}

mt_atom *mt_spaceref(const char *name)
{ if ( !name || name[0] != '&' )
  { err_set(MT_MISUSE,
            "a space reference is written with a leading ampersand; %s is not",
            name ? name : "NULL");
    return NULL;
  }
  return atom_text(MT_SPACE, name, strlen(name));
}

mt_atom *mt_exprv(size_t count, mt_atom **children)
{ mt_atom *a;
  size_t i;
  bool bad = false;

  if ( count > 0 && !children )
    return err_null(MT_MISUSE,
                    "mt_exprv was asked for %zu children and given no array",
                    count);

  for (i = 0; i < count; i++)
    if ( !children[i] ) bad = true;

  if ( bad || !(a = atom_alloc(MT_EXPR)) )
  { /* Steal-on-success, release-on-failure: a NULL from an inner constructor
       must not leak the siblings that did succeed. */
    for (i = 0; i < count; i++) mt_drop(children[i]);
    if ( bad )
      err_set(MT_MISUSE,
              "an expression child was NULL; the constructor that made it "
              "failed and mt_errmsg() said why at the time");
    return NULL;
  }
  if ( count > 0 )
  { if ( !(a->u.e.kids = malloc(count * sizeof(*a->u.e.kids))) )
    { free(a);
      for (i = 0; i < count; i++) mt_drop(children[i]);
      err_set(MT_NOMEM, "out of memory building an expression of %zu", count);
      return NULL;
    }
    memcpy(a->u.e.kids, children, count * sizeof(*a->u.e.kids));
  }
  a->u.e.n = count;
  return a;
}

/* mt_atom_of() widens every integer type to long long and every floating
   type to long double before it dispatches, so there is one branch to land on
   rather than nine. These are those landings. */
mt_atom *mt_num_(long long value)      { return mt_num((int64_t)value); }
mt_atom *mt_real_(long double value)   { return mt_real((double)value); }
mt_atom *mt_same(mt_atom *atom)     { return atom; }
mt_atom *mt_same_c(const mt_atom *atom) { return (mt_atom *)atom; }

mt_atom *mt_unit(void)
{ return mt_exprv(0, NULL);
}

mt_kind mt_kind_of(const mt_atom *atom)
{ return atom ? atom->kind : MT_NONE;
}

const char *mt_name(const mt_atom *atom)
{ if ( !atom ) return NULL;
  switch ( atom->kind )
  { case MT_SYMBOL:
    case MT_VARIABLE:
    case MT_TEXT:
    case MT_SPACE:
    case MT_BIGINT:
    case MT_HANDLE:
      return atom->u.t.text;
    default:
      return NULL;
  }
}

size_t mt_name_len(const mt_atom *atom)
{ return mt_name(atom) ? atom->u.t.len : 0;
}

int64_t mt_int(const mt_atom *atom)
{ if ( !atom || atom->kind != MT_INT )
  { err_set(MT_MISUSE,
            "mt_int wants an exact integer that fits int64_t; this is %s. "
            "A Float is not rounded here and a BigInt does not fit by "
            "definition; read those with mt_float or mt_name",
            atom ? mt_kind_str(atom->kind) : "NULL");
    return 0;
  }
  return atom->u.i;
}

/* Promotes where nothing is lost and refuses where something would be, which
   is the lattice reading in decision 5 of the header. 2^53 is where a double
   stops holding every integer. */
#define MT_EXACT_IN_DOUBLE 9007199254740992LL

double mt_float(const mt_atom *atom)
{ if ( !atom )
  { err_set(MT_MISUSE, "mt_float wants a Number; this is NULL");
    return 0.0;
  }
  switch ( atom->kind )
  { case MT_FLOAT:
      return atom->u.f;
    case MT_INT:
      if ( atom->u.i <= -MT_EXACT_IN_DOUBLE ||
           atom->u.i >= MT_EXACT_IN_DOUBLE )
      { err_set(MT_UNSUPPORTED,
                "%lld does not fit a double exactly, and rounding it here "
                "would answer a different number; read it with mt_int",
                (long long)atom->u.i);
        return 0.0;
      }
      return (double)atom->u.i;
    case MT_RATIONAL:
      return (double)atom->u.r.num / (double)atom->u.r.den;
    default:
      err_set(MT_MISUSE, "mt_float wants a Number; this is %s",
              mt_kind_str(atom->kind));
      return 0.0;
  }
}

bool mt_truth(const mt_atom *atom)
{ if ( !atom || atom->kind != MT_BOOL )
  { err_set(MT_MISUSE, "mt_truth wants a Bool; this is %s",
            atom ? mt_kind_str(atom->kind) : "NULL");
    return false;
  }
  return atom->u.b;
}

mt_ratio mt_ratio_of(const mt_atom *atom)
{ mt_ratio out = { 0, 0 };
  /* An Int reads as itself over one, which is rule 5's promotion and exact.
     It has to, now that a canonical denominator of 1 makes an Int: without it
     mt_ratio_of(mt_rational(3, 1)) refused the very atom mt_rational built.
     A Bigint still refuses, because n/1 for an n outside int64_t is the
     conversion the lattice says to refuse rather than round. */
  if ( atom && atom->kind == MT_INT )
  { out.num = atom->u.i;
    out.den = 1;
    return out;
  }
  if ( !atom || atom->kind != MT_RATIONAL )
  { err_set(MT_MISUSE, "mt_ratio_of wants a Rational or an Int; this is %s",
            atom ? mt_kind_str(atom->kind) : "NULL");
    return out;
  }
  out.num = atom->u.r.num;
  out.den = atom->u.r.den;
  return out;
}

size_t mt_len(const mt_atom *atom)
{ return ( atom && atom->kind == MT_EXPR ) ? atom->u.e.n : 0;
}

const mt_atom *mt_at(const mt_atom *atom, size_t index)
{ if ( !atom || atom->kind != MT_EXPR || index >= atom->u.e.n ) return NULL;
  return atom->u.e.kids[index];
}

/* Two atoms of the same kind, compared WITHOUT their children: for an
   expression this is the arity, which is what tells the walk below whether
   there is any point descending. */
static bool eq_shallow(const mt_atom *a, const mt_atom *b)
{ switch ( a->kind )
  { case MT_SYMBOL:
    case MT_VARIABLE:
    case MT_TEXT:
    case MT_SPACE:
    case MT_BIGINT:
    case MT_HANDLE:
      return a->u.t.len == b->u.t.len &&
             memcmp(a->u.t.text, b->u.t.text, a->u.t.len) == 0;
    case MT_INT:      return a->u.i == b->u.i;
    case MT_FLOAT:
      /* SWI preserves signed zero but canonicalises NaN payloads when a float
         enters a term. The space predicates therefore distinguish the zeros
         and identify every NaN, so the pure-C door must too.
         [tested: tests/test_cmetta.c,
         test_float_identity_agrees_with_the_engine; commit=WORKTREE] */
      return (isnan(a->u.f) && isnan(b->u.f)) ||
             (!isnan(a->u.f) && !isnan(b->u.f) &&
              memcmp(&a->u.f, &b->u.f, sizeof(a->u.f)) == 0);
    case MT_BOOL:     return a->u.b == b->u.b;
    case MT_RATIONAL: return a->u.r.num == b->u.r.num &&
                                a->u.r.den == b->u.r.den;
    case MT_EXPR:     return a->u.e.n == b->u.e.n;
    case MT_OBJECT:
      /* By identity: the whole point of a live value is that its contents
         never become comparable text. */
      return a->u.box == b->u.box;
    case MT_NONE:
      /* Unreachable: both atoms were proven non-NULL by the caller. Named
         rather than defaulted so a kind added later is a compile error. */
      break;
  }
  return false;
}

/* Two expressions being compared child by child. */
typedef struct pair_frame
{ const mt_atom *a, *b;
  size_t         n, at;
} pair_frame;

typedef MT_STACK(pair_frame) pair_stack;

static bool pair_push(pair_stack *frames, const mt_atom *a, const mt_atom *b,
                      size_t n)
{ pair_frame frame;
  frame.a = a;
  frame.b = b;
  frame.n = n;
  frame.at = 0;
  return stack_push(frames, frame);
}

bool mt_eq(const mt_atom *a, const mt_atom *b)
{ pair_frame fixed[MT_WALK_FRAMES];
  pair_stack frames;
  pair_frame *f;
  bool equal = true;

  if ( a == b ) return true;
  if ( !a || !b || a->kind != b->kind || !eq_shallow(a, b) ) return false;
  if ( a->kind != MT_EXPR ) return true;

  stack_init(&frames, fixed);
  pair_push(&frames, a, b, a->u.e.n);

  while ( equal && (f = stack_top(&frames)) != NULL )
  { const mt_atom *x, *y;
    if ( f->at == f->n )
    { stack_pop(&frames);
      continue;
    }
    x = f->a->u.e.kids[f->at];
    y = f->b->u.e.kids[f->at];
    f->at++;
    if ( x == y ) continue;
    if ( !x || !y || x->kind != y->kind || !eq_shallow(x, y) )
    { equal = false;
      break;
    }
    /* `f` is not touched after this: the push may move the block. */
    if ( x->kind == MT_EXPR && x->u.e.n > 0 &&
         !pair_push(&frames, x, y, x->u.e.n) )
    { err_set(MT_NOMEM,
              "out of memory comparing two nested expressions; the answer "
              "below this point was not computed");
      equal = false;
    }
  }
  stack_free(&frames);
  return equal;
}

void *mt_value(const mt_atom *atom)
{ return ( atom && atom->kind == MT_OBJECT ) ? atom->u.box->value : NULL;
}

const char *mt_type(const mt_atom *atom)
{ return ( atom && atom->kind == MT_OBJECT ) ? atom->u.box->type : NULL;
}

static mt_atom *object_from_box(mt_box_t *box)
{ mt_atom *a = atom_alloc(MT_OBJECT);
  if ( !a )
  { box_release(box);
    return NULL;
  }
  a->u.box = box;
  return a;
}

static mt_box_t *box_new(void *value, const char *type_name,
                            mt_free_fn release,
                            mt_fn apply, void *user)
{ mt_box_t *box = calloc(1, sizeof(*box));
  if ( !box )
  { err_set(MT_NOMEM, "out of memory boxing a C value");
    return NULL;
  }
  box->refs = 1;
  box->value = value;
  box->release = release;
  box->apply = apply;
  box->user = user;
  if ( type_name && !(box->type = strdup(type_name)) )
  { free(box);
    err_set(MT_NOMEM, "out of memory copying a type name");
    return NULL;
  }
  return box;
}

mt_atom *mt_object(void *value, const char *type_name,
                           mt_free_fn release)
{ mt_box_t *box = box_new(value, type_name, release, NULL, NULL);
  return box ? object_from_box(box) : NULL;
}

mt_atom *mt_function(mt_fn fn, void *user,
                             mt_free_fn release)
{ mt_box_t *box;
  if ( !fn )
  { err_set(MT_MISUSE, "mt_function needs a function, not NULL");
    return NULL;
  }
  box = box_new(user, "Function", release, fn, user);
  return box ? object_from_box(box) : NULL;
}

/* ================================================================== *
 * The runtime
 * ================================================================== */

typedef struct mt_op_entry
{ char           *name;
  size_t          arity;
  mt_fn     fn;
  void           *user;
} mt_op_entry_t;

struct metta
{ bool              open;
  char             *path;
  bool              verbose;
  mt_limits    limits;
  mt_op_entry_t *ops;
  size_t            nops, cap_ops;
};

static struct metta g_runtime;
static bool         g_open = false;

struct mt_space
{ metta *runtime;
  char    *name;
  bool     borrowed;   /* &self and &metta live with the runtime */
};

/* The two spaces every runtime has. Their names are constants, so they are
   written here rather than in mt_open(): a handle whose name appears only
   once the engine booted is a handle with a window in which reading it is a
   null dereference. */
static mt_space g_self    = { &g_runtime, (char *)"&self",  true };
static mt_space g_catalog = { &g_runtime, (char *)"&metta", true };

/* Every door that reaches the engine asks this first. SWI's foreign-frame
   API reads the calling thread's Prolog environment, so a call made before
   mt_open(), or after mt_close(), does not fail: it SEGFAULTS inside
   PL_open_foreign_frame. Sixteen of cmetta.h's doors died that way, from
   mt_parse to mt_stats_now, and calling a door before the constructor is the
   most ordinary mistake a new caller makes [measured 2026-08-31,
   ai-tmp/cseat-probe-d3.c; C34 in ai-cmetta-c-constraints.md]. The header
   promises that every function that can fail says so, and a signal is not a
   way of saying so. */
static bool engine_ready(const char *door)
{ if ( g_open ) return true;
  err_set(MT_MISUSE,
          "%s needs a running engine: call mt_open() first, and note that "
          "mt_close() ends it", door);
  return false;
}

/* The same, for a door that also takes a handle. NULL is what a failed
   mt_open() or mt_space_open() hands back, so a caller who checked neither
   arrives here rather than at a null dereference. */
static bool handle_ready(const void *handle, const char *door)
{ if ( !handle )
  { err_set(MT_MISUSE,
            "%s was given NULL, which is what a failed mt_open() or "
            "mt_space_open() answers; check that before passing it on", door);
    return false;
  }
  return engine_ready(door);
}

/* --- blob type for a live C value --------------------------------- */

static int object_write(IOSTREAM *s, atom_t a, int flags)
{ mt_box_t *box = PL_blob_data(a, NULL, NULL);
  (void)flags;
  /* What it IS, never what it contains: a live value that printed its
     contents would have become text, which is the one thing it must not do. */
  Sfprintf(s, "<%s>", box->type ? box->type : "cvalue");
  return TRUE;
}

static int object_release_blob(atom_t a)
{ box_release(PL_blob_data(a, NULL, NULL));
  return TRUE;
}

static void object_acquire_blob(atom_t a)
{ mt_box_t *box = PL_blob_data(a, NULL, NULL);
  if ( box ) MT_INC(&box->refs);
}

static PL_blob_t mt_object_blob =
{ .magic   = PL_BLOB_MAGIC,
  .flags   = PL_BLOB_UNIQUE | PL_BLOB_NOCOPY,
  /* The SEAT's spelling, because bridge.pl names this type in blob/2 and the
     two must agree. Like the four foreign predicates above, it is a contract
     with the Prolog half rather than part of the C API's prefix. */
  .name    = "cmetta_object",
  .release = object_release_blob,
  .acquire = object_acquire_blob,
  .write   = object_write
};

/* ================================================================== *
 * Moving values across
 * ================================================================== */

/* A copy of the text behind a term, in UTF-8. SWI's own buffer is reused by
   the next conversion, so it is copied here and never held. */
/* The MARK/RELEASE pair is not optional here. PL_get_nchars() puts its result
   in a string buffer that SWI reclaims when a foreign predicate RETURNS, and
   this binding does not return: a C loop pulling answers stays inside one call
   for thousands of conversions. Without the pair, SWI dies with
   "FATAL ERROR: Too many stacked strings" once the ring fills
   [measured 2026-08-27, draining a bounded endless generator; tested:
   tests/test_cmetta.c, test_a_bound_stops_a_runaway_and_says_so;
   commit=4d20b8d80b2a8eb6fde434e561f30250a35fd3b3]. Releasing from
   the mark is safe because the text is copied out before the release. */
static char *term_text(term_t t, int cvt, size_t *len_out)
{ char *copy = NULL;

  PL_STRINGS_MARK()
  { char *s;
    size_t len;
    if ( PL_get_nchars(t, &len, &s, cvt | REP_UTF8 | BUF_DISCARDABLE) &&
         (copy = malloc(len + 1)) )
    { memcpy(copy, s, len);
      copy[len] = '\0';
      if ( len_out ) *len_out = len;
    }
  }
  PL_STRINGS_RELEASE()
  return copy;
}

static mt_status call_bridge(const char *name, int arity, term_t av);

/* Whether this atom is a space, asked of the engine and of the term itself:
   no text conversion, and no list of names to rebuild per answer.
   metta_c_space_operand/1 is metta_space_operand/1, the test the engine's own
   metatype_of/2 consults, so this seat, the Python seat and get-metatype
   classify one atom alike. Asking the engine per atom is a question only an
   in-process seat can afford, and it is the reason this seat exists.

   The predicate is a test over a bound atom and cannot throw, so a plain
   call is enough; a failure is the answer "no" rather than an error.

   The handle is resolved once. PL_predicate() interns the name and walks the
   module's procedure table on every call, and this runs once per decoded
   atom: caching it takes the question from 3,358 to 2,208 instructions per
   atom [measured 2026-08-27, perf stat -e instructions:u, minimum of three
   runs of kit/driver over 500 programs answering 40 symbols each:
   3,018,075,923 asking nothing, 3,085,234,884 resolving per call,
   3,062,228,470 resolving once, so 1,150 saved of 3,358 and +1.46% over
   asking nothing on a workload that is nothing but symbol decoding]. A static
   is safe because this binding is one runtime per process by construction
   (PL_initialise is process-wide, see cmetta.h's "Fails when") and a
   predicate_t stays valid for the life of that process. */
static bool is_space(term_t t)
{ static predicate_t space_operand = NULL;
  fid_t f;
  int rc;

  if ( !space_operand )
    space_operand = PL_predicate("metta_c_space_operand", 1, "user");
  if ( !(f = frame_open("decoding a symbol")) ) return false;
  rc = PL_call_predicate(NULL, PL_Q_NORMAL, space_operand, t);
  PL_discard_foreign_frame(f);
  return rc == TRUE;
}

/* The source name of a variable, from the engine's Name=Var pairs. */
static char *variable_name(term_t names, term_t var)
{ term_t head, tail, pair;
  if ( !names ) return term_text(var, CVT_WRITE, NULL);

  head = PL_new_term_ref();
  pair = PL_new_term_ref();
  tail = PL_copy_term_ref(names);
  while ( PL_get_list(tail, head, tail) )
  { term_t nm = PL_new_term_ref();
    term_t vr = PL_new_term_ref();
    /* Both Name=Var and Name-Var are read: the engine's name state uses one
       and a reader's variable_names uses the other. */
    if ( (PL_is_functor(head, PL_new_functor(PL_new_atom("="), 2)) ||
          PL_is_functor(head, PL_new_functor(PL_new_atom("-"), 2))) &&
         PL_get_arg(1, head, nm) && PL_get_arg(2, head, vr) &&
         PL_compare(vr, var) == 0 )
    { (void)pair;
      return term_text(nm, CVT_ATOM | CVT_STRING, NULL);
    }
  }
  return term_text(var, CVT_WRITE, NULL);
}

static mt_atom *decode_number(term_t t)
{ int64_t i;
  double d;

  if ( PL_is_integer(t) )
  { if ( PL_get_int64(t, &i) ) return mt_num(i);
    { size_t len;
      char *text = term_text(t, CVT_INTEGER, &len);
      mt_atom *a;
      if ( !text )
      { err_set(MT_NOMEM, "out of memory reading a wide integer");
        return NULL;
      }
      a = atom_text(MT_BIGINT, text, len);
      free(text);
      return a;
    }
  }
  if ( PL_is_float(t) )
  { if ( PL_get_float(t, &d) ) return mt_real(d);
    err_set(MT_UNSUPPORTED, "a float the C boundary cannot read");
    return NULL;
  }
  if ( PL_is_rational(t) )
  { fid_t f = frame_open("decoding a rational");
    term_t av;
    mt_atom *a = NULL;
    int64_t num, den;
    if ( !f ) return NULL;
    av = PL_new_term_refs(3);
    if ( av && PL_unify(av, t) &&
         call_bridge("metta_c_rational_parts", 3, av) == MT_OK &&
         PL_get_int64(av + 1, &num) && PL_get_int64(av + 2, &den) )
      a = mt_rational(num, den);
    else
      err_set(MT_UNSUPPORTED,
              "a rational whose halves do not fit int64_t; C has no type for "
              "it and rounding it would be a different number");
    PL_discard_foreign_frame(f);
    return a;
  }
  err_set(MT_UNSUPPORTED, "a number of no kind this binding reads");
  return NULL;
}

/* Whether a term has CHILDREN, which decode() walks with its own stack
   rather than by calling itself: see MT_STACK above and C35 in
   ai-cmetta-c-constraints.md. [] is a list in SWI 7 and later, and the empty
   expression is unit rather than a name, so it belongs here too. Asking
   before decode_leaf() rather than inside it keeps the answer out of an
   out-parameter, which is a store and a load per node on the hot path. A
   variable is neither nil nor a proper list, so testing this first reads the
   same as the branch order it replaced. */
static bool decode_is_expr(term_t t)
{ return PL_get_nil(t) || PL_is_list(t);
}

/* Every engine term with no children. */
static mt_atom *decode_leaf(term_t t, term_t names)
{ if ( PL_is_variable(t) )
  { char *name = variable_name(names, t);
    mt_atom *a;
    if ( !name )
    { err_set(MT_NOMEM, "out of memory naming a variable");
      return NULL;
    }
    a = atom_text(MT_VARIABLE, name, strlen(name));
    free(name);
    return a;
  }

  if ( PL_is_integer(t) || PL_is_float(t) || PL_is_rational(t) )
    return decode_number(t);

  if ( PL_is_string(t) )
  { size_t len;
    char *text = term_text(t, CVT_STRING, &len);
    mt_atom *a;
    if ( !text )
    { err_set(MT_NOMEM, "out of memory reading a string");
      return NULL;
    }
    a = atom_text(MT_TEXT, text, len);
    free(text);
    return a;
  }

  /* Before the atom branch, and it has to be: every SWI atom is a blob
     underneath, but PL_is_atom() is FALSE for a blob whose type does not
     carry PL_BLOB_TEXT, so a native value asked about that way is neither an
     atom nor anything else and falls off the end
     [measured 2026-08-27: a mt_object reached the refusal branch and the
     dispatcher answered "No permission to read argument `<counter>'";
     tested: tests/test_cmetta.c, test_a_c_value_crosses_by_reference;
     commit=4d20b8d80b2a8eb6fde434e561f30250a35fd3b3].
     The PL_BLOB_TEXT mask is the other half: without it an ordinary symbol
     reads as a native value instead. */
  { void *blob;
    PL_blob_t *type;
    if ( PL_get_blob(t, &blob, NULL, &type) && !(type->flags & PL_BLOB_TEXT) )
    { size_t len;
      char *text;
      mt_atom *a;
      if ( type == &mt_object_blob )
      { mt_box_t *box = blob;
        MT_INC(&box->refs);
        return object_from_box(box);
      }
      /* Somebody else's blob: a native engine value. It crosses by reference
         and prints as itself, which is the `h` tag's whole contract. */
      text = term_text(t, CVT_WRITE, &len);
      if ( !text )
      { err_set(MT_NOMEM, "out of memory naming a native value");
        return NULL;
      }
      a = atom_text(MT_HANDLE, text, len);
      free(text);
      return a;
    }
  }

  if ( PL_is_atom(t) )
  { size_t len;
    char *text;
    mt_atom *a;

    if ( !(text = term_text(t, CVT_ATOM, &len)) )
    { err_set(MT_NOMEM, "out of memory reading a symbol");
      return NULL;
    }
    if ( strcmp(text, "true") == 0 || strcmp(text, "false") == 0 )
    { a = mt_bool(text[0] == 't');
      free(text);
      return a;
    }
    a = atom_text(is_space(t) ? MT_SPACE : MT_SYMBOL,
                  text, len);
    free(text);
    return a;
  }

  { size_t len;
    char *text = term_text(t, CVT_WRITE, &len);
    err_set(MT_UNSUPPORTED,
            "the engine answered %s, which is a Prolog term with no MeTTa "
            "reading; this binding refuses it rather than turning it into a "
            "symbol that cannot go home again",
            text ? text : "a term this binding could not even print");
    free(text);
    return NULL;
  }
}

/* One expression being built: the children taken so far, and how far along
   the engine's list this level has walked. The tail needs a term reference
   per level, which SWI allocates on the Prolog stacks and bounds by
   stack_limit, so depth costs a resource error rather than a signal. */
typedef struct decode_frame
{ mt_atom **kids;
  size_t    n, cap;
  term_t    tail;
} decode_frame;

typedef MT_STACK(decode_frame) decode_stack;

static bool decode_frame_push(decode_stack *frames, term_t list)
{ decode_frame frame;
  frame.kids = NULL;
  frame.n = frame.cap = 0;
  /* Beyond the ten a foreign frame guarantees, PL_new_term_ref() can answer
     0 with a resource exception scheduled, so it is checked
     [source: SWI-Prolog manual, PL_open_foreign_frame: "On success, the stack
     has room for at least 10 term_t handles"]. */
  frame.tail = PL_copy_term_ref(list);
  return frame.tail != 0 && stack_push(frames, frame);
}

static inline bool decode_frame_add(decode_frame *f, mt_atom *kid)
{ if ( f->n == f->cap )
  { size_t cap = f->cap ? f->cap * 2 : 4;
    mt_atom **grown = realloc(f->kids, cap * sizeof(*grown));
    if ( !grown ) return false;
    f->kids = grown;
    f->cap = cap;
  }
  f->kids[f->n++] = kid;
  return true;
}

/* An engine term as a C atom. A leaf answers at once; an expression is walked
   with a stack of levels, so a term nested deeper than the C stack can hold
   decodes rather than killing the process. */
static mt_atom *decode(term_t t, term_t names)
{ decode_frame fixed[MT_WALK_FRAMES];
  decode_stack frames;
  decode_frame *f;
  mt_atom *value = NULL;
  term_t head;

  if ( !decode_is_expr(t) ) return decode_leaf(t, names);

  stack_init(&frames, fixed);
  head = PL_new_term_ref();
  if ( !head || !decode_frame_push(&frames, t) )
    err_set(MT_NOMEM, "out of memory decoding an expression");

  /* `f` is read out of the stack only where it can have MOVED, which is a
     push or a pop, so the children of one level are taken in a loop that
     keeps the level in a register. */
  while ( (f = stack_top(&frames)) != NULL )
  { mt_atom *kid;

    if ( PL_get_list(f->tail, head, f->tail) )
    { if ( decode_is_expr(head) )
      { /* Descend. `f` is not touched afterwards: the push may move it. */
        if ( decode_frame_push(&frames, head) ) continue;
        err_set(MT_NOMEM, "out of memory decoding an expression");
        break;
      }
      if ( !(kid = decode_leaf(head, names)) ) break;   /* it said why */
      if ( !decode_frame_add(f, kid) )
      { mt_drop(kid);
        err_set(MT_NOMEM, "out of memory decoding an expression");
        break;
      }
      continue;
    }

    if ( !PL_get_nil(f->tail) )
    { err_set(MT_UNSUPPORTED,
              "a partial list is not a MeTTa expression; the engine handed "
              "back a term with an unbound or non-list tail");
      break;
    }

    /* This level is complete: mt_exprv steals the children, the array is
       this walk's to free, and the result becomes a child of the level
       above or the answer itself. */
    kid = mt_exprv(f->n, f->kids);
    free(f->kids);
    stack_pop(&frames);
    if ( !kid ) break;
    if ( !(f = stack_top(&frames)) )
    { value = kid;
      break;
    }
    if ( !decode_frame_add(f, kid) )
    { mt_drop(kid);
      err_set(MT_NOMEM, "out of memory decoding an expression");
      break;
    }
  }

  /* Whatever is still open was abandoned by a failure above. */
  while ( (f = stack_top(&frames)) != NULL )
  { size_t i;
    for (i = 0; i < f->n; i++) mt_drop(f->kids[i]);
    free(f->kids);
    stack_pop(&frames);
  }
  stack_free(&frames);
  return value;
}

/* --- the other direction ------------------------------------------ */

/* Variables bound while encoding one term, so two occurrences of $x are one
   variable, which is what makes (f $x $x) different from (f $x $y). */
typedef struct encode_ctx
{ char   **names;
  term_t  *vars;
  size_t   n, cap;
} encode_ctx;

static void encode_ctx_free(encode_ctx *ctx)
{ size_t i;
  for (i = 0; i < ctx->n; i++) free(ctx->names[i]);
  free(ctx->names);
  free(ctx->vars);
}

static bool encode_var(encode_ctx *ctx, const char *name, term_t out)
{ size_t i;
  /* `_` is fresh at every occurrence and never recorded, exactly as $_ is in
     source, so two of them constrain nothing. */
  if ( strcmp(name, "_") != 0 )
  { for (i = 0; i < ctx->n; i++)
      if ( strcmp(ctx->names[i], name) == 0 )
        return PL_put_term(out, ctx->vars[i]);
  }
  if ( !PL_put_variable(out) ) return false;
  if ( strcmp(name, "_") == 0 ) return true;

  if ( ctx->n == ctx->cap )
  { size_t cap = ctx->cap ? ctx->cap * 2 : 4;
    char **nn = realloc(ctx->names, cap * sizeof(*nn));
    term_t *vv = realloc(ctx->vars, cap * sizeof(*vv));
    if ( nn ) ctx->names = nn;
    if ( vv ) ctx->vars = vv;
    if ( !nn || !vv ) return false;
    ctx->cap = cap;
  }
  if ( !(ctx->names[ctx->n] = strdup(name)) ) return false;
  ctx->vars[ctx->n] = PL_copy_term_ref(out);
  ctx->n++;
  return true;
}

/* Every atom with no children. An expression is encode()'s own business,
   because a list is built bottom up and that is where the walk lives. */
static bool encode_leaf(const mt_atom *a, term_t out, encode_ctx *ctx)
{ if ( !a )
  { err_set(MT_MISUSE, "cannot encode a NULL atom");
    return false;
  }
  switch ( a->kind )
  { case MT_SYMBOL:
    case MT_SPACE:
      return PL_put_atom_nchars(out, a->u.t.len, a->u.t.text);
    case MT_TEXT:
      return PL_put_string_nchars(out, a->u.t.len, a->u.t.text);
    case MT_VARIABLE:
      return encode_var(ctx, a->u.t.text, out);
    case MT_INT:
      return PL_put_int64(out, a->u.i);
    case MT_FLOAT:
      return PL_put_float(out, a->u.f);
    case MT_BOOL:
      return PL_put_atom_chars(out, a->u.b ? "true" : "false");
    case MT_BIGINT:
      return PL_put_term_from_chars(out, REP_UTF8, a->u.t.len, a->u.t.text);
    case MT_RATIONAL:
    { char buf[64];
      int n = snprintf(buf, sizeof(buf), "%lldr%lld",
                       (long long)a->u.r.num, (long long)a->u.r.den);
      return n > 0 && PL_put_term_from_chars(out, REP_UTF8, (size_t)n, buf);
    }
    case MT_OBJECT:
      /* PL_put_blob's result reports whether SWI created a blob atom; it is
         not a success flag. The acquire callback pairs one box reference with
         each created blob and an existing unique blob needs no second one.
         [tested: tests/test_cmetta.c,
         test_the_same_c_object_is_one_engine_identity; commit=WORKTREE] */
      (void)PL_put_blob(out, a->u.box, sizeof(*a->u.box), &mt_object_blob);
      return true;
    case MT_HANDLE:
      err_set(MT_UNSUPPORTED,
              "a native engine value cannot be sent back by its printed form: "
              "%s names it but is not it. Keep the answer's own atom and pass "
              "that instead", a->u.t.text);
      return false;
    case MT_NONE:
      err_set(MT_MISUSE, "cannot encode a NULL atom");
      return false;
    case MT_EXPR:
      /* Unreachable: encode() takes an expression itself. Named rather than
         defaulted so a kind added later is a compile error here. */
      break;
  }
  err_set(MT_MISUSE, "an atom of no kind this binding writes");
  return false;
}

/* One expression being written: the children still to place, counted DOWN
   because a Prolog list is built from its tail, and the list built so far. */
typedef struct encode_frame
{ const mt_atom *a;
  size_t         left;
  term_t         list;
} encode_frame;

typedef MT_STACK(encode_frame) encode_stack;

static bool encode_frame_push(encode_stack *frames, const mt_atom *a)
{ encode_frame frame;
  frame.a = a;
  frame.left = a->u.e.n;
  frame.list = PL_new_term_ref();
  return frame.list != 0 && PL_put_nil(frame.list) &&
         stack_push(frames, frame);
}

/* An atom as an engine term. One term reference per level of nesting, and no
   C stack frame per level: an expression a caller can build is an expression
   this has to be able to write. */
static bool encode(const mt_atom *a, term_t out, encode_ctx *ctx)
{ encode_frame fixed[MT_WALK_FRAMES];
  encode_stack frames;
  encode_frame *f;
  term_t item;
  bool ok;

  if ( !a || a->kind != MT_EXPR ) return encode_leaf(a, out, ctx);

  stack_init(&frames, fixed);
  item = PL_new_term_ref();
  ok = item != 0 && encode_frame_push(&frames, a);
  if ( !ok ) err_set(MT_NOMEM, "out of memory writing an expression");

  while ( ok && (f = stack_top(&frames)) != NULL )
  { const mt_atom *kid;

    if ( f->left == 0 )
    { /* This level is complete: it becomes the head of the level above, or
         the answer. `done` is a scalar, so popping does not disturb it. */
      term_t done = f->list;
      stack_pop(&frames);
      f = stack_top(&frames);
      ok = f ? PL_cons_list(f->list, done, f->list) : PL_put_term(out, done);
      continue;
    }

    kid = f->a->u.e.kids[--f->left];
    if ( kid && kid->kind == MT_EXPR )
    { /* Descend. `f` is not touched afterwards: the push may move it. */
      if ( !(ok = encode_frame_push(&frames, kid)) )
        err_set(MT_NOMEM, "out of memory writing an expression");
      continue;
    }
    ok = encode_leaf(kid, item, ctx) && PL_cons_list(f->list, item, f->list);
  }
  stack_free(&frames);
  return ok;
}

/* encode()'s own refusals record their reason; a PL_put_* that fails does
   not, so the one silent path is given words here. A caller that cleared the
   error state before this can then read mt_errmsg() and find the truth
   rather than a stale sentence. */
static bool put_atom(const mt_atom *a, term_t out)
{ encode_ctx ctx = {0};
  bool ok = encode(a, out, &ctx);
  encode_ctx_free(&ctx);
  if ( !ok && mt_ok() )
    err_set(MT_NOMEM, "the engine's stacks could not hold the term written");
  return ok;
}

/* The same, plus the Name-Var pairs the encode collected, which is what the
   engine's writer needs to print $x as $x rather than $_0. The list is built
   in the caller's frame and stays valid as long as `out` does. */
static bool put_atom_named(const mt_atom *a, term_t out, term_t names)
{ encode_ctx ctx = {0};
  bool ok = encode(a, out, &ctx);
  size_t i;

  if ( ok ) ok = PL_put_nil(names);
  for (i = ctx.n; ok && i > 0; i--)
  { term_t pair = PL_new_term_ref();
    term_t name = PL_new_term_ref();
    ok = PL_put_atom_chars(name, ctx.names[i - 1]) &&
         PL_cons_functor(pair, PL_new_functor(PL_new_atom("-"), 2),
                         name, ctx.vars[i - 1]) &&
         PL_cons_list(names, pair, names);
  }
  encode_ctx_free(&ctx);
  return ok;
}

/* ================================================================== *
 * Calling the bridge
 * ================================================================== */

/* Render a pending ball into the thread-local error text, through the bridge,
   which asks SWI to print the message exactly as the console would have. */
static void render_ball(term_t ball)
{ fid_t f = frame_open("rendering an engine error");
  term_t av;
  predicate_t p = PL_predicate("metta_c_error_text", 2, "user");
  qid_t q;

  if ( !f ) return;   /* frame_open() recorded why, which is all there is */
  av = PL_new_term_refs(2);

  if ( av && PL_unify(av, ball) &&
       (q = PL_open_query(NULL, PL_Q_CATCH_EXCEPTION, p, av)) )
  { if ( PL_next_solution(q) == TRUE )
    { char *text = term_text(av + 1, CVT_ATOM | CVT_STRING, NULL);
      if ( text )
      { err_copy(MT_ERROR, text);
        free(text);
        PL_cut_query(q);
        PL_discard_foreign_frame(f);
        return;
      }
    }
    PL_cut_query(q);
  }
  { char *text = term_text(ball, CVT_WRITE, NULL);
    err_copy(MT_ERROR,
             text ? text : "the engine raised a term this binding could not print");
    free(text);
  }
  PL_discard_foreign_frame(f);
}

/* Whether a caught ball is one of this binding's own bounds rather than a
   fault. A caller wants to tell "I stopped it" from "it broke", and those need
   different answers even though both arrive as exceptions. */
static bool ball_is_limit(term_t ball)
{ fid_t f = frame_open("classifying an engine error");
  term_t av;
  predicate_t p = PL_predicate("metta_c_limit_ball", 3, "user");
  qid_t q;
  bool yes = false;

  if ( !f ) return false;
  av = PL_new_term_refs(3);

  if ( av && PL_unify(av, ball) &&
       (q = PL_open_query(NULL, PL_Q_CATCH_EXCEPTION, p, av)) )
  { yes = PL_next_solution(q) == TRUE;
    PL_cut_query(q);
  }
  PL_discard_foreign_frame(f);
  return yes;
}

/* A ball copied off the stacks, read back and classified. It is a function of
   its own rather than a branch inside call_bridge because call_bridge's HOT
   path is a query that ANSWERS: written inline, the extra scope grew that
   function from 225 to 262 instructions and cost 72 instructions per cursor
   step, which is two of these calls [measured 2026-08-31, cursor-step,
   perf stat -e instructions:u, min of 3]. The record is erased here, so the
   caller hands it over and forgets it. */
static mt_status ball_status(record_t saved, const char *name, int arity)
{ fid_t f = frame_open("reading an engine error");
  mt_status kind = MT_ERROR;

  if ( f )
  { term_t ball = PL_new_term_ref();
    if ( ball && PL_recorded(saved, ball) )
    { render_ball(ball);
      if ( ball_is_limit(ball) ) kind = MT_LIMIT;
      /* render_ball() records the words under MT_ERROR, because that is all
         it can know. The classification happens here, and the STICKY status
         is what mt_error() reads, so it has to carry the refined answer
         rather than the one the renderer left behind. */
      err_reclassify(kind);
    }
    else err_set(MT_ERROR, "%s/%d raised a term that could not be read back",
                 name, arity);
    PL_discard_foreign_frame(f);
  }
  PL_erase(saved);
  return kind;
}

/* Call a bridge predicate for its first solution, KEEPING its bindings, so the
   caller can read the output arguments out of av. The caller owns the frame. */
static mt_status call_bridge(const char *name, int arity, term_t av)
{ predicate_t p = PL_predicate(name, arity, "user");
  qid_t q = PL_open_query(NULL, PL_Q_CATCH_EXCEPTION, p, av);
  int rc;
  mt_status status;

  if ( !q )
    return err_set(MT_NOMEM, "could not open a query for %s/%d", name, arity);

  rc = PL_next_solution(q);
  if ( rc == PL_S_EXCEPTION || rc == FALSE )
  { term_t ex = PL_exception(q);
    if ( ex )
    { /* The ball has to survive the cut, and a term_t cannot: cutting rewinds
         the term stack to the query's own mark, so a reference taken after
         PL_open_query is gone by the time it would be read
         [measured 2026-08-27: SWI answered "API error: invalid term_t 77
         (out of range)"]. PL_record copies it off the stacks entirely, which
         is what the record database is for. */
      record_t saved = PL_record(ex);
      PL_cut_query(q);
      PL_clear_exception();
      if ( saved ) return ball_status(saved, name, arity);
      err_set(MT_ERROR, "%s/%d raised, and the ball could not be copied "
              "out of the query to be read", name, arity);
      return MT_ERROR;
    }
    PL_cut_query(q);
    return err_set(MT_FAIL, "%s/%d had no answer", name, arity);
  }
  status = MT_OK;
  /* Cut rather than close: close undoes the bindings the caller is about to
     read [source: SWI-Prolog manual, PL_cut_query vs PL_close_query]. */
  PL_cut_query(q);
  return status;
}

/* ================================================================== *
 * Foreign predicates the bridge calls back into
 * ================================================================== */

static foreign_t pl_cmetta_present(void)
{ return TRUE;
}

struct mt_call
{ metta            *runtime;
  const mt_atom **args;
  size_t              arity;
  mt_atom       *result;
  bool                answered;
  char                error[MT_ERR_MAX];
  bool                failed;
};

size_t mt_arity(const mt_call *call)
{ return call->arity;
}

const mt_atom *mt_arg(const mt_call *call, size_t index)
{ return index < call->arity ? call->args[index] : NULL;
}

metta *mt_of(const mt_call *call)
{ return call->runtime;
}

mt_status mt_answer(mt_call *call, mt_atom *atom)
{ if ( call->answered )
  { mt_drop(atom);
    return err_set(MT_MISUSE,
                   "this application already answered; a function that has "
                   "many answers returns one expression and lets superpose "
                   "spread it");
  }
  if ( !atom ) return err_set(MT_MISUSE, "cannot answer with a NULL atom");
  call->result = atom;
  call->answered = true;
  return MT_OK;
}

/* Returns MT_ERROR so an op can spell its refusal as one line:
       if ( !mt_ok() ) return mt_fail(call, "wanted two numbers"); */
mt_status mt_fail(mt_call *call, const char *message)
{ snprintf(call->error, sizeof(call->error), "%s",
           message ? message : "the C function refused this application");
  call->failed = true;
  return MT_ERROR;
}

/* Run one C function against decoded arguments and unify its answer. Shared by
   a named operation and an applied function value. */
static foreign_t run_call(const char *name, mt_fn fn, void *user,
                          term_t args, term_t result)
{ struct mt_call call;
  mt_atom **decoded = NULL;
  size_t n = 0, cap = 4, i;
  term_t head = PL_new_term_ref();
  term_t tail = PL_copy_term_ref(args);
  mt_status status;
  uint64_t error_before;
  foreign_t rc = FALSE;

  memset(&call, 0, sizeof(call));
  if ( !(decoded = malloc(cap * sizeof(*decoded))) )
    return PL_resource_error("memory");

  while ( PL_get_list(tail, head, tail) )
  { mt_atom *a = decode(head, 0);
    if ( !a )
    { rc = PL_permission_error("read", "argument", head);
      goto done;
    }
    if ( n == cap )
    { mt_atom **grown = realloc(decoded, (cap *= 2) * sizeof(*decoded));
      if ( !grown ) { mt_drop(a); rc = PL_resource_error("memory"); goto done; }
      decoded = grown;
    }
    decoded[n++] = a;
  }

  call.runtime = &g_runtime;
  call.args = (const mt_atom **)decoded;
  call.arity = n;

  error_before = g_error_generation;
  status = fn(&call, user);

  if ( status == MT_OK && call.answered )
  { term_t out = PL_new_term_ref();
    rc = put_atom(call.result, out) && PL_unify(result, out);
  } else if ( status == MT_FAIL )
  { rc = FALSE;
  } else
  { /* An ISO error(Formal, Context) pair rather than a bare term, so SWI's own
       machinery carries it and bridge.pl's prolog:message//1 renders it. A
       bare mt_error(...) printed as "Unknown message: ..."
       [measured 2026-08-27]. */
    const char *why = call.failed ? call.error
                    : (g_error_generation != error_before && mt_errmsg()
                    ? mt_errmsg()
                    : "the C function answered nothing");
    term_t ball = PL_new_term_ref();
    if ( PL_unify_term(ball,
                       PL_FUNCTOR_CHARS, "error", 2,
                         PL_FUNCTOR_CHARS, "cmetta_operation_failed", 2,
                           PL_UTF8_CHARS, name,
                           PL_UTF8_CHARS, why,
                         PL_FUNCTOR_CHARS, "context", 2,
                           PL_UTF8_CHARS, name,
                           /* The context's second half is SWI's own trailing
                              "(...)" note. Repeating the reason there prints
                              it twice, so it is left unbound. */
                           PL_VARIABLE) )
      PL_raise_exception(ball);
    rc = FALSE;
  }

done:
  mt_drop(call.result);
  for (i = 0; i < n; i++) mt_drop(decoded[i]);
  free(decoded);
  return rc;
}

static mt_op_entry_t *find_op(const char *name, size_t arity)
{ size_t i;
  for (i = 0; i < g_runtime.nops; i++)
    if ( g_runtime.ops[i].arity == arity &&
         strcmp(g_runtime.ops[i].name, name) == 0 )
      return &g_runtime.ops[i];
  return NULL;
}

static foreign_t pl_cmetta_dispatch(term_t name, term_t args, term_t result)
{ char *text;
  size_t len;
  mt_op_entry_t *op;
  size_t arity = 0;
  foreign_t rc;

  if ( PL_skip_list(args, 0, &arity) != PL_LIST )
    return PL_type_error("list", args);
  if ( !(text = term_text(name, CVT_ATOM | CVT_STRING, &len)) )
    return PL_type_error("atom", name);

  op = find_op(text, arity);
  if ( !op )
  { rc = PL_existence_error("cmetta_operation", name);
    free(text);
    return rc;
  }
  { foreign_t answered = run_call(op->name, op->fn, op->user, args, result);
    free(text);
    return answered;
  }
}

static mt_box_t *blob_box(term_t t)
{ void *blob;
  PL_blob_t *type;
  if ( PL_get_blob(t, &blob, NULL, &type) && type == &mt_object_blob )
    return blob;
  return NULL;
}

static foreign_t pl_cmetta_object_callable(term_t t)
{ mt_box_t *box = blob_box(t);
  return ( box && box->apply ) ? TRUE : FALSE;
}

static foreign_t pl_cmetta_apply(term_t t, term_t args, term_t result)
{ mt_box_t *box = blob_box(t);
  if ( !box || !box->apply ) return FALSE;
  return run_call(box->type ? box->type : "function",
                  box->apply, box->user, args, result);
}

/* ================================================================== *
 * Boot
 * ================================================================== */

static bool goal(const char *text)
{ fid_t f = frame_open("running an engine goal");
  term_t t;
  bool ok;

  if ( !f ) return false;
  t = PL_new_term_ref();
  ok = t && PL_chars_to_term(text, t) && PL_call(t, NULL);
  PL_discard_foreign_frame(f);
  return ok;
}

static char *default_path(void)
{ const char *env = getenv("METTA_PATH");
  return strdup(env && *env ? env : MT_ENGINE_PATH);
}

metta *mt_open(const mt_config *config)
{ static char *argv[] = { (char *)"cmetta", (char *)"-q",
                          (char *)"--no-signals", NULL };
  mt_config defaults = {0};
  char *path;
  char *buf;
  size_t bufsz;

  if ( !config ) config = &defaults;

  if ( g_open )
  { /* One runtime per process; PL_initialise sets up the process's single
       Prolog heap and there is no second one to hand out. */
    if ( config->path && strcmp(config->path, g_runtime.path) != 0 )
      return err_null(MT_MISUSE,
                      "the engine was booted from %s and cannot be reopened "
                      "from %s: this process holds one runtime",
                      g_runtime.path, config->path);
    return &g_runtime;
  }

  path = config->path ? strdup(config->path) : default_path();
  if ( !path ) return err_null(MT_NOMEM, "out of memory recording the path");

  if ( !PL_is_initialised(NULL, NULL) && !PL_initialise(3, argv) )
  { free(path);
    return err_null(MT_ERROR, "SWI-Prolog would not initialise");
  }

  /* Registered BEFORE the consult, because engine/metta.pl reads
     extensions/ * /extension.pl while it loads and this seat's control file
     declares needs(predicate('$mt_present'/0)). */
  PL_register_foreign("$cmetta_present", 0,
                      as_pl_function((mt_anyfn)pl_cmetta_present), 0);
  PL_register_foreign("$cmetta_dispatch", 3,
                      as_pl_function((mt_anyfn)pl_cmetta_dispatch), 0);
  PL_register_foreign("$cmetta_object_callable", 1,
                      as_pl_function((mt_anyfn)pl_cmetta_object_callable), 0);
  PL_register_foreign("$cmetta_apply", 3,
                      as_pl_function((mt_anyfn)pl_cmetta_apply), 0);
  PL_register_blob_type(&mt_object_blob);

  bufsz = strlen(path) + 128;
  if ( !(buf = malloc(bufsz)) )
  { free(path);
    return err_null(MT_NOMEM, "out of memory building the boot goals");
  }

  if ( config->stack_limit )
  { snprintf(buf, bufsz, "set_prolog_flag(stack_limit, %zu)",
             config->stack_limit);
    if ( !goal(buf) )
    { free(path); free(buf);
      return err_null(MT_ERROR, "the stack limit %zu was refused",
                     config->stack_limit);
    }
  }

  /* `extensions` opts the engine into reading extensions/ * /extension.pl,
     and `silent` is how a host with no command line asks for quiet, because
     engine/filereader.pl reads argv at load time [C2]. */
  if ( !goal(config->verbose ? "set_prolog_flag(argv, [extensions])"
                             : "set_prolog_flag(argv, [silent, extensions])") )
  { free(path); free(buf);
    return err_null(MT_ERROR, "the engine refused its argv");
  }

  /* The purge FIRST, and this seat is the one that has to ask for it. A host
     consulting engine/main.pl gets it, which is what the Python seat does;
     this seat cannot, because main.pl's initialization(main, main) fires on
     consult and prints its demo into a host's output. The engine's units are
     consulted by umbrellas, so engine/spaces/foreign.pl compiles into
     engine/spaces.qlf and SWI's staleness check, which compares an artifact
     against its immediate source, never sees a unit edit: without this a C
     program runs the previous compile and nothing says so
     [tested: tests/checks/check_qlf_freshness.py; commit=888a73c7d231188cd90fafcb8b0cce3799ef5e97].

     It also makes the boot CHEAPER, which is not why it is here but is most of
     what it does to the counters. qlf_boot sets encoding(utf8), and this seat
     consults the engine with an explicit .pl so the umbrella is read from
     SOURCE; without the flag that read goes through the locale's multibyte
     conversion. Boot measures 1,961,762,311 retired instructions with neither,
     1,735,405,359 with the encoding flag alone and 1,761,830,644 with
     qlf_boot, so the flag is worth -11.5% and the purge machinery costs about
     26M of it back, plus 6,955 inferences for globbing the artifact set and
     reading its stamp [measured 2026-08-29, min-of-three per arm, one arm per
     mechanism]. The purge is what makes this correct and the encoding comes
     with it; neither is worth having alone.

     Concurrent opens are safe, which is the question deleting files at boot
     invites. Six hello processes started at once against a STALE artifact set,
     so all six purge and regenerate together, all answered correctly; qlf_boot
     publishes its stamp through a temporary file and rename/2 for that reason,
     and the Python seat has run the same shape through main.pl all along
     [measured 2026-08-29: 6/6, examples/hello, set made stale by touching
     engine/spaces/foreign.pl; commit=888a73c7d231188cd90fafcb8b0cce3799ef5e97]. */
  snprintf(buf, bufsz, "consult('%s/engine/qlf_boot.pl')", path);
  if ( !goal(buf) )
  { void *refused =
      err_null(MT_ERROR,
              "the engine's artifact freshness check would not load from %s; "
              "set config.path or METTA_PATH to the tree holding engine/",
              path);
    free(path); free(buf);
    return refused;
  }

  snprintf(buf, bufsz, "consult('%s/engine/metta.pl')", path);
  if ( !goal(buf) )
  { void *refused =
      err_null(MT_ERROR,
              "the engine would not load from %s; set config.path or "
              "METTA_PATH to the tree holding engine/, lib/ and "
              "extensions/", path);
    free(path); free(buf);
    return refused;
  }
  free(buf);

  g_runtime.open = true;
  g_runtime.path = path;
  g_runtime.verbose = config->verbose;
  g_open = true;

  mt_verbose(&g_runtime, config->verbose);
  return &g_runtime;
}

/* Defined with the show ring below; declared here because mt_close comes
   first in the file and both lifecycle exits release it. */
static void show_ring_release(void);

void mt_close(metta *runtime)
{ size_t i;
  if ( !runtime || !g_open ) return;
  for (i = 0; i < runtime->nops; i++) free(runtime->ops[i].name);
  free(runtime->ops);
  runtime->ops = NULL;
  runtime->nops = runtime->cap_ops = 0;
  free(runtime->path);
  runtime->path = NULL;
  runtime->open = false;
  g_open = false;
  show_ring_release();
  PL_cleanup(0);
}

bool mt_verbose(metta *runtime, bool verbose)
{ bool was;
  fid_t f;
  term_t av;

  if ( !handle_ready(runtime, "mt_verbose") ) return false;
  was = runtime->verbose;
  if ( !(f = frame_open("mt_verbose")) ) return was;
  av = PL_new_term_refs(1);
  /* The engine's own door, not a bridge predicate: bridge.pl carried a
     private copy of the engine's retract-then-assert until C2 was taken
     engine-side as metta_host_set_silent/1. filereader.pl exports it, so
     it resolves in `user` the way every other engine predicate this file
     reaches does. */
  if ( av && PL_put_atom_chars(av, verbose ? "false" : "true") &&
       call_bridge("metta_host_set_silent", 1, av) == MT_OK )
    runtime->verbose = verbose;
  PL_discard_foreign_frame(f);
  return was;
}

/* No runtime argument: there is one per process, so passing it said nothing. */
bool mt_thread_attach(void)
{ if ( !engine_ready("mt_thread_attach") ) return false;
  if ( PL_thread_attach_engine(NULL) < 0 )
  { err_set(MT_ERROR, "this thread could not attach a Prolog engine");
    return false;
  }
  return true;
}

/* A release door, so it is a no-op rather than a refusal when there is
   nothing to release: mt_close() and mt_answers_free() take the same line,
   and a cleanup path that sets an error the caller then reads is a nuisance
   with no remedy behind it. The show ring is C memory and goes either way. */
void mt_thread_detach(void)
{ show_ring_release();
  if ( g_open ) PL_thread_destroy_engine();
}

/* ================================================================== *
 * Text
 * ================================================================== */

/* No runtime argument on the text doors either: they need the ENGINE, and
   there is one of those per process. Threading a handle through them was
   ceremony that never chose anything. */
static mt_atom *parse_n(const char *source, size_t length, const char *door)
{ fid_t f;
  term_t av;
  mt_atom *out = NULL;

  if ( !engine_ready(door) ) return NULL;

  if ( !(f = frame_open(door)) ) return NULL;
  av = PL_new_term_refs(3);
  if ( !av || !PL_put_string_nchars(av, length, source) )
  { PL_discard_foreign_frame(f);
    return err_null(MT_NOMEM, "out of memory holding the source");
  }
  if ( call_bridge("metta_c_read", 3, av) == MT_OK )
    out = decode(av + 1, av + 2);
  PL_discard_foreign_frame(f);
  return out;
}

mt_atom *mt_parse(const char *source)
{ if ( !source ) return err_null(MT_MISUSE, "mt_parse needs source text");
  return parse_n(source, strlen(source), "mt_parse");
}

mt_atom *mt_parsen(const char *source, size_t length)
{ if ( !source ) return err_null(MT_MISUSE, "mt_parsen needs source text");
  return parse_n(source, length, "mt_parsen");
}

char *mt_show_dup(const mt_atom *atom)
{ fid_t f;
  term_t av;
  char *text = NULL;

  if ( !engine_ready("mt_show_dup") ) return NULL;
  if ( !(f = frame_open("mt_show_dup")) ) return NULL;
  av = PL_new_term_refs(3);
  if ( av && put_atom_named(atom, av, av + 1) &&
       call_bridge("metta_c_show", 3, av) == MT_OK &&
       !(text = term_text(av + 2, CVT_ATOM | CVT_STRING, NULL)) )
    err_set(MT_NOMEM, "out of memory copying the engine's rendering");
  PL_discard_foreign_frame(f);
  return text;
}

mt_string mt_write_dup(const mt_atom *atom)
{ fid_t f;
  term_t av;
  mt_string text = { NULL, 0 };

  if ( !engine_ready("mt_write_dup") ) return text;
  if ( !(f = frame_open("mt_write_dup")) ) return text;
  av = PL_new_term_refs(3);
  if ( av && put_atom_named(atom, av, av + 1) &&
       call_bridge("metta_c_write_atom", 3, av) == MT_OK &&
       !(text.data = term_text(av + 2, CVT_ATOM | CVT_STRING, &text.len)) )
    err_set(MT_NOMEM, "out of memory copying the engine's written form");
  PL_discard_foreign_frame(f);
  return text;
}

/* A rotating per-thread buffer, so the common use needs no free:

       printf("%s -> %s\n", mt_show(pattern), mt_show(answer));

   strerror(), inet_ntoa() and ctime() all hand back storage they own on the
   same terms. The ring is MT_SHOW_SLOTS deep rather than one slot deep so
   several renderings can be live in one printf, which one slot would not
   survive. */
static MT_TLS char *g_show[MT_SHOW_SLOTS];
static MT_TLS unsigned g_show_at;

const char *mt_show(const mt_atom *atom)
{ char *text = mt_show_dup(atom);
  unsigned slot = g_show_at++ % MT_SHOW_SLOTS;

  free(g_show[slot]);
  g_show[slot] = text;
  return text ? text : "<unwritable>";
}

/* The ring is bounded, so leaving it allocated would be harmless the way
   strerror()'s buffer is. It is released anyway, at both points where this
   thread says it is done with the engine, so a leak report has nothing of
   this binding's in it at all rather than a small amount to explain. */
static void show_ring_release(void)
{ unsigned i;
  for (i = 0; i < MT_SHOW_SLOTS; i++)
  { free(g_show[i]);
    g_show[i] = NULL;
  }
  g_show_at = 0;
}

void mt_free(void *pointer)
{ free(pointer);
}

/* ================================================================== *
 * Spaces
 * ================================================================== */

/* The runtime argument is not read -- there is one runtime per process -- but
   it is CHECKED, because a caller holding the NULL a failed mt_open() gave
   them is a caller who is about to write to a space that does not exist. */
mt_space *mt_self(metta *runtime)
{ return handle_ready(runtime, "mt_self") ? &g_self : NULL;
}

mt_space *mt_catalog(metta *runtime)
{ return handle_ready(runtime, "mt_catalog") ? &g_catalog : NULL;
}

/* The name is C memory, so this answers after mt_close() as well as before
   mt_open(), and NULL is an answer rather than a failure: a caller asking a
   handle its own name is classifying, not acting. */
const char *mt_space_name(const mt_space *space)
{ return space ? space->name : NULL;
}

mt_space *mt_space_open(metta *runtime, const char *name)
{ mt_space *s;

  if ( !handle_ready(runtime, "mt_space_open") ) return NULL;
  if ( !name || name[0] != '&' )
    return err_null(MT_MISUSE,
                    "a space is named with a leading ampersand; %s is not",
                    name ? name : "NULL");
  if ( strcmp(name, "&self") == 0 )  return &g_self;
  if ( strcmp(name, "&metta") == 0 ) return &g_catalog;

  if ( !(s = calloc(1, sizeof(*s))) )
    return err_null(MT_NOMEM, "out of memory opening a space");
  if ( !(s->name = strdup(name)) )
  { free(s);
    return err_null(MT_NOMEM, "out of memory naming a space");
  }
  s->runtime = runtime;
  return s;
}

void mt_space_close(mt_space *space)
{ if ( !space || space->borrowed ) return;
  free(space->name);
  free(space);
}

/* A door that TAKES an atom refuses NULL rather than passing it on: see
   space_call's note on what an unbound argument means to the bridge. */
static bool atom_given(const mt_atom *atom, const char *door)
{ if ( atom ) return true;
  err_set(MT_MISUSE,
          "%s was given no atom; the constructor that should have made one "
          "failed and mt_errmsg() said why at the time", door);
  return false;
}

/* One of the bridge's space predicates, with the space's name in av[0] and,
   when `atom` is not NULL, the atom in av[1].

   A door that TAKES an atom checks it BEFORE it calls here, because a NULL
   would leave av[1] an UNBOUND VARIABLE and the bridge reads that as a
   wildcard: mt_del(space, NULL) removed every atom in the space and
   mt_add(space, NULL) stored a fresh variable, both answering `ok`
   [measured 2026-08-31, ai-tmp/cseat-probe-d1.c; C32 in
   ai-cmetta-c-constraints.md].

   THE FRAME AND THE VECTOR GO BACK ON EVERY EXIT when the caller asked for
   them, so the caller's cleanup is unconditional and right however this
   ended. The early return this replaced kept both and wrote neither, and the
   two callers that take a frame then discarded an UNINITIALISED fid_t
   [measured 2026-08-31: clang --analyze reported cmetta.c:1737 and 1741, and
   valgrind "Use of uninitialised value of size 8 at
   PL_discard_foreign_frame" under mt_space_del]. */
static mt_status space_call(const char *pred, mt_space *space,
                                 const mt_atom *atom, int arity,
                                 term_t *avp, fid_t *fp)
{ fid_t f = frame_open(pred);
  term_t av = 0;
  mt_status status = MT_NOMEM;

  if ( f && !(av = PL_new_term_refs(arity)) )
    err_set(MT_NOMEM, "out of memory holding the arguments for %s", pred);

  if ( fp ) *fp = f;
  if ( avp ) *avp = av;

  if ( f && av )
  { if ( !PL_put_atom_chars(av, space->name) ||
         ( atom && !put_atom(atom, av + 1) ) )
      status = mt_ok() ? err_set(MT_MISUSE,
                                 "%s could not write its arguments", pred)
                       : mt_error();   /* put_atom already said why */
    else
      status = call_bridge(pred, arity, av);
  }
  if ( !fp ) frame_close(f);
  return status;
}

/* These TAKE their atom. Building one inline is the common shape, so the door
   that consumes it owns it; a caller keeping a term hands over mt_keep(t).
   The atom is dropped whatever happens, including on a refusal before the
   engine was reached, so no path leaks it. */
bool mt_space_add(mt_space *space, mt_atom *atom)
{ mt_status status = MT_MISUSE;

  if ( handle_ready(space, "mt_space_add") && atom_given(atom, "mt_space_add") )
    status = space_call("metta_c_add", space, atom, 2, NULL, NULL);
  mt_drop(atom);
  return status == MT_OK;
}

bool mt_space_del(mt_space *space, mt_atom *atom)
{ fid_t f = 0;
  term_t av = 0;
  mt_status status = MT_MISUSE;
  bool removed = false;

  if ( handle_ready(space, "mt_space_del") && atom_given(atom, "mt_space_del") )
    status = space_call("metta_c_remove", space, atom, 3, &av, &f);

  if ( status == MT_OK )
  { char *text = term_text(av + 2, CVT_ATOM, NULL);
    removed = text && strcmp(text, "true") == 0;
    free(text);
  }
  frame_close(f);
  mt_drop(atom);
  return removed;
}

size_t mt_space_count(mt_space *space)
{ fid_t f = 0;
  term_t av = 0;
  mt_status status = MT_MISUSE;
  int64_t n = 0;

  if ( handle_ready(space, "mt_space_count") )
    status = space_call("metta_c_count", space, NULL, 2, &av, &f);

  if ( status == MT_OK && !PL_get_int64(av + 1, &n) )
    err_set(MT_ERROR, "the space did not answer a count");
  frame_close(f);
  return status == MT_OK ? (size_t)n : 0;
}

bool mt_space_wipe(mt_space *space)
{ return handle_ready(space, "mt_space_wipe") &&
         space_call("metta_c_clear", space, NULL, 1, NULL, NULL) == MT_OK;
}

/* The &self halves of the same verbs, which is what a `metta *` receiver
   reaches. Written out rather than generated so each one is greppable. */
bool mt_self_add(metta *runtime, mt_atom *atom)
{ return mt_space_add(mt_self(runtime), atom); }
bool mt_self_del(metta *runtime, mt_atom *atom)
{ return mt_space_del(mt_self(runtime), atom); }
size_t mt_self_count(metta *runtime)
{ return mt_space_count(mt_self(runtime)); }
bool mt_self_wipe(metta *runtime)
{ return mt_space_wipe(mt_self(runtime)); }

/* ================================================================== *
 * Answers
 * ================================================================== */

/* A cursor is one of two things wearing one face: a table of answers a run
   already computed, or an engine suspended between them. */
struct mt_answers
{ metta        *runtime;
  bool          lazy;
  mt_atom      *pattern;        /* what mt_bound lines each answer against */
  int64_t       cursor_id;      /* lazy: the bridge's engine id     */
  mt_atom **rows;          /* eager: every answer, in order    */
  char        **texts;
  size_t       *groups;
  size_t        n, at;
  bool          started, done;
  mt_atom      *current;
  char         *current_text;
  size_t        current_group;
  mt_row        row;            /* refreshed each step; mt_next points at it */
};

static mt_answers *answers_alloc(metta *runtime)
{ mt_answers *a = calloc(1, sizeof(*a));
  if ( !a ) err_set(MT_NOMEM, "out of memory opening a cursor");
  else a->runtime = runtime;
  return a;
}

/* Read the engine's Groups term: a list of groups, each a list of answers. */
static mt_status collect_groups(term_t groups, mt_answers *out)
{ term_t group = PL_new_term_ref();
  term_t gtail = PL_copy_term_ref(groups);
  term_t answer = PL_new_term_ref();
  size_t cap = 8, index = 0;

  if ( !(out->rows = malloc(cap * sizeof(*out->rows))) ||
       !(out->texts = malloc(cap * sizeof(*out->texts))) ||
       !(out->groups = malloc(cap * sizeof(*out->groups))) )
    return err_set(MT_NOMEM, "out of memory collecting answers");

  while ( PL_get_list(gtail, group, gtail) )
  { term_t atail = PL_copy_term_ref(group);
    while ( PL_get_list(atail, answer, atail) )
    { fid_t f = frame_open("collecting an answer");
      term_t av = f ? PL_new_term_refs(4) : 0;
      mt_atom *atom = NULL;
      char *text = NULL;

      if ( av && PL_unify(av, answer) &&
           call_bridge("metta_c_answer_parts", 4, av) == MT_OK )
      { atom = decode(av + 1, av + 2);
        text = term_text(av + 3, CVT_ATOM | CVT_STRING, NULL);
      }
      frame_close(f);

      if ( !atom )
      { free(text);
        return MT_UNSUPPORTED;
      }
      if ( out->n == cap )
      { cap *= 2;
        out->rows = realloc(out->rows, cap * sizeof(*out->rows));
        out->texts = realloc(out->texts, cap * sizeof(*out->texts));
        out->groups = realloc(out->groups, cap * sizeof(*out->groups));
        if ( !out->rows || !out->texts || !out->groups )
          return err_set(MT_NOMEM, "out of memory collecting answers");
      }
      out->rows[out->n] = atom;
      out->texts[out->n] = text;
      out->groups[out->n] = index;
      out->n++;
    }
    index++;
  }
  return MT_OK;
}

static mt_status run_or_load(metta *runtime, const char *pred,
                                  const char *argument, const char *space,
                                  mt_answers **out)
{ fid_t f;
  term_t av;
  mt_answers *answers;
  mt_status status;

  *out = NULL;   /* zeroed FIRST: a caller reusing one variable across calls
                    would otherwise still hold the last cursor's pointer after
                    a failure, and free it twice. */
  if ( !argument ) return err_set(MT_MISUSE, "%s needs an argument", pred);
  if ( !(answers = answers_alloc(runtime)) ) return MT_NOMEM;

  if ( !(f = frame_open(pred)) )
  { mt_answers_free(answers);
    return MT_NOMEM;
  }
  av = PL_new_term_refs(5);
  if ( !av || !PL_put_string_chars(av, argument) ||
       !PL_put_atom_chars(av + 1, space) ||
       !PL_put_float(av + 2, runtime->limits.seconds) ||
       !PL_put_int64(av + 3, (int64_t)runtime->limits.inferences) )
  { PL_discard_foreign_frame(f);
    mt_answers_free(answers);
    return err_set(MT_NOMEM, "out of memory holding the argument");
  }
  status = call_bridge(pred, 5, av);
  if ( status == MT_OK ) status = collect_groups(av + 4, answers);
  PL_discard_foreign_frame(f);

  if ( status != MT_OK )
  { mt_answers_free(answers);
    return status;
  }
  *out = answers;
  return MT_OK;
}

mt_answers *mt_run(metta *runtime, const char *source)
{ mt_answers *out = NULL;
  if ( handle_ready(runtime, "mt_run") )
    run_or_load(runtime, "metta_c_run", source, "&self", &out);
  return out;
}

bool mt_do(metta *runtime, const char *source)
{ mt_answers *answers;
  if ( !handle_ready(runtime, "mt_do") ) return false;
  if ( !(answers = mt_run(runtime, source)) ) return false;
  mt_answers_free(answers);
  return true;
}

mt_answers *mt_load(metta *runtime, const char *path)
{ mt_answers *out = NULL;
  if ( handle_ready(runtime, "mt_load") )
    run_or_load(runtime, "metta_c_load", path, "&self", &out);
  return out;
}

static mt_status open_cursor(mt_space *space, const char *pred,
                                  const mt_atom *atom,
                                  mt_answers **out)
{ fid_t f;
  term_t av;
  mt_answers *answers;
  mt_status status;
  int64_t id;

  *out = NULL;   /* see run_or_load: zeroed before anything can fail. */
  if ( !(answers = answers_alloc(space->runtime)) ) return MT_NOMEM;

  if ( !(f = frame_open(pred)) )
  { mt_answers_free(answers);
    return MT_NOMEM;
  }
  av = PL_new_term_refs(4);
  if ( !av || !put_atom(atom, av) || !PL_put_atom_chars(av + 1, space->name) ||
       !PL_put_int64(av + 2, (int64_t)space->runtime->limits.inferences) )
  { PL_discard_foreign_frame(f);
    mt_answers_free(answers);
    return mt_ok() ? err_set(MT_MISUSE, "%s could not write its goal", pred)
                   : mt_error();   /* put_atom already said why */
  }
  status = call_bridge(pred, 4, av);
  if ( status == MT_OK && PL_get_int64(av + 3, &id) )
  { answers->lazy = true;
    answers->cursor_id = id;
  } else if ( status == MT_OK )
  { status = err_set(MT_ERROR, "the bridge did not answer a cursor id");
  }
  PL_discard_foreign_frame(f);

  if ( status != MT_OK )
  { mt_answers_free(answers);
    return status;
  }
  *out = answers;
  return MT_OK;
}

/* These TAKE their atom, on the same reasoning as the write verbs: a goal is
   almost always built at the call site, and a door that consumes it is what
   makes mt_eval(m, mt_expr("+", 1, 2)) leak nothing. */
/* The atom is TAKEN, and the cursor keeps a reference to it rather than
   dropping it outright: mt_bound() needs the pattern to say which subterm a
   name reached, and the caller no longer has it to pass back in. */
/* `door` is the name a caller would recognise, because a refusal that names
   the bridge predicate answers a question the caller did not ask. Which
   predicate to open follows from `as_pattern` rather than being a second
   argument that could disagree with it: only a match has a pattern. */
static mt_answers *open_with(mt_space *space, const char *door,
                             mt_atom *atom, bool as_pattern)
{ mt_answers *out = NULL;

  if ( handle_ready(space, door) && atom_given(atom, door) )
    open_cursor(space, as_pattern ? "metta_c_open_match"
                                  : "metta_c_open_eval", atom, &out);
  /* Only a MATCH keeps its atom. A match answer is an INSTANCE of the
     pattern, so the two line up position for position and mt_bound() can read
     a binding off them. An eval answer is a reduced value and shares no shape
     with the goal, so keeping the goal would let mt_bound() find a subterm at
     the same index and call it a binding, which is a wrong answer rather than
     a missing one. */
  if ( out && as_pattern ) out->pattern = atom;   /* takes the reference */
  else mt_drop(atom);
  return out;
}

mt_answers *mt_space_eval(mt_space *space, mt_atom *goal)
{ return open_with(space, "mt_space_eval", goal, false);
}

mt_answers *mt_space_match(mt_space *space, mt_atom *pattern)
{ return open_with(space, "mt_space_match", pattern, true);
}

mt_answers *mt_space_atoms(mt_space *space)
{ /* Every stored atom is the match a fresh variable makes. */
  return mt_space_match(space, mt_var("_"));
}

mt_answers *mt_self_eval(metta *runtime, mt_atom *goal)
{ return mt_space_eval(mt_self(runtime), goal); }
mt_answers *mt_self_match(metta *runtime, mt_atom *pattern)
{ return mt_space_match(mt_self(runtime), pattern); }
mt_answers *mt_self_atoms(metta *runtime)
{ return mt_space_atoms(mt_self(runtime)); }

static void clear_current(mt_answers *answers)
{ if ( answers->lazy )
  { mt_drop(answers->current);
    free(answers->current_text);
  }
  answers->current = NULL;
  answers->current_text = NULL;
}

static mt_status answers_step(mt_answers *answers)
{ fid_t f;
  term_t av;
  mt_status status;
  term_t head, tail;

  if ( !answers ) return err_set(MT_MISUSE, "mt_answers_step needs a cursor");
  if ( answers->done ) return MT_DONE;

  if ( !answers->lazy )
  { if ( answers->at >= answers->n )
    { answers->done = true;
      answers->current = NULL;
      answers->current_text = NULL;
      return MT_DONE;
    }
    answers->current = answers->rows[answers->at];
    answers->current_text = answers->texts[answers->at];
    answers->current_group = answers->groups[answers->at];
    answers->at++;
    return MT_ROW;
  }

  /* Only the lazy half needs the engine; a run's answers are C memory and a
     caller may walk them after mt_close(). */
  if ( !engine_ready("mt_next") ) return MT_MISUSE;

  clear_current(answers);

  if ( !(f = frame_open("mt_next")) ) return MT_NOMEM;
  av = PL_new_term_refs(3);
  if ( !av || !PL_put_int64(av, answers->cursor_id) ||
       !PL_put_float(av + 1, answers->runtime->limits.seconds) )
  { PL_discard_foreign_frame(f);
    return err_set(MT_NOMEM, "out of memory stepping a cursor");
  }
  status = call_bridge("metta_c_next", 3, av);
  if ( status != MT_OK )
  { PL_discard_foreign_frame(f);
    answers->done = true;
    return status;
  }

  head = PL_new_term_ref();
  tail = PL_copy_term_ref(av + 2);
  if ( !head || !tail || !PL_get_list(tail, head, tail) )
  { PL_discard_foreign_frame(f);
    answers->done = true;
    return MT_DONE;
  }

  { term_t parts = PL_new_term_refs(4);
    if ( parts && PL_unify(parts, head) &&
         call_bridge("metta_c_answer_parts", 4, parts) == MT_OK )
    { answers->current = decode(parts + 1, parts + 2);
      answers->current_text = term_text(parts + 3, CVT_ATOM | CVT_STRING, NULL);
    }
  }
  PL_discard_foreign_frame(f);

  if ( !answers->current )
  { answers->done = true;
    return MT_UNSUPPORTED;
  }
  answers->started = true;
  return MT_ROW;
}

/* One call per answer instead of step-then-read, so the loop condition and
   the value are the same expression. NULL ends the walk; mt_ok() says
   whether that was exhaustion or a failure. */
const mt_atom *mt_next(mt_answers *answers)
{ if ( !answers ) return NULL;
  return answers_step(answers) == MT_ROW ? answers->current : NULL;
}

/* The same step reported in full. The row lives in the cursor and is
   refreshed here, so it is a pointer and costs no copy per answer. */
const mt_row *mt_row_next(mt_answers *answers)
{ if ( !mt_next(answers) ) return NULL;
  answers->row.atom  = answers->current;
  answers->row.text  = answers->current_text;
  answers->row.group = answers->current_group;
  answers->row.of    = answers;
  return &answers->row;
}

/* The first answer, owned, with the cursor closed behind it. Consuming the
   cursor is what lets this compose in one expression. */
mt_atom *mt_first(mt_answers *answers)
{ const mt_atom *found;
  mt_atom *owned = NULL;

  if ( !answers ) return NULL;
  if ( (found = mt_next(answers)) != NULL ) owned = mt_keep(found);
  mt_answers_free(answers);
  return owned;
}

/* Every answer as one owned array, for a caller who wants them all rather
   than a walk. */
/* Exactly one, or a recorded failure. Pulling the second answer is what makes
   the claim real, and it costs one step of a lazy cursor. */
mt_atom *mt_one(mt_answers *answers)
{ const mt_atom *found;
  mt_atom *owned = NULL;

  if ( !answers ) return NULL;
  if ( (found = mt_next(answers)) != NULL ) owned = mt_keep(found);
  if ( !owned )
  { if ( mt_ok() ) err_set(MT_FAIL, "the question had no answer");
  } else if ( mt_next(answers) != NULL )
  { err_set(MT_MISUSE,
            "the question answered more than once, and mt_one is a claim "
            "that it would not; use mt_first to take the first, or "
            "mt_each to walk them all");
    mt_drop(owned);
    owned = NULL;
  }
  mt_answers_free(answers);
  return owned;
}

/* Ask, read, and let go, which is the shape almost every question has: the
   caller wants the number, not an atom to look after. Each of these closes
   the cursor and drops the atom, so nothing is left owned. */
#define MT_ONE(name, type, read, zero)                                       \
  type name(mt_answers *answers)                                             \
  { mt_atom *a = mt_one(answers);                                            \
    type value;                                                              \
    if ( !a ) return zero;                                                   \
    value = read(a);                                                         \
    mt_drop(a);                                                              \
    return value;                                                            \
  }

MT_ONE(mt_one_int,   int64_t, mt_int,   0)
MT_ONE(mt_one_float, double,  mt_float, 0.0)
MT_ONE(mt_one_truth, bool,    mt_truth, false)
#undef MT_ONE

/* The text goes into the same rotating buffer mt_show() writes, so the atom
   can be released here and the caller still has something to print. */
const char *mt_one_name(mt_answers *answers)
{ mt_atom *a = mt_one(answers);
  const char *shown;

  if ( !a ) return NULL;
  shown = mt_show(a);
  mt_drop(a);
  return shown;
}

mt_list mt_all(mt_answers *answers)
{ mt_list out = { NULL, 0 };
  size_t cap = 0;
  const mt_atom *found;

  if ( !answers ) return out;
  while ( (found = mt_next(answers)) != NULL )
  { if ( out.len == cap )
    { size_t grown_cap = cap ? cap * 2 : 8;
      mt_atom **grown = realloc(out.items, grown_cap * sizeof(*grown));
      if ( !grown )
      { mt_list_free(out);
        mt_answers_free(answers);
        err_set(MT_NOMEM, "out of memory collecting answers");
        out.items = NULL;
        out.len = 0;
        return out;
      }
      out.items = grown;
      cap = grown_cap;
    }
    out.items[out.len++] = mt_keep(found);
  }
  mt_answers_free(answers);
  return out;
}

void mt_list_free(mt_list list)
{ size_t i;
  if ( !list.items ) return;
  for (i = 0; i < list.len; i++) mt_drop(list.items[i]);
  free(list.items);
}

/* Walk the pattern and the answer together; where the pattern has the named
   variable, the answer's subterm at that position is what it reached. Pure C
   over two C terms, so it costs one walk and no engine call. The first
   occurrence wins, which is the same rule a repeated variable already has:
   two occurrences of one name are one variable, so they agree.

   This is one variable's half of the directional match the Python seat spells
   `unify(pattern, atom)`, whose documented reconstruction is
   `substitute(pattern, unify(pattern, atom))`
   [source: extensions/python/metta/atoms.py, _match/unify]. Narrower on
   purpose: a caller asking for one name does not need the whole binding set
   built to be handed one term out of it. */
static size_t shorter_of(const mt_atom *a, const mt_atom *b)
{ return a->u.e.n < b->u.e.n ? a->u.e.n : b->u.e.n;
}

static const mt_atom *bound_in(const mt_atom *pattern, const mt_atom *answer,
                               const char *name)
{ pair_frame fixed[MT_WALK_FRAMES];
  pair_stack frames;
  pair_frame *f;
  const mt_atom *found = NULL;

  if ( !pattern || !answer ) return NULL;
  if ( mt_kind_of(pattern) == MT_VARIABLE )
    return strcmp(pattern->u.t.text, name) == 0 ? answer : NULL;
  if ( mt_kind_of(pattern) != MT_EXPR || mt_kind_of(answer) != MT_EXPR )
    return NULL;

  /* Depth first, left to right, taking each level's frame before its
     siblings, which is the order the recursive walk this replaced had and
     what makes "the first occurrence wins" mean the leftmost one. */
  stack_init(&frames, fixed);
  pair_push(&frames, pattern, answer, shorter_of(pattern, answer));

  while ( !found && (f = stack_top(&frames)) != NULL )
  { const mt_atom *p, *a;

    if ( f->at == f->n )
    { stack_pop(&frames);
      continue;
    }
    p = f->a->u.e.kids[f->at];
    a = f->b->u.e.kids[f->at];
    f->at++;

    if ( mt_kind_of(p) == MT_VARIABLE )
    { if ( strcmp(p->u.t.text, name) == 0 ) found = a;
      continue;
    }
    if ( mt_kind_of(p) != MT_EXPR || mt_kind_of(a) != MT_EXPR ) continue;
    /* `f` is not touched afterwards: the push may move it. */
    if ( !pair_push(&frames, p, a, shorter_of(p, a)) )
    { err_set(MT_NOMEM,
              "out of memory reading a binding out of a nested answer");
      break;
    }
  }
  stack_free(&frames);
  return found;
}

const mt_atom *mt_bound(const mt_row *row, const char *name)
{ if ( !row || !row->of || !row->of->pattern || !row->atom || !name )
    return NULL;
  return bound_in(row->of->pattern, row->atom, name);
}

/* A release door: it is a no-op rather than a refusal when there is nothing
   to release, so a host tidying up after mt_close() is not punished for the
   order it tidied in. What it can still free is C memory, and it does. */
void mt_answers_free(mt_answers *answers)
{ size_t i;
  if ( !answers ) return;
  mt_drop(answers->pattern);

  if ( answers->lazy )
  { if ( g_open )
    { fid_t f = frame_open("mt_answers_free");
      term_t av = f ? PL_new_term_refs(1) : 0;
      if ( av && PL_put_int64(av, answers->cursor_id) )
        call_bridge("metta_c_close", 1, av);
      frame_close(f);
    }
    clear_current(answers);
  } else
  { for (i = 0; i < answers->n; i++)
    { mt_drop(answers->rows[i]);
      free(answers->texts[i]);
    }
    free(answers->rows);
    free(answers->texts);
    free(answers->groups);
  }
  free(answers);
}

/* ================================================================== *
 * Bounding an evaluation
 * ================================================================== */

bool mt_limit(metta *runtime, mt_limits limits)
{ if ( !handle_ready(runtime, "mt_limit") ) return false;
  runtime->limits = limits;

  if ( runtime->limits.stack_bytes )
  { char goal_text[96];
    snprintf(goal_text, sizeof(goal_text),
             "set_prolog_flag(stack_limit, %zu)", runtime->limits.stack_bytes);
    if ( !goal(goal_text) )
    { err_set(MT_ERROR, "the stack ceiling %zu was refused",
              runtime->limits.stack_bytes);
      return false;
    }
  }
  return true;
}

/* Returned by value. A three-scalar struct is cheaper to copy than to
   out-parameter, and the call site reads as an expression. The bounds are C
   memory, so this answers after mt_close() as well; only NULL is refused. */
mt_limits mt_limits_of(const metta *runtime)
{ static const mt_limits none = {0, 0, 0};

  if ( !runtime )
  { err_set(MT_MISUSE, "mt_limits_of was given NULL, which is what a failed "
                       "mt_open() answers");
    return none;
  }
  return runtime->limits;
}

/* ================================================================== *
 * Measuring
 * ================================================================== */

mt_stats mt_stats_now(metta *runtime)
{ fid_t f;
  term_t av, head, tail;
  mt_status status;
  double values[6] = {0, 0, 0, 0, 0, 0};
  mt_stats out = {0, 0, 0, 0, 0, 0};
  size_t i = 0;

  if ( !handle_ready(runtime, "mt_stats_now") ) return out;
  if ( !(f = frame_open("mt_stats_now")) ) return out;

  av = PL_new_term_refs(1);
  status = av ? call_bridge("metta_c_stats", 1, av)
              : err_set(MT_NOMEM, "out of memory sampling the counters");
  if ( status == MT_OK )
  { head = PL_new_term_ref();
    tail = PL_copy_term_ref(av);
    while ( i < 6 && PL_get_list(tail, head, tail) )
    { int64_t whole;
      if ( PL_get_int64(head, &whole) ) values[i] = (double)whole;
      else if ( !PL_get_float(head, &values[i]) ) values[i] = 0;
      i++;
    }
    if ( i < 6 )
      status = err_set(MT_ERROR,
                       "the engine answered %zu counters where six were "
                       "expected", i);
  }
  PL_discard_foreign_frame(f);
  if ( status != MT_OK ) return out;

  out.inferences  = (uint64_t)values[0];
  out.cputime     = values[1];
  out.gc_count    = (uint64_t)values[2];
  out.gc_freed    = (uint64_t)values[3];
  out.gc_time     = values[4] / 1000.0;   /* the engine reports milliseconds */
  out.table_bytes = (uint64_t)values[5];
  return out;
}

mt_stats mt_stats_since(mt_stats before, mt_stats after)
{ mt_stats spent;
  spent.inferences  = after.inferences  - before.inferences;
  spent.cputime     = after.cputime     - before.cputime;
  spent.gc_count    = after.gc_count    - before.gc_count;
  spent.gc_freed    = after.gc_freed    - before.gc_freed;
  spent.gc_time     = after.gc_time     - before.gc_time;
  spent.table_bytes = after.table_bytes - before.table_bytes;
  return spent;
}

/* ================================================================== *
 * Publishing C functions
 * ================================================================== */

/* C spells a compound name with underscores and MeTTa spells it with hyphens,
   so car_atom publishes car-atom. This is the same map the Python seat makes,
   and for the same reason: each host reaches the meaning through its own
   casing convention. A name already carrying a hyphen or any character
   outside C's identifier grammar is passed through untouched, which is the
   escape for prime? and %Undefined%. */
static char *metta_name(const char *name)
{ char *out = strdup(name);
  char *p;
  bool identifier = true;

  if ( !out ) return NULL;
  for (p = out; *p; p++)
  { if ( !(*p == '_' || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
           (*p >= '0' && *p <= '9')) )
      identifier = false;
  }
  if ( identifier )
    for (p = out; *p; p++)
      if ( *p == '_' ) *p = '-';
  return out;
}

/* One struct argument, so the call site names what it is passing. Designated
   initializers are what C has instead of keyword arguments, and five
   positional parameters is exactly where they start paying. */
bool mt_def(metta *runtime, mt_op op)
{ fid_t f;
  term_t av;
  mt_status status;
  char *published;
  const char *name = op.name;
  size_t arity = op.arity;
  mt_fn fn = op.fn;
  void *user = op.user;
  const char *kind = mt_effect_str(op.effect);
  mt_op_entry_t *slot;

  if ( !handle_ready(runtime, "mt_def") ) return false;
  if ( !name || !fn )
  { err_set(MT_MISUSE, "mt_def needs a name and a function");
    return false;
  }
  if ( !kind )
  { err_set(MT_MISUSE,
            "an operation must name one of the five effect classes; "
            "%d is not one of them", (int)op.effect);
    return false;
  }
  if ( !(published = metta_name(name)) )
  { err_set(MT_NOMEM, "out of memory naming an operation");
    return false;
  }

  if ( !(f = frame_open("mt_def")) )
  { free(published);
    return false;
  }
  av = PL_new_term_refs(3);
  if ( !av || !PL_put_atom_chars(av, published) ||
       !PL_put_int64(av + 1, (int64_t)arity) ||
       !PL_put_atom_chars(av + 2, kind) )
  { PL_discard_foreign_frame(f);
    free(published);
    err_set(MT_NOMEM, "out of memory registering an operation");
    return false;
  }
  status = call_bridge("metta_c_register_op", 3, av);
  PL_discard_foreign_frame(f);
  if ( status != MT_OK )
  { free(published);
    return false;
  }

  if ( (slot = find_op(published, arity)) )
  { slot->fn = fn;
    slot->user = user;
    free(published);
    return true;
  }
  if ( runtime->nops == runtime->cap_ops )
  { size_t cap = runtime->cap_ops ? runtime->cap_ops * 2 : 8;
    mt_op_entry_t *grown = realloc(runtime->ops, cap * sizeof(*grown));
    if ( !grown )
    { free(published);
      err_set(MT_NOMEM, "out of memory recording an operation");
      return false;
    }
    runtime->ops = grown;
    runtime->cap_ops = cap;
  }
  slot = &runtime->ops[runtime->nops++];
  slot->name = published;
  slot->arity = arity;
  slot->fn = fn;
  slot->user = user;
  return true;
}

bool mt_undef(metta *runtime, const char *name)
{ fid_t f;
  term_t av;
  mt_status status;
  char *published;
  size_t i;

  if ( !handle_ready(runtime, "mt_undef") ) return false;
  if ( !name )
  { err_set(MT_MISUSE, "mt_undef needs a name");
    return false;
  }
  if ( !(published = metta_name(name)) )
  { err_set(MT_NOMEM, "out of memory naming an operation");
    return false;
  }

  if ( !(f = frame_open("mt_undef")) )
  { free(published);
    return false;
  }
  av = PL_new_term_refs(1);
  status = ( av && PL_put_atom_chars(av, published) )
         ? call_bridge("metta_c_unregister_op", 1, av)
         : err_set(MT_NOMEM, "out of memory withdrawing an operation");
  PL_discard_foreign_frame(f);

  for (i = 0; i < runtime->nops; )
  { if ( strcmp(runtime->ops[i].name, published) == 0 )
    { free(runtime->ops[i].name);
      runtime->ops[i] = runtime->ops[--runtime->nops];
    } else i++;
  }
  free(published);
  return status == MT_OK;
}
