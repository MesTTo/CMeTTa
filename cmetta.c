/* Purpose: the C half of the C binding. Boot SWI-Prolog in this process,
 *   consult the engine, and move values between C structures and engine terms
 *   directly, with no wire encoding in between.
 *
 * Assumes:
 *   - SWI-Prolog 10 with threads
 *     [source: /usr/lib/swi-prolog/include/SWI-Prolog.h, PLVERSION 100114;
 *     commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c]
 *   - extensions/cmetta/bridge.pl is loaded by extensions/cmetta/extension.pl, which
 *     the engine globs at boot, and which finds this file because mt_open()
 *     registers '$cmetta_present'/0 before consulting engine/metta.pl
 *   - a term handed out by the bridge is valid only inside the foreign frame
 *     the call opened, so every decode completes before the frame is discarded
 *     [source: SWI-Prolog.h:432-435; C1 in ai-cmetta-c-constraints.md]
 *
 * Guarantees:
 *   - mt_remedy() and mt_ground() answer the engine's own (refusal ...) row
 *     for the last ball rendered, and NULL for every failure of this
 *     library's own contract, because each error setter clears them
 *     [tested: extensions/cmetta/tests/test_cmetta.c,
 *     test_a_refusal_carries_the_engines_remedy_and_ground; commit=f33b7ab0200e6dc74c88fb4c7f827bf545a447ed]
 *   - no Prolog exception crosses into a caller: every query runs under
 *     PL_Q_CATCH_EXCEPTION, and the ball is rendered by the bridge into the
 *     thread-local error text
 *   - an engine term with no MeTTa reading is REFUSED by name rather than
 *     stringified into something that cannot go home again
 *   - an ampersand-prefixed atom becomes MT_SPACE only when the engine
 *     says it is a space [tested: test_a_user_space_decodes_as_a_space;
 *     commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c]
 *   - a door that reaches the engine before mt_open(), or after mt_close(),
 *     REFUSES with MT_MISUSE naming mt_open() instead of dereferencing a
 *     thread environment that is not there
 *     [tested: tests/test_cmetta.c, test_a_door_before_the_runtime_refuses;
 *     commit=c530ccb8fb7d0a5b2aa53df6e9f981ada9f81be8]
 *   - term walks do not recurse on the C stack. Decode, encode, equality and
 *     binding use explicit stacks; drop links already-dead nodes directly
 *     [tested: tests/test_cmetta.c, test_a_deep_term_does_not_overrun_the_stack,
 *     tests/test_ownership.c; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c]
 *   - allocator sizes, callback lists, space counts, engine counters and stack
 *     defaults are validated at their representation boundaries
 *     [tested: tests/test_internal_contracts.c and
 *     test_a_refused_stack_limit_clears_the_engine_exception;
 *     commit=da8c4da9df83114ab1d32f3e4049008f37535886]
 *   - a cursor refusal names the public step door used, and cached predicate
 *     and variable-pair handles are rebuilt after successful engine cleanup
 *     [tested: tests/test_cursor_ids.c and tests/test_reopen.c;
 *     commit=da8c4da9df83114ab1d32f3e4049008f37535886]
 *   - engine cursors retain the recorded owner reference across frames,
 *     unregister it after close even if Prolog raises, and never touch it
 *     after its runtime has ended [tested: tests/test_cursor_ids.c,
 *     tests/test_transactions.c; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c].
 *
 * Owns resources: the process's Prolog runtime, shut down through mt_close(); the
 *   op table; one malloc'ed box per live mt_object, released when both the
 *   C atom and the engine blob have let go; each engine cursor's registered
 *   owner atom until mt_answers_free() or runtime cleanup; native iterators
 *   close explicitly. C allocations carry their allocator and drop performs
 *   no allocation [tested: tests/test_ownership.c; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c].
 * Fails when: a dependency leaks despite PL_CLEANUP_SUCCESS. The installed SWI
 *   fails the independent make runtime-memory gate; see the memory result in
 *   CAPABILITIES.md [measured: 2026-09-22, three exit-99 probes;
 *   commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c].
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
 *   - term walks are iterative. A
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

/* Expose the POSIX interfaces used by the embedding boundary. */
#define _POSIX_C_SOURCE 200809L

#include "cmetta.h"

#include <SWI-Prolog.h>
#include <SWI-Stream.h>

#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
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
/* What to do about it and what says so, from the engine's own (refusal ...)
   row for the kind the ball was. Empty for every failure that is this
   library's contract rather than a MeTTa refusal, and cleared by every setter
   below so a later unrelated failure never reads an earlier refusal's advice.
   Only err_advice() fills them, from render_ball(). */
static MT_TLS char g_remedy[MT_ERR_MAX];
static MT_TLS char g_ground[MT_ERR_MAX];
static MT_TLS mt_status g_status = MT_OK;
/* A callback needs to tell a failure raised DURING this application from the
   errno-shaped failure that was already present when it began. Comparing the
   status is not enough: two consecutive failures may have the same kind. */
static MT_TLS uint64_t g_error_generation;

static void err_forget_advice(void)
{ g_remedy[0] = '\0';
  g_ground[0] = '\0';
}

static mt_status err_set(mt_status status, const char *fmt, ...)
{ va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_err, sizeof(g_err), fmt, ap);
  va_end(ap);
  err_forget_advice();
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
  err_forget_advice();
  g_status = status;
  g_error_generation++;
  return NULL;
}

static mt_status err_copy(mt_status status, const char *text)
{ snprintf(g_err, sizeof(g_err), "%s", text);
  err_forget_advice();
  g_status = status;
  g_error_generation++;
  return status;
}

/* The engine's own advice for the ball just rendered. Set AFTER err_copy(),
   which cleared it, and never composed here: the strings are the (refusal
   ...) row's, so all three seats say the same sentence about one refusal. */
static void err_advice(const char *remedy, const char *ground)
{ if ( remedy ) snprintf(g_remedy, sizeof(g_remedy), "%s", remedy);
  if ( ground ) snprintf(g_ground, sizeof(g_ground), "%s", ground);
}

static void err_reclassify(mt_status status)
{ g_status = status;
  g_error_generation++;
}

void mt_clear(void)
{ g_err[0] = '\0';
  err_forget_advice();
  g_status = MT_OK;
}

mt_status mt_error_set(mt_status status, const char *message)
{ if ( status < MT_ERROR || status > MT_LIMIT )
    return err_set(MT_MISUSE, "mt_error_set requires an error status");
  return err_copy(status, message ? message : "C callback failed");
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

const char *mt_remedy(void)
{ return ( g_status == MT_OK || g_remedy[0] == '\0' ) ? NULL : g_remedy;
}

const char *mt_ground(void)
{ return ( g_status == MT_OK || g_ground[0] == '\0' ) ? NULL : g_ground;
}

/* Preserve the allocating context across allocator changes and thread transfer.
   Lua's allocator contract preserves a block on failed growth:
   https://github.com/lua/lua/blob/6e22fedb74cf0c9b6656e9fce8b7331db847c605/lmem.c
   Time: one allocator call. Space: one aligned header per live allocation.
   [tested: tests/test_ownership.c; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c] */
typedef union allocation_header {
  max_align_t alignment;
  struct { mt_allocator allocator; size_t size; } value;
} allocation_header;

static MT_TLS mt_allocator g_allocator;

static void *libc_resize(void *user, void *pointer, size_t old_size,
                         size_t new_size)
{ (void)user;
  (void)old_size;
  if ( !new_size ) { free(pointer); return NULL; }
  return realloc(pointer, new_size);
}

mt_allocator mt_allocator_set(mt_allocator allocator)
{ mt_allocator previous = g_allocator;
  g_allocator = allocator;
  return previous;
}

void *mt_resize(void *pointer, size_t size)
{ allocation_header *old = pointer ? (allocation_header *)pointer - 1 : NULL;
  allocation_header *grown;
  mt_allocator allocator = old ? old->value.allocator : g_allocator;
  size_t bytes;
  if ( !allocator.resize ) allocator.resize = libc_resize;
  if ( !size )
  { if ( old ) allocator.resize(allocator.user, old, old->value.size, 0);
    return NULL;
  }
  if ( size > SIZE_MAX - sizeof(*old) )
    return err_null(MT_NOMEM, "allocation of %zu bytes exceeds addressable memory", size);
  bytes = sizeof(*old) + size;
  grown = allocator.resize(allocator.user, old, old ? old->value.size : 0, bytes);
  if ( !grown ) return err_null(MT_NOMEM, "allocator refused %zu bytes", size);
  grown->value.allocator = allocator;
  grown->value.size = bytes;
  return grown + 1;
}

void *mt_alloc(size_t size)
{ return mt_resize(NULL, size);
}

void *mt_calloc(size_t count, size_t size)
{ void *result;
  if ( size && count > SIZE_MAX / size )
    return err_null(MT_NOMEM, "allocation of %zu elements of %zu bytes overflows", count, size);
  result = mt_alloc(count * size);
  if ( result ) memset(result, 0, count * size);
  return result;
}

void mt_free(void *pointer)
{ allocation_header *header;
  if ( !pointer ) return;
  header = (allocation_header *)pointer - 1;
  header->value.allocator.resize(header->value.allocator.user, header,
                                 header->value.size, 0);
}

static char *mt_strdup(const char *text)
{ size_t size = strlen(text) + 1;
  char *copy = mt_alloc(size);
  if ( copy ) memcpy(copy, text, size);
  return copy;
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
{ return MT_VERSION;
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
   nesting, each one a SIGSEGV rather than a refusal
   [measured 2026-08-31 on an 8 MB stack]. RapidJSON reached the same
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
  { if ( (s)->items != (s)->fixed ) mt_free((s)->items);                  \
    (s)->items = (s)->fixed;                                              \
    (s)->n = 0;                                                           \
  } while (0)

/* Array arithmetic is checked before allocator arithmetic. malloc(count *
   width) is otherwise a smaller successful allocation followed by an
   out-of-bounds walk, not an allocation failure. */
static bool array_bytes(size_t count, size_t width, size_t *bytes)
{ if ( width && count > SIZE_MAX / width ) return false;
  *bytes = count * width;
  return true;
}

static bool next_capacity(size_t current, size_t initial, size_t width,
                          size_t *next, size_t *bytes)
{ size_t capacity;
  if ( current )
  { if ( current > SIZE_MAX / 2 ) return false;
    capacity = current * 2;
  } else
    capacity = initial;
  if ( !array_bytes(capacity, width, bytes) ) return false;
  *next = capacity;
  return true;
}

/* Twice the room, or the block it was given back unchanged: growing is the
   only part of a push that is not a store, so it is the only part that is a
   function, and a failure leaves the caller's stack intact. */
static void *stack_grow_(void *items, void *fixed, size_t *cap, size_t width)
{ size_t bigger, bytes;
  void *grown;

  if ( !next_capacity(*cap, 1, width, &bigger, &bytes) )
  { err_set(MT_NOMEM, "a term walk is too deep for an addressable stack");
    return items;
  }
  grown = ( items == fixed ) ? mt_alloc(bytes) : mt_resize(items, bytes);
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
  mt_answers     *stream;
  void                 *user;
} mt_box_t;

struct mt_atom
{ MT_ATOMIC unsigned refs;
  mt_kind          kind;
  struct mt_atom  *drop_next; /* writable only after the last reference */
  void           *owner;
  mt_free_fn      release;
  bool           borrowed;
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
{ mt_atom *a = mt_calloc(1, sizeof(*a));
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
  if ( len == SIZE_MAX )
    return err_null(MT_NOMEM, "text length leaves no space for its terminator");
  if ( !(a = atom_alloc(kind)) ) return NULL;
  if ( !(a->u.t.text = mt_alloc(len + 1)) )
  { mt_free(a);
    err_set(MT_NOMEM, "out of memory copying %zu bytes of text", len);
    return NULL;
  }
  memcpy(a->u.t.text, text, len);
  a->u.t.text[len] = '\0';
  a->u.t.len = len;
  return a;
}

#ifdef MT_TEST_FAULTS
/* MT_HANDLE is a native engine value and cannot be constructed by the public
   C surface. The fault library exposes one only so the pure-C hash regression
   covers the final kind without inventing a public constructor. */
mt_atom *mt_test_handle_atom(const char *text)
{ return atom_text(MT_HANDLE, text, text ? strlen(text) : 0);
}
#endif

static void box_release(mt_box_t *box)
{ if ( !box ) return;
  if ( MT_DEC(&box->refs) == 1 )
  { if ( box->release ) box->release(box->value);
    mt_free(box->type);
    mt_free(box);
  }
}

mt_atom *mt_keep(const mt_atom *atom)
{ mt_atom *a = (mt_atom *)atom;
  if ( a ) MT_INC(&a->refs);
  return a;
}

/* Dead nodes carry the pending links, so even an exhausted allocator can
   release a DAG. A live node's link is never touched.
   Time: Theta(V + E), V released atoms and E their child references.
   Space: O(1) auxiliary bytes; no allocation and no recursive calls.
   [tested: tests/test_ownership.c; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c] */
void mt_drop(const mt_atom *atom)
{ mt_atom *pending = (mt_atom *)atom;
  if ( !pending || MT_DEC(&pending->refs) != 1 ) return;
  pending->drop_next = NULL;
  while ( pending )
  { mt_atom *a = pending;
    pending = a->drop_next;
    switch ( a->kind )
    { case MT_EXPR:
        for (size_t i = a->u.e.n; i > 0; i--)
        { mt_atom *kid = a->u.e.kids[i - 1];
          if ( MT_DEC(&kid->refs) == 1 )
          { kid->drop_next = pending;
            pending = kid;
          }
        }
        if ( !a->borrowed ) mt_free(a->u.e.kids);
        break;
      case MT_SYMBOL:
      case MT_VARIABLE:
      case MT_TEXT:
      case MT_SPACE:
      case MT_BIGINT:
      case MT_HANDLE:
        if ( !a->borrowed ) mt_free(a->u.t.text);
        break;
      case MT_OBJECT:
        box_release(a->u.box);
        break;
      default:
        break;
    }
    if ( a->release ) a->release(a->owner);
    mt_free(a);
  }
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

mt_atom *mt_text_ref(const char *text, size_t length, void *owner,
                      mt_free_fn release)
{ mt_atom *atom;
  if ( !text || length == SIZE_MAX || text[length] != '\0' )
    return err_null(MT_MISUSE, "mt_text_ref needs terminated immutable text");
  atom = atom_alloc(MT_TEXT);
  if ( !atom ) return NULL;
  atom->u.t.text = (char *)text;
  atom->u.t.len = length;
  atom->borrowed = true;
  atom->owner = owner;
  atom->release = release;
  return atom;
}

mt_atom *mt_num(int64_t value)
{ mt_atom *a = atom_alloc(MT_INT);
  if ( a ) a->u.i = value;
  return a;
}

mt_atom *mt_unum(uint64_t value)
{ char decimal[sizeof(value) * 3 + 1];
  if ( value <= INT64_MAX ) return mt_num((int64_t)value);
  snprintf(decimal, sizeof(decimal), "%llu", (unsigned long long)value);
  return mt_bigint(decimal);
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
  int previous_errno;
  intmax_t value;
  bool in_range, negative;
  mt_atom *atom;
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
  /* Match the engine's integer representation: small values are MT_INT and
     leading zeroes never change identity. Time and space: O(D) decimal digits.
     [tested: tests/test_native_parity.c; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c] */
  previous_errno = errno;
  errno = 0;
  value = strtoimax(decimal, NULL, 10);
  in_range = errno != ERANGE && value >= INT64_MIN && value <= INT64_MAX;
  errno = previous_errno;
  if ( in_range ) return mt_num((int64_t)value);
  negative = *decimal == '-';
  p = decimal + negative;
  while ( *p == '0' ) p++;
  /* Reserve the byte before the significant digits for the sign. */
  atom = atom_text(MT_BIGINT, p - negative, strlen(p) + negative);
  if ( atom && negative ) atom->u.t.text[0] = '-';
  return atom;
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
  size_t bytes, i;
  bool bad = false;

  if ( count > 0 && !children )
    return err_null(MT_MISUSE,
                    "mt_exprv was asked for %zu children and given no array",
                    count);
  if ( !array_bytes(count, sizeof(*children), &bytes) )
    return err_null(MT_NOMEM,
                    "an expression with %zu children exceeds addressable memory",
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
  { if ( !(a->u.e.kids = mt_alloc(bytes)) )
    { mt_free(a);
      for (i = 0; i < count; i++) mt_drop(children[i]);
      err_set(MT_NOMEM, "out of memory building an expression of %zu", count);
      return NULL;
    }
    memcpy(a->u.e.kids, children, bytes);
  }
  a->u.e.n = count;
  return a;
}

/* Keep unsigned values exact and retain borrowed const atoms. */
mt_atom *mt_num_(long long value)      { return mt_num((int64_t)value); }
mt_atom *mt_unum_(unsigned long long value) { return mt_unum((uint64_t)value); }
mt_atom *mt_real_(long double value)
{ double narrowed = (double)value;
  if ( !isnan(value) && (long double)narrowed != value )
    return err_null(MT_UNSUPPORTED, "long double is not exactly representable as an engine float");
  return mt_real(narrowed);
}
mt_atom *mt_same(mt_atom *atom)     { return atom; }
mt_atom *mt_same_c(const mt_atom *atom) { return mt_keep(atom); }

mt_atom *mt_expr_ref(size_t count, const mt_atom *const *children,
                      void *owner, mt_free_fn release)
{ mt_atom *atom;
  if ( count && !children )
    return err_null(MT_MISUSE, "mt_expr_ref needs a child vector");
  if ( count > SIZE_MAX / sizeof(*children) )
    return err_null(MT_NOMEM, "mt_expr_ref child count exceeds addressable memory");
  for (size_t i = 0; i < count; i++)
    if ( !children[i] ) return err_null(MT_MISUSE, "mt_expr_ref has a NULL child");
  atom = atom_alloc(MT_EXPR);
  if ( !atom ) return NULL;
  for (size_t i = 0; i < count; i++) (void)mt_keep(children[i]);
  atom->u.e.kids = (mt_atom **)children;
  atom->u.e.n = count;
  atom->borrowed = true;
  atom->owner = owner;
  atom->release = release;
  return atom;
}

const mt_atom *const *mt_children(const mt_atom *atom)
{ return atom && atom->kind == MT_EXPR
           ? (const mt_atom *const *)atom->u.e.kids : NULL;
}

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
         test_float_identity_agrees_with_the_engine;
         commit=2e13376bb6e1662655525533a1ab02800940aec5] */
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

/* RFC 9923 defines FNV-1a as xor-then-multiply per input octet and recommends
   it for general non-cryptographic use. This hash is deliberately an
   in-process table hash rather than a persistent wire value, so native byte
   order and object addresses are part of its contract
   [source: https://www.rfc-editor.org/rfc/rfc9923.html#section-2;
   commit=d37f1a5192999fdaa1a617e86191de4fe3570f91]. */
#define MT_FNV64_OFFSET UINT64_C(14695981039346656037)
#define MT_FNV64_PRIME  UINT64_C(1099511628211)

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t len)
{ const unsigned char *bytes = data;
  size_t i;
  for (i = 0; i < len; i++)
  { hash ^= bytes[i];
    hash *= MT_FNV64_PRIME;
  }
  return hash;
}

static uint64_t hash_shallow(uint64_t hash, const mt_atom *atom)
{ unsigned char kind = (unsigned char)atom->kind;
  hash = hash_bytes(hash, &kind, sizeof(kind));
  switch ( atom->kind )
  { case MT_SYMBOL:
    case MT_VARIABLE:
    case MT_TEXT:
    case MT_SPACE:
    case MT_BIGINT:
    case MT_HANDLE:
      hash = hash_bytes(hash, &atom->u.t.len, sizeof(atom->u.t.len));
      return hash_bytes(hash, atom->u.t.text, atom->u.t.len);
    case MT_INT:
      return hash_bytes(hash, &atom->u.i, sizeof(atom->u.i));
    case MT_FLOAT:
    { unsigned char nan_value = isnan(atom->u.f) ? 1u : 0u;
      hash = hash_bytes(hash, &nan_value, sizeof(nan_value));
      return nan_value ? hash
                       : hash_bytes(hash, &atom->u.f, sizeof(atom->u.f));
    }
    case MT_BOOL:
    { unsigned char value = atom->u.b ? 1u : 0u;
      return hash_bytes(hash, &value, sizeof(value));
    }
    case MT_RATIONAL:
      hash = hash_bytes(hash, &atom->u.r.num, sizeof(atom->u.r.num));
      return hash_bytes(hash, &atom->u.r.den, sizeof(atom->u.r.den));
    case MT_EXPR:
      return hash_bytes(hash, &atom->u.e.n, sizeof(atom->u.e.n));
    case MT_OBJECT:
    { uintptr_t identity = (uintptr_t)atom->u.box;
      return hash_bytes(hash, &identity, sizeof(identity));
    }
    case MT_NONE:
      break;
  }
  return hash;
}

typedef struct hash_frame
{ const mt_atom *atom;
  size_t at;
} hash_frame;

typedef MT_STACK(hash_frame) hash_stack;

uint64_t mt_hash(const mt_atom *atom)
{ hash_frame fixed[MT_WALK_FRAMES];
  hash_stack frames;
  const mt_atom *current = atom;
  uint64_t hash = MT_FNV64_OFFSET;

  if ( !atom )
  { err_set(MT_MISUSE, "mt_hash was given NULL");
    return 0;
  }
  stack_init(&frames, fixed);
  while ( current )
  { hash = hash_shallow(hash, current);
    if ( current->kind == MT_EXPR && current->u.e.n )
    { hash_frame frame = { current, 1 };
      if ( !stack_push(&frames, frame) )
      { stack_free(&frames);
        return 0;
      }
      current = current->u.e.kids[0];
      continue;
    }
    current = NULL;
    while ( stack_top(&frames) )
    { hash_frame *frame = stack_top(&frames);
      if ( frame->at < frame->atom->u.e.n )
      { current = frame->atom->u.e.kids[frame->at++];
        break;
      }
      stack_pop(&frames);
    }
  }
  stack_free(&frames);
  return hash;
}

/* ================================================================== *
 * Pure-C unification and substitution
 * ================================================================== */

typedef struct binding_entry
{ mt_atom *variable;
  mt_atom *value;
} binding_entry;

struct mt_bindings
{ binding_entry *entries;
  size_t         len, cap;
  /* Open-addressed indices, stored plus one so zero remains the empty slot.
     Bindings are never deleted individually, so no tombstone state exists. */
  size_t        *slots;
  size_t         slot_cap;
};

#define NO_BINDING SIZE_MAX

static bool named_variable(const mt_atom *atom)
{ return atom && atom->kind == MT_VARIABLE &&
         !(atom->u.t.len == 1 && atom->u.t.text[0] == '_');
}

static uint64_t binding_name_hash(const char *name, size_t len)
{ unsigned char kind = (unsigned char)MT_VARIABLE;
  uint64_t hash = MT_FNV64_OFFSET;
  hash = hash_bytes(hash, &kind, sizeof(kind));
  hash = hash_bytes(hash, &len, sizeof(len));
  return hash_bytes(hash, name, len);
}

static bool binding_name_equal(const binding_entry *entry,
                               const char *name, size_t len)
{ return entry->variable->u.t.len == len &&
         memcmp(entry->variable->u.t.text, name, len) == 0;
}

static size_t binding_index_text(const mt_bindings *bindings,
                                 const char *name, size_t len)
{ size_t at, probed;

  if ( !bindings->slot_cap || !bindings->slots || !bindings->entries ||
       !bindings->len )
    return NO_BINDING;
  at = (size_t)binding_name_hash(name, len) & (bindings->slot_cap - 1);
  for (probed = 0; probed < bindings->slot_cap; probed++)
  { size_t slot = bindings->slots[at];
    if ( !slot ) return NO_BINDING;
    if ( binding_name_equal(&bindings->entries[slot - 1], name, len) )
      return slot - 1;
    at = (at + 1) & (bindings->slot_cap - 1);
  }
  return NO_BINDING;
}

static size_t binding_index_atom(const mt_bindings *bindings,
                                 const mt_atom *variable)
{ if ( !named_variable(variable) ) return NO_BINDING;
  return binding_index_text(bindings, variable->u.t.text, variable->u.t.len);
}

static void binding_insert_slot(size_t *slots, size_t slot_cap,
                                const binding_entry *entries, size_t index)
{ const mt_atom *variable = entries[index].variable;
  size_t at = (size_t)binding_name_hash(variable->u.t.text,
                                        variable->u.t.len) & (slot_cap - 1);
  while ( slots[at] ) at = (at + 1) & (slot_cap - 1);
  slots[at] = index + 1;
}

/* Reserve both arrays before an entry acquires references. Half-full tables
   keep an unsuccessful lookup bounded by an empty slot, and power-of-two
   doubling makes the mask above valid. */
static bool bindings_reserve_one(mt_bindings *bindings)
{ if ( !bindings->slot_cap || bindings->len >= bindings->slot_cap / 2 )
  { size_t slot_cap, bytes, i;
    size_t *slots;
    if ( !next_capacity(bindings->slot_cap, 8, sizeof(*slots),
                        &slot_cap, &bytes) )
    { err_set(MT_NOMEM, "a binding table exceeds addressable memory");
      return false;
    }
    slots = mt_calloc(1, bytes);
    if ( !slots )
    { err_set(MT_NOMEM, "out of memory growing a binding index to %zu slots",
              slot_cap);
      return false;
    }
    for (i = 0; i < bindings->len; i++)
      binding_insert_slot(slots, slot_cap, bindings->entries, i);
    mt_free(bindings->slots);
    bindings->slots = slots;
    bindings->slot_cap = slot_cap;
  }

  if ( bindings->len == bindings->cap )
  { size_t cap, bytes;
    binding_entry *entries;
    if ( !next_capacity(bindings->cap, 8, sizeof(*entries), &cap, &bytes) )
    { err_set(MT_NOMEM, "a substitution exceeds addressable memory");
      return false;
    }
    entries = mt_resize(bindings->entries, bytes);
    if ( !entries )
    { err_set(MT_NOMEM, "out of memory growing a substitution to %zu entries",
              cap);
      return false;
    }
    bindings->entries = entries;
    bindings->cap = cap;
  }
  return true;
}

static void binding_replace_value(binding_entry *entry,
                                  const mt_atom *value)
{ mt_atom *kept = mt_keep(value);
  mt_drop(entry->value);
  entry->value = kept;
}

static bool binding_put(mt_bindings *bindings, const mt_atom *variable,
                        const mt_atom *value)
{ size_t index = binding_index_atom(bindings, variable);
  if ( index != NO_BINDING )
  { assert(bindings->entries && index < bindings->len);
    binding_replace_value(&bindings->entries[index], value);
    return true;
  }
  if ( !bindings_reserve_one(bindings) ) return false;
  index = bindings->len;
  bindings->entries[index].variable = mt_keep(variable);
  bindings->entries[index].value = mt_keep(value);
  bindings->len++;
  binding_insert_slot(bindings->slots, bindings->slot_cap,
                      bindings->entries, index);
  return true;
}

/* Follow aliases without entering expressions. An acyclic path is compressed;
   len+1 followed bindings proves a cycle, which stays untouched and finite. */
static const mt_atom *binding_walk(mt_bindings *bindings,
                                   const mt_atom *atom)
{ const mt_atom *current = atom, *terminal;
  size_t followed = 0, i;

  while ( named_variable(current) )
  { size_t index = binding_index_atom(bindings, current);
    if ( index == NO_BINDING ) break;
    if ( followed >= bindings->len ) return current;
    current = bindings->entries[index].value;
    followed++;
  }
  terminal = current;
  current = atom;
  for (i = 0; i < followed; i++)
  { size_t index = binding_index_atom(bindings, current);
    const mt_atom *next;
    if ( index == NO_BINDING ) break;
    next = bindings->entries[index].value;
    if ( next != terminal )
      binding_replace_value(&bindings->entries[index], terminal);
    current = next;
  }
  return terminal;
}

typedef struct substitution_frame
{ const mt_atom *source;
  mt_atom      **kids;
  size_t         at;
  size_t         binding_index;
  bool           is_binding;
} substitution_frame;

typedef MT_STACK(substitution_frame) substitution_stack;

static void substitution_cleanup(substitution_stack *frames, mt_atom *value)
{ substitution_frame *frame;
  mt_drop(value);
  while ( (frame = stack_top(frames)) != NULL )
  { if ( !frame->is_binding && frame->kids )
    { size_t i;
      for (i = 0; i < frame->at; i++) mt_drop(frame->kids[i]);
      mt_free(frame->kids);
    }
    stack_pop(frames);
  }
  stack_free(frames);
}

/* One post-order mapper serves public one-pass substitution and the unifier's
   transitive normalization. Replacement values are followed only in the
   latter mode; active binding names cut no-occurs-check cycles, while resolved
   names cache completed subgraphs. Unchanged expressions retain their exact
   node and allocate no child array.
   [source: extensions/python/metta/atoms.py, _resolve_binding/_map_atoms;
   commit=e927fffde3a19d9927892bf64a7fc6202b866ae0] */
static mt_atom *substitute_impl(const mt_atom *root, mt_bindings *bindings,
                                bool follow_values, unsigned char *active,
                                unsigned char *resolved)
{ substitution_frame fixed[MT_WALK_FRAMES];
  substitution_stack frames;
  const mt_atom *current = root;
  mt_atom *value = NULL;
  bool producing = false;

  stack_init(&frames, fixed);
  for (;;)
  { if ( !producing )
    { size_t index = binding_index_atom(bindings, current);
      if ( index != NO_BINDING )
      { if ( !follow_values || resolved[index] )
        { value = mt_keep(bindings->entries[index].value);
          producing = true;
        } else if ( active[index] )
        { value = mt_keep(current);
          producing = true;
        } else
        { substitution_frame frame = {0};
          frame.is_binding = true;
          frame.binding_index = index;
          if ( !stack_push(&frames, frame) )
          { err_set(MT_NOMEM,
                    "out of memory following a nested substitution");
            substitution_cleanup(&frames, NULL);
            return NULL;
          }
          active[index] = 1;
          current = bindings->entries[index].value;
          continue;
        }
      } else if ( current->kind == MT_EXPR && current->u.e.n )
      { substitution_frame frame = {0};
        frame.source = current;
        if ( !stack_push(&frames, frame) )
        { err_set(MT_NOMEM,
                  "out of memory walking a nested substitution");
          substitution_cleanup(&frames, NULL);
          return NULL;
        }
        current = current->u.e.kids[0];
        continue;
      } else
      { value = mt_keep(current);
        producing = true;
      }
    }

    while ( producing )
    { substitution_frame *frame = stack_top(&frames);
      if ( !frame )
      { stack_free(&frames);
        return value;
      }

      if ( frame->is_binding )
      { size_t index = frame->binding_index;
        binding_replace_value(&bindings->entries[index], value);
        resolved[index] = 1;
        active[index] = 0;
        stack_pop(&frames);
        continue;
      }

      { size_t index = frame->at;
        const mt_atom *original = frame->source->u.e.kids[index];

        if ( frame->kids )
        { frame->kids[index] = value;
          value = NULL;
        } else if ( value != original )
        { size_t bytes, i;
          if ( !array_bytes(frame->source->u.e.n,
                            sizeof(*frame->kids), &bytes) ||
               !(frame->kids = mt_alloc(bytes)) )
          { mt_drop(value);
            value = NULL;
            err_set(MT_NOMEM,
                    "out of memory rebuilding a substituted expression");
            substitution_cleanup(&frames, NULL);
            return NULL;
          }
          for (i = 0; i < index; i++)
            frame->kids[i] = mt_keep(frame->source->u.e.kids[i]);
          frame->kids[index] = value;
          value = NULL;
        } else
        { /* `value` is the retain this walk just took on `original`, and the
             live source expression is another owner. Decrementing that known
             extra reference directly both preserves the source node and tells
             static analysis why no leaf can be freed here. */
          unsigned refs_before = MT_DEC(&value->refs);
          assert(refs_before > 1);
          (void)refs_before;
          value = NULL;
        }
        frame->at++;
      }

      if ( frame->at < frame->source->u.e.n )
      { current = frame->source->u.e.kids[frame->at];
        producing = false;
        break;
      }

      { const mt_atom *source = frame->source;
        mt_atom **kids = frame->kids;
        size_t count = source->u.e.n;
        stack_pop(&frames);
        if ( kids )
        { value = mt_exprv(count, kids);
          mt_free(kids);
          if ( !value )
          { substitution_cleanup(&frames, NULL);
            return NULL;
          }
        } else
          value = mt_keep(source);
      }
    }
  }
}

static bool bindings_normalize(mt_bindings *bindings)
{ unsigned char *active, *resolved;
  size_t i;

  if ( !bindings->len ) return true;
  active = mt_calloc(bindings->len, sizeof(*active));
  resolved = mt_calloc(bindings->len, sizeof(*resolved));
  if ( !active || !resolved )
  { mt_free(active);
    mt_free(resolved);
    err_set(MT_NOMEM, "out of memory normalizing a substitution");
    return false;
  }

  /* Direct aliases get the path-compression pass before expression values are
     rebuilt. This is what keeps a long x0=x1=...=a chain linear. */
  for (i = 0; i < bindings->len; i++)
    (void)binding_walk(bindings, bindings->entries[i].variable);

  for (i = 0; i < bindings->len; i++)
  { mt_atom *value;
    if ( resolved[i] ) continue;
    active[i] = 1;
    value = substitute_impl(bindings->entries[i].value, bindings, true,
                            active, resolved);
    active[i] = 0;
    if ( !value )
    { mt_free(active);
      mt_free(resolved);
      return false;
    }
    mt_drop(bindings->entries[i].value);
    bindings->entries[i].value = value;
    resolved[i] = 1;
  }
  mt_free(active);
  mt_free(resolved);
  return true;
}

/* Robinson's work-list unifier, matching the Python seat's last-child-first
   stack order and no-occurs-check behavior. `matched` distinguishes a clean
   structural mismatch from an allocation failure in this C implementation.
   [source: extensions/python/metta/atoms.py, _unify_symmetric;
   commit=e927fffde3a19d9927892bf64a7fc6202b866ae0] */
static bool unify_pair(mt_bindings *bindings, const mt_atom *left,
                       const mt_atom *right, bool *matched)
{ pair_frame fixed[MT_WALK_FRAMES];
  pair_stack frames;
  const mt_atom *x = left, *y = right;

  *matched = false;
  stack_init(&frames, fixed);
  for (;;)
  { x = binding_walk(bindings, x);
    y = binding_walk(bindings, y);

    if ( x == y ||
         (x->kind == y->kind && x->kind != MT_EXPR && eq_shallow(x, y)) )
      ;
    else if ( x->kind == MT_VARIABLE )
    { if ( named_variable(x) && !binding_put(bindings, x, y) )
      { stack_free(&frames);
        return false;
      }
    } else if ( y->kind == MT_VARIABLE )
    { if ( named_variable(y) && !binding_put(bindings, y, x) )
      { stack_free(&frames);
        return false;
      }
    } else if ( x->kind == MT_EXPR && y->kind == MT_EXPR &&
                x->u.e.n == y->u.e.n )
    { if ( x->u.e.n )
      { pair_frame *frame;
        if ( !pair_push(&frames, x, y, x->u.e.n) )
        { err_set(MT_NOMEM,
                  "out of memory unifying two nested expressions");
          stack_free(&frames);
          return false;
        }
        frame = stack_top(&frames);
        frame->at = frame->n - 1;
        x = frame->a->u.e.kids[frame->at];
        y = frame->b->u.e.kids[frame->at];
        continue;
      }
    } else
    { stack_free(&frames);
      return true;
    }

    for (;;)
    { pair_frame *frame = stack_top(&frames);
      if ( !frame )
      { *matched = true;
        stack_free(&frames);
        return true;
      }
      if ( frame->at )
      { frame->at--;
        x = frame->a->u.e.kids[frame->at];
        y = frame->b->u.e.kids[frame->at];
        break;
      }
      stack_pop(&frames);
    }
  }
}

mt_bindings *mt_unifyv(size_t count, const mt_atom *const *atoms)
{ mt_bindings *bindings;
  size_t i;

  if ( count < 2 )
    return err_null(MT_MISUSE,
                    "mt_unifyv needs at least two atoms; it was given %zu",
                    count);
  if ( !atoms )
    return err_null(MT_MISUSE,
                    "mt_unifyv was given a count and no atom array");
  for (i = 0; i < count; i++)
    if ( !atoms[i] )
      return err_null(MT_MISUSE,
                      "mt_unifyv atom %zu of %zu was NULL", i + 1, count);

  bindings = mt_calloc(1, sizeof(*bindings));
  if ( !bindings )
    return err_null(MT_NOMEM, "out of memory allocating a substitution");

  for (i = 1; i < count; i++)
  { bool matched;
    if ( !unify_pair(bindings, atoms[0], atoms[i], &matched) )
    { mt_bindings_free(bindings);
      return NULL;
    }
    if ( !matched )
    { mt_bindings_free(bindings);
      return NULL;
    }
  }
  if ( !bindings_normalize(bindings) )
  { mt_bindings_free(bindings);
    return NULL;
  }
  return bindings;
}

mt_bindings *mt_unify(const mt_atom *left, const mt_atom *right)
{ const mt_atom *atoms[2] = { left, right };
  return mt_unifyv(2, atoms);
}

size_t mt_bindings_len(const mt_bindings *bindings)
{ return bindings ? bindings->len : 0;
}

const mt_atom *mt_binding_var(const mt_bindings *bindings, size_t index)
{ return bindings && index < bindings->len
       ? bindings->entries[index].variable : NULL;
}

const mt_atom *mt_binding_value(const mt_bindings *bindings, size_t index)
{ return bindings && index < bindings->len
       ? bindings->entries[index].value : NULL;
}

const mt_atom *mt_binding(const mt_bindings *bindings, const char *name)
{ size_t index;
  if ( !bindings || !name )
  { err_set(MT_MISUSE, "mt_binding needs bindings and a variable name");
    return NULL;
  }
  index = binding_index_text(bindings, name, strlen(name));
  return index == NO_BINDING ? NULL : bindings->entries[index].value;
}

mt_atom *mt_substitute(const mt_atom *atom, const mt_bindings *bindings)
{ if ( !atom || !bindings )
    return err_null(MT_MISUSE,
                    "mt_substitute needs an atom and a substitution");
  return substitute_impl(atom, (mt_bindings *)bindings, false, NULL, NULL);
}

void mt_bindings_free(mt_bindings *bindings)
{ size_t i;
  if ( !bindings ) return;
  for (i = 0; i < bindings->len; i++)
  { mt_drop(bindings->entries[i].variable);
    mt_drop(bindings->entries[i].value);
  }
  mt_free(bindings->entries);
  mt_free(bindings->slots);
  mt_free(bindings);
}

#undef NO_BINDING
#undef MT_FNV64_OFFSET
#undef MT_FNV64_PRIME

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
{ mt_box_t *box = mt_calloc(1, sizeof(*box));
  if ( !box )
  { err_set(MT_NOMEM, "out of memory boxing a C value");
    return NULL;
  }
  box->refs = 1;
  box->value = value;
  box->release = release;
  box->apply = apply;
  box->user = user;
  if ( type_name && !(box->type = mt_strdup(type_name)) )
  { mt_free(box);
    err_set(MT_NOMEM, "out of memory copying a type name");
    return NULL;
  }
  return box;
}

mt_atom *mt_object(void *value, const char *type_name,
                           mt_free_fn release)
{ mt_box_t *box = box_new(value, type_name, release, NULL, NULL);
  if ( !box && release ) release(value);
  return box ? object_from_box(box) : NULL;
}

mt_atom *mt_function(mt_fn fn, void *user,
                             mt_free_fn release)
{ mt_box_t *box;
  if ( !fn )
  { if ( release ) release(user);
    err_set(MT_MISUSE, "mt_function needs a function, not NULL");
    return NULL;
  }
  box = box_new(user, "Function", release, fn, user);
  if ( !box && release ) release(user);
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

typedef struct mt_row_entry {
  MT_ATOMIC unsigned refs;
  mt_seam_row row;
} mt_row_entry;

struct metta
{ bool              open;
  uint64_t          generation;
  char             *path;
  bool              verbose;
  mt_limits    limits;
  size_t            initial_stack_bytes;
  predicate_t        space_operand;
  functor_t          equal_functor;
  functor_t          pair_functor;
  mt_op_entry_t *ops;
  size_t            nops, cap_ops;
  mt_point         *points;
  size_t            npoints, cap_points;
  mt_row_entry   **rows;
  size_t            nrows, cap_rows;
  void            **handles;      /* every dlopen'd extension, kept open */
  size_t            nhandles, cap_handles;
};

static struct metta g_runtime;
static bool         g_open = false;
static bool         g_cleanup_failed = false;
static uint64_t     g_runtime_generation;
typedef struct transaction_frame transaction_frame;
static MT_TLS transaction_frame *g_transaction;
static bool registry_writable(const char *door);

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

#ifdef MT_TEST_FAULTS
/* Inspect only the ownership boundary exercised by the restart regression;
   this symbol is absent from the installed library
   [tested: test_restart_replaces_runtime_owned_predicates;
   commit=802878f86f478c23fc05f7e68cbe605160eedb59]. */
void *mt_test_cached_space_predicate(void)
{ return g_runtime.space_operand;
}
#endif

/* Every door that reaches the engine asks this first. SWI's foreign-frame
   API reads the calling thread's Prolog environment, so a call made before
   mt_open(), or after mt_close(), does not fail: it SEGFAULTS inside
   PL_open_foreign_frame. Sixteen of cmetta.h's doors died that way, from
   mt_parse to mt_stats_now, and calling a door before the constructor is the
   most ordinary mistake a new caller makes
   [measured 2026-08-31; C34 in ai-cmetta-c-constraints.md]. The header
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
{ size_t len = 0;
  mt_box_t *box = PL_blob_data(a, &len, NULL);
  (void)flags;
  /* What it IS, never what it contains: a live value that printed its
     contents would have become text, which is the one thing it must not do. */
  if ( !box || len != sizeof(*box) ) Sfprintf(s, "<released-cvalue>");
  else Sfprintf(s, "<%s>", box->type ? box->type : "cvalue");
  return TRUE;
}

static int object_release_blob(atom_t a)
{ size_t len = 0;
  mt_box_t *box = PL_blob_data(a, &len, NULL);
  if ( box && len == sizeof(*box) ) box_release(box);
  return TRUE;
}

static void object_acquire_blob(atom_t a)
{ size_t len = 0;
  mt_box_t *box = PL_blob_data(a, &len, NULL);
  if ( box && len == sizeof(*box) ) MT_INC(&box->refs);
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
         (copy = mt_alloc(len + 1)) )
    { memcpy(copy, s, len);
      copy[len] = '\0';
      if ( len_out ) *len_out = len;
    }
  }
  PL_STRINGS_RELEASE()
  return copy;
}

static mt_status call_bridge(const char *name, int arity, term_t av);
/* The seam lives below `Publishing C functions`, where its doors read best
   beside mt_def(); mt_close() and the boot's foreign registrations are above
   it and reach it through these. */
static void seam_release(metta *runtime);
static bool seam_declare_shipped(metta *runtime);
static foreign_t pl_cmetta_repr(term_t object, term_t out);
static foreign_t pl_cmetta_provider(term_t space, term_t operation,
                                    term_t payload, term_t result);
static foreign_t pl_cmetta_provider_query(term_t space, term_t args,
                                         term_t result, control_t control);
static foreign_t pl_cmetta_provider_identity(term_t space, term_t identity);
static foreign_t pl_cmetta_provider_capture(term_t space, term_t identity, term_t held);
static foreign_t pl_cmetta_provider_finish(term_t held, term_t operation);
static foreign_t pl_cmetta_notify(term_t name, term_t token, term_t added, term_t atom);
static void row_release(mt_row_entry *entry);
static foreign_t pl_cmetta_stream(term_t stream, term_t result, control_t control);
static foreign_t pl_cmetta_tx_body(term_t ticket);
static foreign_t pl_cmetta_tx_outcome(term_t ticket, term_t committed);

/* Whether this atom is a space, asked of the engine and of the term itself:
   no text conversion, and no list of names to rebuild per answer.
   metta_c_space_operand/1 is metta_space_operand/1, the test the engine's own
   get_type_candidate/2 consults before answering SpaceType, so this seat and
   the Python seat classify one atom alike. get-metatype asks a different
   question since 2026-09-05, upstream PeTTa's one about whether a function
   carries the name. Asking the engine per atom is a question only an
   in-process seat can afford, and it is the reason this seat exists.

   The predicate is a test over a bound atom and cannot throw, so a plain
   call is enough; a failure is the answer "no" rather than an error.

   The handle is resolved once per runtime. PL_predicate() interns the name
   and walks the module's procedure table on every call, and this runs once
   per decoded atom: caching it takes the question from 3,358 to 2,208
   instructions per atom [measured 2026-08-27, perf stat -e instructions:u,
   minimum of three runs of kit/driver over 500 programs answering 40 symbols each:
   3,018,075,923 asking nothing, 3,085,234,884 resolving per call,
   3,062,228,470 resolving once, so 1,150 saved of 3,358 and +1.46% over
   asking nothing on a workload that is nothing but symbol decoding].
   PL_cleanup() invalidates predicate handles, so the runtime owns this one
   and mt_open() resolves it again after every successful restart. */
static bool is_space(term_t t)
{ fid_t f;
  int rc;

  if ( !g_runtime.space_operand )
  { err_set(MT_ERROR,
            "the runtime has no metta_c_space_operand/1 predicate handle");
    return false;
  }
  if ( !(f = frame_open("decoding a symbol")) ) return false;
  rc = PL_call_predicate(NULL, PL_Q_NORMAL, g_runtime.space_operand, t);
  PL_discard_foreign_frame(f);
  return rc == TRUE;
}

/* The source name of a variable, from the engine's Name=Var pairs. */
static char *variable_name(term_t names, term_t var)
{ term_t head, tail;
  if ( !names ) return term_text(var, CVT_WRITE, NULL);

  head = PL_new_term_ref();
  tail = PL_copy_term_ref(names);
  while ( PL_get_list(tail, head, tail) )
  { term_t nm = PL_new_term_ref();
    term_t vr = PL_new_term_ref();
    /* Both Name=Var and Name-Var are read: the engine's name state uses one
       and a reader's variable_names uses the other. */
    if ( (PL_is_functor(head, g_runtime.equal_functor) ||
          PL_is_functor(head, g_runtime.pair_functor)) &&
         PL_get_arg(1, head, nm) && PL_get_arg(2, head, vr) &&
         PL_compare(vr, var) == 0 )
      return term_text(nm, CVT_ATOM | CVT_STRING, NULL);
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
      mt_free(text);
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
    mt_free(name);
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
    mt_free(text);
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
    size_t blob_len;
    PL_blob_t *type;
    if ( PL_get_blob(t, &blob, &blob_len, &type) &&
         !(type->flags & PL_BLOB_TEXT) )
    { size_t len;
      char *text;
      mt_atom *a;
      if ( type == &mt_object_blob )
      { mt_box_t *box = blob;
        if ( !box || blob_len != sizeof(*box) )
        { err_set(MT_UNSUPPORTED,
                  "a C object in the engine was explicitly released; its "
                  "remaining Prolog aliases are invalid");
          return NULL;
        }
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
      mt_free(text);
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
      mt_free(text);
      return a;
    }
    a = atom_text(is_space(t) ? MT_SPACE : MT_SYMBOL,
                  text, len);
    mt_free(text);
    return a;
  }

  { size_t len;
    char *text = term_text(t, CVT_WRITE, &len);
    err_set(MT_UNSUPPORTED,
            "the engine answered %s, which is a Prolog term with no MeTTa "
            "reading; this binding refuses it rather than turning it into a "
            "symbol that cannot go home again",
            text ? text : "a term this binding could not even print");
    mt_free(text);
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
  { size_t cap, bytes;
    mt_atom **grown;
    if ( !next_capacity(f->cap, 4, sizeof(*grown), &cap, &bytes) )
      return false;
    grown = mt_resize(f->kids, bytes);
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
    mt_free(f->kids);
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
    mt_free(f->kids);
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
  for (i = 0; i < ctx->n; i++) mt_free(ctx->names[i]);
  mt_free(ctx->names);
  mt_free(ctx->vars);
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
  { size_t cap, name_bytes, var_bytes;
    char **nn;
    term_t *vv;
    if ( !next_capacity(ctx->cap, 4, sizeof(*nn), &cap, &name_bytes) ||
         !array_bytes(cap, sizeof(*vv), &var_bytes) )
      return false;
    nn = mt_alloc(name_bytes);
    vv = mt_alloc(var_bytes);
    if ( !nn || !vv ) { mt_free(nn); mt_free(vv); return false; }
    if ( ctx->n )
    { memcpy(nn, ctx->names, ctx->n * sizeof(*nn));
      memcpy(vv, ctx->vars, ctx->n * sizeof(*vv));
    }
    mt_free(ctx->names);
    mt_free(ctx->vars);
    ctx->names = nn;
    ctx->vars = vv;
    ctx->cap = cap;
  }
  if ( !(ctx->names[ctx->n] = mt_strdup(name)) ) return false;
  ctx->vars[ctx->n] = PL_copy_term_ref(out);
  ctx->n++;
  return true;
}

static mt_status ball_status(record_t saved, const char *name, int arity);

/* Validate before SWI's replacement decoder can silently change malformed
   input. Width and second-byte bounds follow RFC 3629 section 4, including
   shortest encodings, surrogate exclusion and the U+10FFFF ceiling.
   [source: https://www.rfc-editor.org/rfc/rfc3629.txt; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c]
   Time: Θ(n) byte inspections, n = length. Space: O(1). */
static bool valid_utf8(const char *text, size_t length)
{ const unsigned char *bytes = (const unsigned char *)text;
  for (size_t i = 0; i < length; )
  { unsigned lead = bytes[i];
    size_t width;
    if ( lead < 0x80 ) { i++; continue; }
    width = lead >= 0xc2 && lead <= 0xdf ? 2 :
            lead >= 0xe0 && lead <= 0xef ? 3 :
            lead >= 0xf0 && lead <= 0xf4 ? 4 : 0;
    bool valid = width && width <= length - i;
    for (size_t j = 1; valid && j < width; j++)
      valid = bytes[i + j] >= 0x80 && bytes[i + j] <= 0xbf;
    if ( valid )
    { unsigned second = bytes[i + 1];
      valid = !(lead == 0xe0 && second < 0xa0) &&
              !(lead == 0xed && second > 0x9f) &&
              !(lead == 0xf0 && second < 0x90) &&
              !(lead == 0xf4 && second > 0x8f);
    }
    if ( !valid )
    { err_set(MT_MISUSE, "invalid UTF-8 at byte %zu", i); return false; }
    i += width;
  }
  return true;
}

/* Logical text is UTF-8; filenames use SWI's platform representation. The
   legacy byte constructors interpret UTF-8 as Latin-1. Preserve a conversion
   exception before another FLI call can overwrite it.
   [tested: tests/test_native_parity.c; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c] */
static bool put_chars(term_t out, int flags, size_t length, const char *text)
{ if ( length == (size_t)-1 ) length = strlen(text);
  if ( (flags & REP_UTF8) && !valid_utf8(text, length) ) return false;
  if ( PL_put_chars(out, flags, length, text) ) return true;
  term_t exception = PL_exception(0);
  record_t saved = exception ? PL_record(exception) : 0;
  PL_clear_exception();
  if ( saved ) ball_status(saved, "PL_put_chars", 4);
  else err_set(MT_NOMEM, "the engine could not hold the text");
  return false;
}

static bool put_name(term_t out, const char *name)
{ return put_chars(out, PL_ATOM | REP_UTF8, (size_t)-1, name); }

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
      return put_chars(out, PL_ATOM | REP_UTF8, a->u.t.len, a->u.t.text);
    case MT_TEXT:
      return put_chars(out, PL_STRING | REP_UTF8, a->u.t.len, a->u.t.text);
    case MT_VARIABLE:
      return encode_var(ctx, a->u.t.text, out);
    case MT_INT:
      return PL_put_int64(out, a->u.i);
    case MT_FLOAT:
      return PL_put_float(out, a->u.f);
    case MT_BOOL:
      return put_name(out, a->u.b ? "true" : "false");
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
         "the same C object is one engine identity across store, match and delete";
         commit=2e13376bb6e1662655525533a1ab02800940aec5] */
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
    ok = put_name(name, ctx.names[i - 1]) &&
         PL_cons_functor(pair, g_runtime.pair_functor,
                         name, ctx.vars[i - 1]) &&
         PL_cons_list(names, pair, names);
  }
  encode_ctx_free(&ctx);
  return ok;
}

bool mt_object_free(mt_atom *atom)
{ fid_t f = 0;
  term_t t = 0;
  atom_t blob = 0;
  bool released = false;

  if ( !atom )
  { err_set(MT_MISUSE, "mt_object_free was given NULL");
    return false;
  }
  if ( atom->kind != MT_OBJECT )
  { err_set(MT_MISUSE, "mt_object_free needs an object atom, not %s",
            mt_kind_str(atom->kind));
    mt_drop(atom);
    return false;
  }

  /* PL_cleanup() has already released every registered blob. What remains is
     the C reference, and consuming it is both sufficient and safe without an
     FLI engine [tested: test_an_object_can_be_released_without_waiting_for_atom_gc;
     commit=1bd77577f263374ada1452e47bbc8dea17319d62]. */
  if ( !g_open )
  { mt_drop(atom);
    return true;
  }

  if ( !(f = frame_open("mt_object_free")) ) goto done;
  t = PL_new_term_ref();
  if ( !t || !put_atom(atom, t) || !PL_get_atom(t, &blob) )
  { if ( mt_ok() )
      err_set(MT_ERROR, "the engine could not identify the object's blob");
    goto done;
  }
  /* A unique NOCOPY blob has one engine reference regardless of how many
     terms contain it. PL_free_blob() releases that reference now and makes
     surviving Prolog aliases explicitly invalid; blob_box() and decode_leaf()
     reject those aliases before touching their former payload
     [tested: test_an_object_can_be_released_without_waiting_for_atom_gc;
     commit=1bd77577f263374ada1452e47bbc8dea17319d62]. */
  if ( !PL_free_blob(blob) )
  { err_set(MT_ERROR, "the engine refused to release the object's blob");
    goto done;
  }
  released = true;

done:
  frame_close(f);
  mt_drop(atom);
  return released;
}

/* ================================================================== *
 * Calling the bridge
 * ================================================================== */

/* What to DO about a ball, and what says so, from the engine's own (refusal
   ...) catalog row for the kind that ball is. Asked after the message is in
   place, because err_copy() clears the advice; the strings are the row's own,
   rendered by the engine with this ball's fields already in them, so the three
   seats say one sentence about one refusal rather than three.

   Silent when the bridge does not answer: a ball whose kind carries no row
   leaves both empty and mt_remedy() answers NULL, which is what a caller had
   before this existed. */
static void advise_ball(term_t ball)
{ fid_t f = PL_open_foreign_frame();
  term_t av;
  predicate_t p = PL_predicate("metta_c_error_advice", 3, "user");
  qid_t q;

  /* PL_open_foreign_frame() rather than frame_open(): this runs with the
     engine's own message already recorded, and frame_open()'s own failure
     report would replace it with one about a frame. */
  if ( !f ) return;
  av = PL_new_term_refs(3);

  if ( av && PL_unify(av, ball) &&
       (q = PL_open_query(NULL, PL_Q_CATCH_EXCEPTION, p, av)) )
  { if ( PL_next_solution(q) == TRUE )
    { char *remedy = term_text(av + 1, CVT_ATOM | CVT_STRING, NULL);
      char *ground = term_text(av + 2, CVT_ATOM | CVT_STRING, NULL);
      err_advice(remedy, ground);
      mt_free(remedy);
      mt_free(ground);
    }
    PL_cut_query(q);
  }
  PL_discard_foreign_frame(f);
}

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
        mt_free(text);
        PL_cut_query(q);
        PL_discard_foreign_frame(f);
        advise_ball(ball);
        return;
      }
    }
    PL_cut_query(q);
  }
  { char *text = term_text(ball, CVT_WRITE, NULL);
    err_copy(MT_ERROR,
             text ? text : "the engine raised a term this binding could not print");
    mt_free(text);
  }
  PL_discard_foreign_frame(f);
  advise_ball(ball);
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
  mt_iterator         iterator;
};

/* A callback context belongs to one invocation, but a host helper can still
   be handed NULL or retain a stale nullable slot. Every public callback door
   refuses the missing context through the same errno-shaped channel instead
   of reading it [tested: test_a_door_that_takes_an_atom_refuses_null;
   commit=acf9c5e7503903ead353176c0a6e116bbf23f07a]. */
static bool call_given(const mt_call *call, const char *door)
{ if ( call ) return true;
  err_set(MT_MISUSE, "%s was given a NULL operation call", door);
  return false;
}

size_t mt_arity(const mt_call *call)
{ return call_given(call, "mt_arity") ? call->arity : 0;
}

const mt_atom *mt_arg(const mt_call *call, size_t index)
{ return call_given(call, "mt_arg") && index < call->arity
       ? call->args[index] : NULL;
}

metta *mt_of(const mt_call *call)
{ return call_given(call, "mt_of") ? call->runtime : NULL;
}

mt_status mt_answer(mt_call *call, mt_atom *atom)
{ if ( !call_given(call, "mt_answer") )
  { mt_drop(atom);
    return MT_MISUSE;
  }
  if ( call->answered )
  { mt_drop(atom);
    return err_set(MT_MISUSE,
                   "this application already answered; use mt_answer_iter for multiple answers");
  }
  if ( !atom ) return err_set(MT_MISUSE, "cannot answer with a NULL atom");
  call->result = atom;
  call->answered = true;
  return MT_OK;
}

mt_status mt_answer_iter(mt_call *call, mt_iterator iterator)
{ if ( !call_given(call, "mt_answer_iter") || call->answered || !iterator.next )
  { if ( iterator.close ) iterator.close(iterator.state);
    return err_set(MT_MISUSE, "mt_answer_iter needs an unanswered call and a next callback");
  }
  call->iterator = iterator;
  call->answered = true;
  return MT_OK;
}

static void iterator_close(mt_iterator *iterator)
{ mt_iterator held = *iterator;
  *iterator = (mt_iterator){0};
  if ( held.close ) held.close(held.state);
}

/* Validate the ownership/status boundary once for native and engine cursors. */
static mt_status iterator_next(mt_iterator *iterator, mt_atom **answer)
{ uint64_t before = g_error_generation;
  mt_status status;
  *answer = NULL;
  status = iterator->next(iterator->state, answer);
  if ( status == MT_ROW && *answer ) return status;
  if ( status == MT_DONE && !*answer ) return status;
  mt_drop(*answer);
  *answer = NULL;
  if ( status < MT_ERROR || status > MT_LIMIT )
    return err_set(MT_MISUSE, "iterator status and answer disagree");
  if ( before == g_error_generation )
    err_set(status, "C iterator failed with status %s", mt_status_str(status));
  return status;
}

/* Returns MT_ERROR so an op can spell its refusal as one line:
       if ( !mt_ok() ) return mt_fail(call, "wanted two numbers"); */
mt_status mt_fail(mt_call *call, const char *message)
{ if ( !call_given(call, "mt_fail") ) return MT_MISUSE;
  snprintf(call->error, sizeof(call->error), "%s",
           message ? message : "the C function refused this application");
  call->failed = true;
  return MT_ERROR;
}

/* SWI contexts own suspended C iterators. Prune receives no usable term
   arguments, so every release is driven only by the saved context.
   https://www.swi-prolog.org/pldoc/man?section=foreign-control
   Time: O(A) decoding and cleanup, A argument atoms plus their descendants;
   each resume costs the producer's next operation and one atom encoding.
   [tested: tests/test_iterators.c; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c] */
typedef struct native_call {
  mt_call call;
  char *name;
  mt_box_t *owner;
  mt_row_entry *registration;
} native_call;

static void native_call_free(native_call *held)
{ iterator_close(&held->call.iterator);
  mt_drop(held->call.result);
  for (size_t i = 0; i < held->call.arity; i++) mt_drop(held->call.args[i]);
  mt_free(held->call.args);
  mt_free(held->name);
  box_release(held->owner);
  if ( held->registration ) row_release(held->registration);
  mt_free(held);
}

static mt_status native_iterator_next(void *state, mt_atom **answer)
{ native_call *held = state;
  return iterator_next(&held->call.iterator, answer);
}

static void native_iterator_close(void *state)
{ native_call_free(state);
}

static foreign_t callback_error(const char *name, const char *why)
{ term_t ball = PL_new_term_ref();
  if ( PL_unify_term(ball,
        PL_FUNCTOR_CHARS, "error", 2,
          PL_FUNCTOR_CHARS, "cmetta_operation_failed", 2,
            PL_UTF8_CHARS, name,
            PL_UTF8_CHARS, why,
          PL_FUNCTOR_CHARS, "context", 2,
            PL_UTF8_CHARS, name,
            PL_VARIABLE) )
    PL_raise_exception(ball);
  return FALSE;
}

static foreign_t native_call_error(native_call *held, uint64_t before)
{ return callback_error(held->name, held->call.failed ? held->call.error
                  : (g_error_generation != before && mt_errmsg() ? mt_errmsg()
                     : "the C function answered nothing"));
}

static foreign_t native_resume(native_call *held, term_t result)
{ mt_atom *answer = NULL;
  mt_status status;
  foreign_t rc = FALSE;
  uint64_t before = g_error_generation;
  /* A bound result may reject one generated value; backtrack locally until
     a value unifies. Each failed unification gets its own discarded frame. */
  for (;;)
  { fid_t frame;
    term_t out;
    status = iterator_next(&held->call.iterator, &answer);
    if ( status != MT_ROW ) break;
    frame = PL_open_foreign_frame();
    out = frame ? PL_new_term_ref() : 0;
    if ( !out || !put_atom(answer, out) )
    { mt_drop(answer);
      frame_close(frame);
      status = mt_error() >= MT_ERROR ? mt_error() : MT_NOMEM;
      break;
    }
    mt_drop(answer);
    answer = NULL;
    if ( PL_unify(result, out) )
    { PL_close_foreign_frame(frame);
      PL_retry_address(held);
    }
    PL_discard_foreign_frame(frame);
    if ( PL_exception(0) ) { status = MT_ERROR; break; }
  }
  if ( status != MT_DONE ) rc = native_call_error(held, before);
  native_call_free(held);
  return rc;
}

static foreign_t native_continue(term_t result, control_t control)
{ native_call *held = PL_foreign_context_address(control);
  if ( PL_foreign_control(control) == PL_PRUNED )
  { native_call_free(held);
    return TRUE;
  }
  return native_resume(held, result);
}

static foreign_t run_call(const char *name, mt_fn fn, void *user,
                          mt_box_t *owner, mt_row_entry *registration,
                          term_t args, term_t result)
{ native_call *held;
  mt_call *call;
  term_t head = PL_new_term_ref(), tail = PL_copy_term_ref(args);
  size_t count = 0;
  mt_status status;
  foreign_t rc = FALSE;
  uint64_t before;

  if ( PL_skip_list(args, 0, &count) != PL_LIST )
    return PL_type_error("list", args);
  held = mt_calloc(1, sizeof(*held));
  if ( !held ) return PL_resource_error("memory");
  call = &held->call;
  held->name = mt_strdup(name);
  call->args = count ? mt_calloc(count, sizeof(*call->args)) : NULL;
  if ( !held->name || (count && !call->args) )
  { native_call_free(held);
    return PL_resource_error("memory");
  }
  held->owner = owner;
  if ( owner ) MT_INC(&owner->refs);
  held->registration = registration;
  if ( registration ) MT_INC(&registration->refs);
  call->runtime = &g_runtime;
  while ( PL_get_list(tail, head, tail) )
  { mt_atom *atom = decode(head, 0);
    if ( !atom )
    { rc = PL_permission_error("read", "argument", head);
      goto done;
    }
    call->args[call->arity++] = atom;
  }
  before = g_error_generation;
  status = fn(call, user);
  if ( status == MT_OK && call->answered && !call->failed )
  { if ( call->iterator.next )
    { if ( owner )
      { mt_atom *stream = mt_stream((mt_iterator){held, native_iterator_next,
                                                  native_iterator_close});
        term_t out = PL_new_term_ref();
        rc = stream && put_atom(stream, out) && PL_unify(result, out);
        mt_drop(stream);
        return rc;
      }
      return native_resume(held, result);
    }
    term_t out = PL_new_term_ref();
    rc = put_atom(call->result, out) && PL_unify(result, out);
  } else if ( status != MT_FAIL || call->failed )
    rc = native_call_error(held, before);
done:
  native_call_free(held);
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

static foreign_t pl_cmetta_dispatch(term_t name, term_t args, term_t result,
                                    control_t control)
{ char *text;
  size_t len;
  mt_op_entry_t *op;
  size_t arity = 0;
  foreign_t rc;

  if ( PL_foreign_control(control) != PL_FIRST_CALL )
    return native_continue(result, control);
  if ( PL_skip_list(args, 0, &arity) != PL_LIST )
    return PL_type_error("list", args);
  if ( !(text = term_text(name, CVT_ATOM | CVT_STRING, &len)) )
    return PL_type_error("atom", name);

  op = find_op(text, arity);
  if ( !op )
  { rc = PL_existence_error("cmetta_operation", name);
    mt_free(text);
    return rc;
  }
  { foreign_t answered = run_call(op->name, op->fn, op->user, NULL, NULL, args, result);
    mt_free(text);
    return answered;
  }
}

static mt_box_t *blob_box(term_t t)
{ void *blob;
  size_t len;
  PL_blob_t *type;
  if ( PL_get_blob(t, &blob, &len, &type) && type == &mt_object_blob )
  { if ( blob && len == sizeof(mt_box_t) ) return blob;
    PL_existence_error("cmetta_object", t);
  }
  return NULL;
}

static foreign_t pl_cmetta_object_callable(term_t t)
{ mt_box_t *box = blob_box(t);
  return ( box && box->apply ) ? TRUE : FALSE;
}

static foreign_t pl_cmetta_object_live(term_t t)
{ return blob_box(t) ? TRUE : FALSE; }

/* The box owns the type name; the engine owns type inference and dispatch.
   [tested: test_native_object_types_reach_engine_dispatch; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c] */
static foreign_t pl_cmetta_object_type(term_t t, term_t type)
{ mt_box_t *box = blob_box(t);
  term_t name;
  if ( !box || !box->type ) return FALSE;
  name = PL_new_term_ref();
  if ( !name ) return PL_resource_error("memory");
  if ( !put_name(name, box->type) ) return callback_error("mt_object type", mt_errmsg());
  return PL_unify(type, name);
}

static foreign_t pl_cmetta_apply(term_t t, term_t args, term_t result,
                                 control_t control)
{ size_t arity;
  mt_box_t *box;

  if ( control && PL_foreign_control(control) != PL_FIRST_CALL )
    return native_continue(result, control);
  if ( PL_skip_list(args, 0, &arity) != PL_LIST )
    return PL_type_error("list", args);
  (void)arity;
  box = blob_box(t);
  if ( !box || !box->apply ) return FALSE;
  return run_call(box->type ? box->type : "function",
                  box->apply, box->user, box, NULL, args, result);
}

/* ================================================================== *
 * Boot
 * ================================================================== */

static bool goal(const char *text)
{ fid_t f = frame_open("running an engine goal");
  term_t t;
  mt_status status;

  if ( !f ) return false;
  t = PL_new_term_ref();
  if ( !t )
    status = err_set(MT_NOMEM, "out of memory holding an engine goal");
  else if ( !PL_chars_to_term(text, t) )
  { term_t ex = PL_exception(0);
    if ( ex )
    { record_t saved = PL_record(ex);
      PL_clear_exception();
      status = saved ? ball_status(saved, "reading an engine goal", 1)
                     : err_set(MT_ERROR,
                               "the engine goal was invalid and its exception "
                               "could not be copied");
    } else
      status = err_set(MT_ERROR, "the engine goal could not be read");
  } else
    status = call_bridge("call", 1, t);
  PL_discard_foreign_frame(f);
  return status == MT_OK;
}

/* Pass one atom argument as term data. Engine paths used to be pasted into a
   quoted Prolog term, so an ordinary apostrophe in a directory name changed
   the term rather than naming the directory
   [tested: test_engine_path_is_passed_as_data;
   commit=2ed500695e8c9ecefaeeaa2b3fd30e4fef32a8e2]. SWI defines
   REP_FN as the platform's filename representation, rather than the legacy
   byte interpretation used by PL_put_atom_chars
   [source: https://github.com/SWI-Prolog/swipl-devel/blob/dec2acf760a8571381fb6b554438bd7d90c8cacf/src/SWI-Prolog.h#L969-L981;
   commit=2ed500695e8c9ecefaeeaa2b3fd30e4fef32a8e2]. */
static bool goal_atom(const char *name, const char *value)
{ fid_t f = frame_open("running an engine goal with an atom argument");
  term_t av;
  mt_status status;

  if ( !f ) return false;
  av = PL_new_term_refs(1);
  if ( !av )
    status = err_set(MT_NOMEM, "out of memory holding the argument for %s/1",
                     name);
  else if ( !PL_put_chars(av, PL_ATOM | REP_FN, (size_t)-1, value) )
  { term_t ex = PL_exception(0);
    if ( ex )
    { record_t saved = PL_record(ex);
      PL_clear_exception();
      status = saved ? ball_status(saved, name, 1)
                     : err_set(MT_ERROR,
                               "%s/1 rejected its filename argument and its "
                               "exception could not be copied", name);
    } else
      status = err_set(MT_NOMEM,
                       "the filename argument for %s/1 could not be copied",
                       name);
  }
  else
    status = call_bridge(name, 1, av);
  frame_close(f);
  return status == MT_OK;
}

/* Read and write a size-valued Prolog flag as a bound term. Besides avoiding
   source interpolation, these helpers keep the size_t/Prolog-integer boundary
   explicit on platforms where their ranges differ. */
static bool prolog_size_flag(const char *name, size_t *value)
{ fid_t f = frame_open("reading a Prolog size flag");
  term_t av;
  uint64_t wide;
  mt_status status;

  if ( !f ) return false;
  av = PL_new_term_refs(2);
  if ( !av || !put_name(av, name) )
    status = err_set(MT_NOMEM, "out of memory reading Prolog flag %s", name);
  else
    status = call_bridge("current_prolog_flag", 2, av);
  if ( status == MT_OK && !PL_get_uint64(av + 1, &wide) )
    status = err_set(MT_ERROR, "Prolog flag %s was not an unsigned integer",
                     name);
  if ( status == MT_OK && sizeof(size_t) < sizeof(wide) && wide > SIZE_MAX )
    status = err_set(MT_ERROR,
                     "Prolog flag %s exceeds this platform's size_t range",
                     name);
  if ( status == MT_OK ) *value = (size_t)wide;
  frame_close(f);
  return status == MT_OK;
}

static bool set_prolog_size_flag(const char *name, size_t value)
{ fid_t f = frame_open("setting a Prolog size flag");
  term_t av;
  mt_status status;

  if ( !f ) return false;
  av = PL_new_term_refs(2);
  if ( sizeof(size_t) > sizeof(uint64_t) && value > (size_t)UINT64_MAX )
    status = err_set(MT_UNSUPPORTED,
                     "requested Prolog flag %s exceeds uint64_t", name);
  else if ( !av || !put_name(av, name) ||
            !PL_put_uint64(av + 1, (uint64_t)value) )
    status = err_set(MT_NOMEM,
                     "could not represent the requested Prolog flag %s",
                     name);
  else
    status = call_bridge("set_prolog_flag", 2, av);
  frame_close(f);
  return status == MT_OK;
}

static char *default_path(void)
{ const char *env = getenv("METTA_PATH");
  return mt_strdup(env && *env ? env : MT_ENGINE_PATH);
}

metta *mt_open(const mt_config *config)
{ static char *argv[] = { (char *)"cmetta", (char *)"-q",
                          (char *)"--no-signals", NULL };
  mt_config defaults = {0};
  char *path;
  char *buf;
  size_t bufsz;
  size_t initial_stack_bytes;
  functor_t equal_functor, pair_functor;

  if ( !config ) config = &defaults;

  if ( g_cleanup_failed )
    return err_null(MT_ERROR,
                    "a previous SWI-Prolog cleanup could not reclaim the "
                    "runtime; this process cannot safely initialise it again");

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

  path = config->path ? mt_strdup(config->path) : default_path();
  if ( !path ) return err_null(MT_NOMEM, "out of memory recording the path");

  if ( !PL_is_initialised(NULL, NULL) && !PL_initialise(3, argv) )
  { mt_free(path);
    return err_null(MT_ERROR, "SWI-Prolog would not initialise");
  }

  /* Registered BEFORE the consult, because engine/metta.pl reads
     extensions/ * /extension.pl while it loads and this seat's control file
     declares needs(predicate('$mt_present'/0)). */
  PL_register_foreign("$cmetta_present", 0,
                      as_pl_function((mt_anyfn)pl_cmetta_present), 0);
  PL_register_foreign("$cmetta_dispatch", 3,
                      as_pl_function((mt_anyfn)pl_cmetta_dispatch), PL_FA_NONDETERMINISTIC);
  PL_register_foreign("$cmetta_object_callable", 1,
                      as_pl_function((mt_anyfn)pl_cmetta_object_callable), 0);
  PL_register_foreign("$cmetta_object_live", 1,
                      as_pl_function((mt_anyfn)pl_cmetta_object_live), 0);
  PL_register_foreign("$cmetta_object_type", 2,
                      as_pl_function((mt_anyfn)pl_cmetta_object_type), 0);
  PL_register_foreign("$cmetta_apply", 3,
                      as_pl_function((mt_anyfn)pl_cmetta_apply), PL_FA_NONDETERMINISTIC);
  PL_register_foreign("$cmetta_repr", 2,
                      as_pl_function((mt_anyfn)pl_cmetta_repr), 0);
  PL_register_foreign("$cmetta_provider", 4,
                      as_pl_function((mt_anyfn)pl_cmetta_provider), 0);
  PL_register_foreign("$cmetta_provider_query", 3,
                      as_pl_function((mt_anyfn)pl_cmetta_provider_query), PL_FA_NONDETERMINISTIC);
  PL_register_foreign("$cmetta_provider_identity", 2,
                      as_pl_function((mt_anyfn)pl_cmetta_provider_identity), 0);
  PL_register_foreign("$cmetta_provider_capture", 3,
                      as_pl_function((mt_anyfn)pl_cmetta_provider_capture), 0);
  PL_register_foreign("$cmetta_provider_finish", 2,
                      as_pl_function((mt_anyfn)pl_cmetta_provider_finish), 0);
  PL_register_foreign("$cmetta_notify", 4,
                      as_pl_function((mt_anyfn)pl_cmetta_notify), 0);
  PL_register_foreign("$cmetta_stream", 2,
                      as_pl_function((mt_anyfn)pl_cmetta_stream), PL_FA_NONDETERMINISTIC);
  PL_register_foreign("$cmetta_tx_body", 1,
                      as_pl_function((mt_anyfn)pl_cmetta_tx_body), 0);
  PL_register_foreign("$cmetta_tx_outcome", 2,
                      as_pl_function((mt_anyfn)pl_cmetta_tx_outcome), 0);
  PL_register_blob_type(&mt_object_blob);

  if ( !prolog_size_flag("stack_limit", &initial_stack_bytes) )
  { mt_free(path);
    return NULL;
  }
  equal_functor = PL_new_functor(PL_new_atom("="), 2);
  pair_functor = PL_new_functor(PL_new_atom("-"), 2);
  if ( !equal_functor || !pair_functor )
  { mt_free(path);
    return err_null(MT_NOMEM,
                    "out of memory caching the engine's pair functors");
  }

  bufsz = strlen(path) + 128;
  if ( !(buf = mt_alloc(bufsz)) )
  { mt_free(path);
    return err_null(MT_NOMEM, "out of memory building the boot goals");
  }

  if ( config->stack_limit )
  { if ( !set_prolog_size_flag("stack_limit", config->stack_limit) )
    { mt_free(path); mt_free(buf);
      return NULL;
    }
  }

  /* `extensions` opts the engine into reading extensions/ * /extension.pl,
     and `silent` is how a host with no command line asks for quiet, because
     engine/filereader.pl reads argv at load time [C2]. */
  if ( !goal(config->verbose ? "set_prolog_flag(argv, [extensions])"
                             : "set_prolog_flag(argv, [silent, extensions])") )
  { mt_free(path); mt_free(buf);
    return NULL;
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

     It also makes the boot CHEAPER, which is not why it is here but was most
     of what it did to the counters while this seat still read the umbrella
     from source: qlf_boot sets encoding(utf8), and a source read goes through
     the locale's multibyte conversion without it. Boot measured 1,961,762,311
     retired instructions with neither, 1,735,405,359 with the encoding flag
     alone and 1,761,830,644 with qlf_boot, so the flag was worth -11.5% and
     the purge machinery cost about 26M of it back, plus 6,955 inferences for
     globbing the artifact set and reading its stamp [measured 2026-08-29,
     min-of-three per arm, one arm per mechanism, all three in the source
     regime the load below has since left]. The purge is what makes this
     correct and the encoding comes with it; neither is worth having alone.

     Concurrent opens are safe, which is the question deleting files at boot
     invites, and it is the sharper question now that this seat GENERATES the
     artifacts it purges. Six hello processes started at once against a STALE
     artifact set, so all six purge and regenerate together, all answered
     correctly; qlf_boot publishes its stamp through a temporary file and
     rename/2 for that reason, and SWI publishes each .qlf the same way,
     through `.<name>.qlf.<pid>` and rename(2)
     [measured 2026-08-29: 6/6, examples/hello, set made stale by touching
     engine/spaces/foreign.pl; commit=888a73c7d231188cd90fafcb8b0cce3799ef5e97;
     re-run 2026-09-05 against an EMPTY artifact set, so all six generate:
     6/6]. */
  snprintf(buf, bufsz, "%s/engine/qlf_boot.pl", path);
  if ( !goal_atom("consult", buf) )
  { mt_free(path); mt_free(buf);
    return NULL;
  }
  mt_free(buf);

  /* Then the engine, through the engine's OWN load rather than a consult
     spelled here. metta_qlf_boot:qlf_load_engine is what engine/main.pl runs,
     so the compiled regime and its recovery have one implementation in the
     tree; the goal carries no file name because qlf_boot.pl asserted the
     engine directory from its own load context when it was consulted above.

     This seat used to name "%s/engine/metta.pl", and an explicit .pl is the
     SOURCE: SWI read and compiled the umbrella and its eleven engine/metta/
     units on every boot. Naming it without the extension takes
     engine/metta.qlf instead and the boot case falls from 1,563,321
     inferences to 633,848, three identical samples each way, and from
     1,885,311,169 retired instructions to 1,107,958,359, so 929,473
     inferences, 59.5% of the row, and 41.2% of the whole process, were the
     compiler doing the same work again. The other five cases are identical
     to the inference
     [measured 2026-09-05; command=CHECK_PY=$CHECK_PY sh
     extensions/cmetta/bench.sh; fixture=built C host with the mork, node and
     python seats loaded;
     commit=48b6cb4eea09e6f2f9637c7186e77c628d61b7e3].

     It also stops this seat being the one host that cannot warm its own tree.
     On a tree with no artifacts the old spelling read 3,417,125 inferences,
     wrote nothing, and the NEXT boot read 3,417,141: it never warmed, so an
     installation that only ever ran a C program paid the whole source compile
     on every run. Under qcompile(auto) the first boot reads 3,459,587 and
     leaves the fourteen artifacts, and the second reads 633,837
     [measured 2026-09-05, both binaries against the same artifact-free copy of
     this checkout, in the harness's own built environment].
     A tree the process may not write is unchanged: SWI falls back to source,
     writes nothing and says nothing
     [tested: test_a_read_only_engine_tree_boots_from_source;
     commit=48b6cb4eea09e6f2f9637c7186e77c628d61b7e3]. */
  if ( !goal("metta_qlf_boot:qlf_load_engine") )
  { mt_free(path);
    return NULL;
  }

  /* predicate_t values point into SWI's procedure table and PL_cleanup()
     invalidates them. Resolve the cache after the bridge has loaded, then
     clear it only after cleanup succeeds
     [tested: test_restart_replaces_runtime_owned_predicates;
     commit=802878f86f478c23fc05f7e68cbe605160eedb59]. */
  g_runtime.space_operand =
    PL_predicate("metta_c_space_operand", 1, "user");
  g_runtime.equal_functor = equal_functor;
  g_runtime.pair_functor = pair_functor;
  g_runtime.initial_stack_bytes = initial_stack_bytes;
  g_runtime.generation = ++g_runtime_generation;
  g_runtime.open = true;
  g_runtime.path = path;
  g_runtime.verbose = config->verbose;
  g_open = true;

  /* The seam's shipped points, declared once the runtime is open, so every
     door this seat already had is a row from the first call and "what can I
     extend here" is a query rather than a source reading. */
  if ( !seam_declare_shipped(&g_runtime) )
  { mt_close(&g_runtime);
    return NULL;
  }

  mt_verbose(&g_runtime, config->verbose);
  return &g_runtime;
}

/* Defined with the show ring below; declared here because mt_close comes
   first in the file and both lifecycle exits release it. */
static void show_ring_release(void);

void mt_close(metta *runtime)
{ size_t i;
  int cleaned;

  if ( !runtime || !g_open ) return;
  if ( g_transaction )
  { err_set(MT_MISUSE, "mt_close cannot close the runtime inside a transaction callback");
    return;
  }

  /* Halt hooks may cancel cleanup. Until SWI confirms completion, every C
     handle and the g_open state still describe the live engine and must stay
     intact. A failed reclamation has passed the point of cancellation but
     cannot be restarted safely, so it is closed and remembered as terminal. */
  cleaned = PL_cleanup(0);
  if ( cleaned == PL_CLEANUP_CANCELED )
  { err_set(MT_ERROR,
            "SWI-Prolog canceled runtime cleanup; the runtime remains open");
    return;
  }
  if ( cleaned == PL_CLEANUP_RECURSIVE )
  { err_set(MT_ERROR,
            "mt_close was called recursively from SWI-Prolog cleanup; the "
            "outer cleanup still owns the runtime");
    return;
  }

  g_cleanup_failed = cleaned != PL_CLEANUP_SUCCESS;
  for (i = 0; i < runtime->nops; i++) mt_free(runtime->ops[i].name);
  mt_free(runtime->ops);
  seam_release(runtime);
  mt_free(runtime->path);
  memset(runtime, 0, sizeof(*runtime));
  g_open = false;
  show_ring_release();
  if ( g_cleanup_failed )
    err_set(MT_ERROR,
            "SWI-Prolog cleanup failed to reclaim the runtime; this process "
            "cannot safely initialise it again");
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
  if ( av && put_name(av, verbose ? "false" : "true") &&
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
{ if ( g_transaction )
  { err_set(MT_MISUSE, "mt_thread_detach cannot detach inside a transaction callback");
    return;
  }
  show_ring_release();
  if ( g_open ) PL_thread_destroy_engine();
}

/* ================================================================== *
 * Text
 * ================================================================== */

/* No runtime argument on the text doors either: they need the ENGINE, and
   there is one of those per process. Threading a handle through them was
   ceremony that never chose anything. */
static mt_atom *parse_n(const char *source, size_t length, const char *door,
                        const char *predicate)
{ fid_t f;
  term_t av;
  mt_atom *out = NULL;

  if ( !engine_ready(door) ) return NULL;

  if ( !(f = frame_open(door)) ) return NULL;
  av = PL_new_term_refs(3);
  if ( !av || !put_chars(av, PL_STRING | REP_UTF8, length, source) )
  { PL_discard_foreign_frame(f);
    if ( mt_ok() ) err_set(MT_NOMEM, "out of memory holding the source");
    return NULL;
  }
  if ( call_bridge(predicate, 3, av) == MT_OK )
    out = decode(av + 1, av + 2);
  PL_discard_foreign_frame(f);
  return out;
}

mt_atom *mt_parse(const char *source)
{ if ( !source ) return err_null(MT_MISUSE, "mt_parse needs source text");
  return parse_n(source, strlen(source), "mt_parse", "metta_c_read");
}

mt_atom *mt_parsen(const char *source, size_t length)
{ if ( !source ) return err_null(MT_MISUSE, "mt_parsen needs source text");
  return parse_n(source, length, "mt_parsen", "metta_c_read");
}

mt_list mt_forms(const char *source)
{ mt_atom *forms;
  mt_list result = {0};
  if ( !source ) { err_set(MT_MISUSE, "mt_forms needs source text"); return result; }
  forms = parse_n(source, strlen(source), "mt_forms", "metta_c_read_forms");
  if ( !forms ) return result;
  /* The fresh decoded expression has one owner; transfer its child vector. */
  result = (mt_list){forms->u.e.kids, forms->u.e.n};
  forms->u.e.kids = NULL; forms->u.e.n = 0;
  mt_drop(forms);
  return result;
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

  mt_free(g_show[slot]);
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
  { mt_free(g_show[i]);
    g_show[i] = NULL;
  }
  g_show_at = 0;
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

  if ( !(s = mt_calloc(1, sizeof(*s))) )
    return err_null(MT_NOMEM, "out of memory opening a space");
  if ( !(s->name = mt_strdup(name)) )
  { mt_free(s);
    return err_null(MT_NOMEM, "out of memory naming a space");
  }
  s->runtime = runtime;
  return s;
}

void mt_space_close(mt_space *space)
{ if ( !space || space->borrowed ) return;
  mt_free(space->name);
  mt_free(space);
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
   [measured 2026-08-31; C32 in ai-cmetta-c-constraints.md].

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
  { if ( !put_name(av, space->name) ||
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

static bool atom_list_given(mt_list atoms, const char *door)
{ size_t i;

  if ( atoms.len && !atoms.items )
  { err_set(MT_MISUSE, "%s was given a length but no atom array", door);
    return false;
  }
  for (i = 0; i < atoms.len; i++)
    if ( !atoms.items[i] )
    { err_set(MT_MISUSE, "%s was given NULL at index %zu", door, i);
      return false;
    }
  return true;
}

/* Build the complete Prolog list before calling metta_add_atoms/2. Each atom
   gets its own variable-name context, as it does through repeated mt_add(),
   while the bridge receives one batch and therefore one engine transaction. */
bool mt_space_add_all(mt_space *space, mt_list atoms)
{ mt_status status = MT_MISUSE;
  fid_t f = 0;
  term_t av = 0, item = 0;
  size_t i;

  if ( handle_ready(space, "mt_space_add_all") &&
       atom_list_given(atoms, "mt_space_add_all") )
  { f = frame_open("mt_space_add_all");
    av = f ? PL_new_term_refs(2) : 0;
    item = av ? PL_new_term_ref() : 0;
    if ( !av || !item || !put_name(av, space->name) ||
         !PL_put_nil(av + 1) )
      status = mt_ok() ? err_set(MT_NOMEM, "out of memory encoding an atom batch")
                       : mt_error();
    else
    { status = MT_OK;
      for (i = atoms.len; i > 0; i--)
        if ( !put_atom(atoms.items[i - 1], item) ||
             !PL_cons_list(av + 1, item, av + 1) )
        { status = mt_ok()
                 ? err_set(MT_NOMEM, "the engine could not hold an atom batch")
                 : mt_error();
          break;
        }
      if ( status == MT_OK ) status = call_bridge("metta_c_add_all", 2, av);
    }
  }
  frame_close(f);
  mt_list_free(atoms);
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
    mt_free(text);
  }
  frame_close(f);
  mt_drop(atom);
  return removed;
}

static bool space_count_value(term_t term, size_t *count)
{ uint64_t wide;

  if ( !PL_get_uint64(term, &wide) )
  { err_set(MT_ERROR,
            "the space answered a negative or non-integer count");
    return false;
  }
  if ( sizeof(size_t) < sizeof(wide) && wide > SIZE_MAX )
  { err_set(MT_ERROR, "the space count exceeds this platform's size_t range");
    return false;
  }
  *count = (size_t)wide;
  return true;
}

size_t mt_space_count(mt_space *space)
{ fid_t f = 0;
  term_t av = 0;
  mt_status status = MT_MISUSE;
  size_t n = 0;

  if ( handle_ready(space, "mt_space_count") )
    status = space_call("metta_c_count", space, NULL, 2, &av, &f);

  if ( status == MT_OK && !space_count_value(av + 1, &n) )
    status = MT_ERROR;
  frame_close(f);
  return status == MT_OK ? n : 0;
}

bool mt_space_wipe(mt_space *space)
{ return handle_ready(space, "mt_space_wipe") &&
         space_call("metta_c_clear", space, NULL, 1, NULL, NULL) == MT_OK;
}

/* The &self halves of the same verbs, which is what a `metta *` receiver
   reaches. Written out rather than generated so each one is greppable. */
bool mt_self_add(metta *runtime, mt_atom *atom)
{ return mt_space_add(mt_self(runtime), atom); }
bool mt_self_add_all(metta *runtime, mt_list atoms)
{ mt_space *self = mt_self(runtime);
  return self ? mt_space_add_all(self, atoms)
              : (mt_list_free(atoms), false);
}
bool mt_self_del(metta *runtime, mt_atom *atom)
{ return mt_space_del(mt_self(runtime), atom); }
size_t mt_self_count(metta *runtime)
{ return mt_space_count(mt_self(runtime)); }
bool mt_self_wipe(metta *runtime)
{ return mt_space_wipe(mt_self(runtime)); }

/* ================================================================== *
 * Answers
 * ================================================================== */

typedef struct eager_answer
{ mt_atom *atom;
  char    *text;
  size_t   group;
} eager_answer;

/* One cursor protocol covers materialized results, engine goals and C producers. */
struct mt_answers
{ metta        *runtime;
  uint64_t       generation;     /* runtime that owns a lazy cursor id */
  enum { ANSWERS_EAGER, ANSWERS_ENGINE, ANSWERS_NATIVE } kind;
  mt_iterator   iterator;
  mt_status     status;
  mt_atom      *pattern;        /* what mt_bound lines each answer against */
  int64_t       cursor_id;      /* lazy: the bridge's engine id     */
  atom_t        cursor_ref;     /* lazy: registered record reference */
  eager_answer *items;          /* eager: every answer, in order    */
  size_t        n, at;
  bool          started, done;
  mt_atom      *current;
  char         *current_text;
  size_t        current_group;
  mt_row        row;            /* refreshed each step; mt_next points at it */
};

static mt_answers *answers_alloc(metta *runtime)
{ mt_answers *a = mt_calloc(1, sizeof(*a));
  if ( !a ) err_set(MT_NOMEM, "out of memory opening a cursor");
  else
  { a->runtime = runtime;
    a->generation = runtime ? runtime->generation : 0;
  }
  return a;
}

mt_answers *mt_answers_from(mt_iterator iterator)
{ mt_answers *answers;
  if ( !iterator.next )
  { iterator_close(&iterator);
    return err_null(MT_MISUSE, "mt_answers_from needs a next callback");
  }
  answers = answers_alloc(NULL);
  if ( !answers ) { iterator_close(&iterator); return NULL; }
  answers->kind = ANSWERS_NATIVE;
  answers->iterator = iterator;
  return answers;
}

static void stream_release(void *cursor)
{ mt_answers_free(cursor);
}

mt_atom *mt_stream(mt_iterator iterator)
{ mt_answers *cursor = mt_answers_from(iterator);
  mt_box_t *box;
  if ( !cursor ) return NULL;
  box = box_new(cursor, "Iterator", stream_release, NULL, NULL);
  if ( !box ) { mt_answers_free(cursor); return NULL; }
  box->stream = cursor;
  return object_from_box(box);
}

mt_answers *mt_stream_of(const mt_atom *atom)
{ if ( !atom || atom->kind != MT_OBJECT || !atom->u.box->stream )
    return err_null(MT_MISUSE, "mt_stream_of needs an iterator value");
  return atom->u.box->stream;
}

static foreign_t pl_cmetta_stream(term_t stream, term_t result, control_t control)
{ mt_box_t *box;
  const mt_atom *answer;
  mt_status status;
  if ( PL_foreign_control(control) == PL_PRUNED ) return TRUE;
  box = blob_box(stream);
  if ( !box || !box->stream ) return PL_type_error("cmetta_iterator", stream);
  for (;;)
  { fid_t frame;
    term_t out;
    status = mt_step(box->stream, &answer);
    if ( status == MT_DONE ) return FALSE;
    if ( status != MT_ROW ) return callback_error("c-iter", mt_errmsg() ? mt_errmsg() : "iterator failed");
    frame = PL_open_foreign_frame();
    out = frame ? PL_new_term_ref() : 0;
    if ( !out || !put_atom(answer, out) )
    { frame_close(frame); return PL_resource_error("memory"); }
    if ( PL_unify(result, out) )
    { PL_close_foreign_frame(frame); PL_retry(0); }
    PL_discard_foreign_frame(frame);
    if ( PL_exception(0) ) return FALSE;
  }
}

mt_status mt_answers_status(const mt_answers *answers)
{ return answers ? answers->status : MT_MISUSE;
}

#ifdef MT_TEST_FAULTS
/* One-shot fault injection for the separate allocator regression binary. It
   is absent from the installed library and counts successful vector growths
   before refusing one [tested: test_eager_growth_is_transactional;
   commit=31075a6fadc4bf7508bd089f7abb2a542ec9a787]. */
static MT_TLS size_t test_eager_grows_before_failure = SIZE_MAX;

void mt_test_fail_eager_grow_after(size_t successful_grows)
{ test_eager_grows_before_failure = successful_grows;
}

/* Expose the identifier but not the engine-owned cursor itself. The regression
   uses it to prove that an emptied table cannot recycle a stale identifier
   [tested: test_cursor_ids_are_monotone_and_constant_cost;
   commit=b5ddebe73273447caa7c57212d6ee86fc71e0d4a]. */
int64_t mt_test_cursor_id(const mt_answers *answers)
{ return answers && answers->kind == ANSWERS_ENGINE ? answers->cursor_id : -1;
}
#endif

static void *eager_grow(void *items, size_t bytes)
{
#ifdef MT_TEST_FAULTS
  if ( test_eager_grows_before_failure != SIZE_MAX )
  { if ( test_eager_grows_before_failure == 0 )
    { test_eager_grows_before_failure = SIZE_MAX;
      return NULL;
    }
    test_eager_grows_before_failure--;
  }
#endif
  return mt_resize(items, bytes);
}

/* Read the engine's Groups term: a list of groups, each a list of answers. */
static mt_status collect_groups(term_t groups, mt_answers *out)
{ term_t group = PL_new_term_ref();
  term_t gtail = PL_copy_term_ref(groups);
  term_t answer = PL_new_term_ref();
  size_t cap = 8, index = 0;

  if ( !(out->items = mt_alloc(cap * sizeof(*out->items))) )
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
      { mt_free(text);
        return MT_UNSUPPORTED;
      }
      if ( out->n == cap )
      { eager_answer *grown;
        size_t next;

        if ( cap > SIZE_MAX / 2 ||
             (next = cap * 2) > SIZE_MAX / sizeof(*out->items) ||
             !(grown = eager_grow(out->items,
                                  next * sizeof(*out->items))) )
        { mt_drop(atom);
          mt_free(text);
          return err_set(MT_NOMEM, "out of memory collecting answers");
        }
        out->items = grown;
        cap = next;
      }
      out->items[out->n].atom = atom;
      out->items[out->n].text = text;
      out->items[out->n].group = index;
      out->n++;
    }
    index++;
  }
  return MT_OK;
}

static mt_status run_or_load(metta *runtime, const char *pred, int representation,
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
  if ( !av || !put_chars(av, PL_STRING | representation, (size_t)-1, argument) ||
       !put_name(av + 1, space) ||
       !PL_put_float(av + 2, runtime->limits.seconds) ||
       !PL_put_int64(av + 3, (int64_t)runtime->limits.inferences) )
  { PL_discard_foreign_frame(f);
    mt_answers_free(answers);
    return mt_ok() ? err_set(MT_NOMEM, "out of memory holding the argument")
                   : mt_error();
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

mt_answers *mt_self_run(metta *runtime, const char *source)
{ mt_answers *out = NULL;
  if ( handle_ready(runtime, "mt_run") )
    run_or_load(runtime, "metta_c_run", REP_UTF8, source, "&self", &out);
  return out;
}

bool mt_self_do(metta *runtime, const char *source)
{ mt_answers *answers;
  if ( !handle_ready(runtime, "mt_do") ) return false;
  if ( !(answers = mt_run(runtime, source)) ) return false;
  mt_answers_free(answers);
  return true;
}

mt_answers *mt_self_load(metta *runtime, const char *path)
{ mt_answers *out = NULL;
  if ( handle_ready(runtime, "mt_load") )
    run_or_load(runtime, "metta_c_load", REP_FN, path, "&self", &out);
  return out;
}

mt_answers *mt_space_run(mt_space *space, const char *source)
{ mt_answers *out = NULL;
  if ( handle_ready(space, "mt_space_run") )
    run_or_load(space->runtime, "metta_c_run", REP_UTF8, source, space->name, &out);
  return out;
}

mt_answers *mt_space_load(mt_space *space, const char *path)
{ mt_answers *out = NULL;
  if ( handle_ready(space, "mt_space_load") )
    run_or_load(space->runtime, "metta_c_load", REP_FN, path, space->name, &out);
  return out;
}

bool mt_space_do(mt_space *space, const char *source)
{ mt_answers *answers = mt_space_run(space, source);
  if ( !answers ) return false;
  mt_answers_free(answers);
  return true;
}

bool mt_space_drop(mt_space *space)
{ if ( !handle_ready(space, "mt_space_drop") ) return false;
  return space_call("metta_c_drop_space", space, NULL, 1, NULL, NULL) == MT_OK;
}

static mt_status open_cursor(mt_space *space, const char *pred,
                                  const mt_atom *atom,
                                  mt_answers **out)
{ fid_t f;
  term_t av;
  mt_answers *answers;
  mt_status status;
  int64_t id;
  atom_t ref;

  *out = NULL;   /* see run_or_load: zeroed before anything can fail. */
  if ( !(answers = answers_alloc(space->runtime)) ) return MT_NOMEM;

  if ( !(f = frame_open(pred)) )
  { mt_answers_free(answers);
    return MT_NOMEM;
  }
  av = PL_new_term_refs(4);
  if ( !av || !put_atom(atom, av) || !put_name(av + 1, space->name) ||
       !PL_put_int64(av + 2, (int64_t)space->runtime->limits.inferences) )
  { PL_discard_foreign_frame(f);
    mt_answers_free(answers);
    return mt_ok() ? err_set(MT_MISUSE, "%s could not write its goal", pred)
                   : mt_error();   /* put_atom already said why */
  }
  status = call_bridge(pred, 4, av);
  if ( status == MT_OK )
  { term_t parts = PL_new_term_refs(2);
    if ( parts && PL_get_arg(1, av + 3, parts) &&
         PL_get_int64(parts, &id) && PL_get_arg(2, av + 3, parts + 1) &&
         PL_get_atom(parts + 1, &ref) )
    { /* The record owns either a suspended engine or transaction-held rows.
         A direct reference lookup is independent of the number of cursors.
         [tested: tests/test_cursor_ids.c; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c] */
      PL_register_atom(ref);
      answers->kind = ANSWERS_ENGINE;
      answers->cursor_id = id;
      answers->cursor_ref = ref;
    } else
    { if ( parts && PL_get_arg(2, av + 3, parts) )
        call_bridge("metta_c_close", 1, parts);
      status = err_set(MT_ERROR, "the bridge did not answer a cursor reference");
    }
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

/* The query is one engine expression, so pattern and guard share variable cells.
   Time and space: O(1) new nodes; the existing immutable pattern is retained.
   [tested: test_prepared_queries_join_and_guard_current_facts; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c] */
mt_answers *mt_space_query(mt_space *space, mt_atom *pattern, mt_atom *guard)
{ mt_answers *out = NULL;
  if ( handle_ready(space, "mt_space_query") && atom_given(pattern, "mt_space_query") )
  { out = mt_space_eval(space, mt_expr("match", mt_spaceref(space->name),
                     mt_keep(pattern), mt_expr("if", guard ? guard : mt_bool(true),
                                               mt_keep(pattern), "Empty")));
    guard = NULL;
  }
  mt_drop(guard);
  if ( out ) out->pattern = pattern;
  else mt_drop(pattern);
  return out;
}

mt_answers *mt_space_eval_under(mt_space *space, mt_atom *algebra, mt_atom *goal)
{ mt_answers *out = NULL;
  mt_atom *request = mt_expr(algebra, goal);
  if ( handle_ready(space, "mt_space_eval_under") && atom_given(request, "mt_space_eval_under") )
    open_cursor(space, "metta_c_open_under", request, &out);
  mt_drop(request);
  return out;
}

mt_answers *mt_self_query(metta *runtime, mt_atom *pattern, mt_atom *guard)
{ return mt_space_query(mt_self(runtime), pattern, guard); }
mt_answers *mt_self_eval_under(metta *runtime, mt_atom *algebra, mt_atom *goal)
{ return mt_space_eval_under(mt_self(runtime), algebra, goal); }

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
{ if ( answers->kind != ANSWERS_EAGER )
  { mt_drop(answers->current);
    mt_free(answers->current_text);
  }
  answers->current = NULL;
  answers->current_text = NULL;
}

static mt_status answers_pull(mt_answers *answers, const char *door)
{ fid_t f;
  term_t av;
  mt_status status;
  term_t head, tail;

  if ( !answers ) return err_set(MT_MISUSE, "%s needs a cursor", door);
  if ( answers->done ) return answers->status;

  if ( answers->kind == ANSWERS_NATIVE )
  { clear_current(answers);
    status = iterator_next(&answers->iterator, &answers->current);
    if ( status != MT_ROW )
    { answers->done = true;
      iterator_close(&answers->iterator);
    }
    return status;
  }
  if ( answers->kind == ANSWERS_EAGER )
  { if ( answers->at >= answers->n )
    { answers->done = true;
      answers->current = NULL;
      answers->current_text = NULL;
      return MT_DONE;
    }
    answers->current = answers->items[answers->at].atom;
    answers->current_text = answers->items[answers->at].text;
    answers->current_group = answers->items[answers->at].group;
    answers->at++;
    return MT_ROW;
  }

  /* Only the lazy half needs the engine; a run's answers are C memory and a
     caller may walk them after mt_close(). */
  if ( !engine_ready(door) ) return MT_MISUSE;
  if ( answers->generation != g_runtime.generation )
  { answers->done = true;
    return err_set(MT_MISUSE,
                   "%s was given a cursor from a previous engine runtime",
                   door);
  }

  clear_current(answers);

  if ( !(f = frame_open(door)) ) return MT_NOMEM;
  av = PL_new_term_refs(4);
  if ( !av || !PL_put_int64(av, answers->cursor_id) ||
       !PL_put_atom(av + 1, answers->cursor_ref) ||
       !PL_put_float(av + 2, answers->runtime->limits.seconds) )
  { PL_discard_foreign_frame(f);
    return err_set(MT_NOMEM, "out of memory stepping a cursor");
  }
  status = call_bridge("metta_c_next", 4, av);
  if ( status != MT_OK )
  { PL_discard_foreign_frame(f);
    answers->done = true;
    return status;
  }

  head = PL_new_term_ref();
  tail = PL_copy_term_ref(av + 3);
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

static mt_status answers_step(mt_answers *answers, const char *door)
{ mt_status status = answers_pull(answers, door);
  if ( answers ) answers->status = status;
  return status;
}

mt_status mt_step(mt_answers *answers, const mt_atom **answer)
{ mt_status status;
  if ( !answer ) return err_set(MT_MISUSE, "mt_step needs an answer pointer");
  *answer = NULL;
  status = answers_step(answers, "mt_step");
  if ( status == MT_ROW ) *answer = answers->current;
  return status;
}

/* One call per answer instead of step-then-read, so the loop condition and
   the value are the same expression. NULL ends the walk; mt_ok() says
   whether that was exhaustion or a failure. */
const mt_atom *mt_next(mt_answers *answers)
{ if ( !answers ) return NULL;
  return answers_step(answers, "mt_next") == MT_ROW ? answers->current : NULL;
}

/* The same step reported in full. The row lives in the cursor and is
   refreshed here, so it is a pointer and costs no copy per answer. */
const mt_row *mt_row_next(mt_answers *answers)
{ if ( !answers || answers_step(answers, "mt_row_next") != MT_ROW ) return NULL;
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
  if ( answers->status != MT_DONE && answers->status != MT_ROW )
  { mt_drop(owned);
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
    { size_t grown_cap, bytes;
      mt_atom **grown;
      if ( !next_capacity(cap, 8, sizeof(*grown), &grown_cap, &bytes) )
        grown = NULL;
      else
        grown = mt_resize(out.items, bytes);
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
  if ( answers->status != MT_DONE )
  { mt_list_free(out);
    out = (mt_list){0};
  }
  mt_answers_free(answers);
  return out;
}

void mt_list_free(mt_list list)
{ size_t i;
  if ( !list.items ) return;
  for (i = 0; i < list.len; i++) mt_drop(list.items[i]);
  mt_free(list.items);
}

/* Walk the pattern and the answer together; where the pattern has the named
   variable, the answer's subterm at that position is what it reached. Pure C
   over two C terms, so it costs one walk and no engine call. The first
   occurrence wins, which is the same rule a repeated variable already has:
   two occurrences of one name are one variable, so they agree.

   This is one variable's half of the directional match the Python seat spells
   `unify(pattern, atom)`, whose documented reconstruction is
   `substitute(pattern, unify(pattern, atom))`
   [source: extensions/python/metta/atoms.py, _match/unify;
   commit=e927fffde3a19d9927892bf64a7fc6202b866ae0].
   Narrower on purpose: a caller asking for one answer-row name does not need
   the whole mt_unify() binding set built to be handed one term out of it. */
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

  if ( answers->kind == ANSWERS_NATIVE )
  { iterator_close(&answers->iterator);
    clear_current(answers);
  } else if ( answers->kind == ANSWERS_ENGINE )
  { /* SWI resets flag/3 state at PL_cleanup(), so the first cursor after a
       restart may reuse the old runtime's numeric id. The generation is the
       other half of the handle: an old C cursor can release its own memory,
       but cannot close a new runtime's engine
       [tested: test_cursor_ids_are_monotone_and_constant_cost;
       commit=b5ddebe73273447caa7c57212d6ee86fc71e0d4a]. */
    if ( g_open && answers->generation == g_runtime.generation )
    { fid_t f = frame_open("mt_answers_free");
      term_t av = f ? PL_new_term_refs(1) : 0;
      if ( av && PL_put_atom(av, answers->cursor_ref) )
        call_bridge("metta_c_close", 1, av);
      frame_close(f);
      /* A close error is reported through call_bridge, but must not strand
         C's reference. The bridge has already erased its recorded owner and
         run engine destruction in cleanup. A previous close is harmless. */
      PL_unregister_atom(answers->cursor_ref);
    }
    clear_current(answers);
  } else
  { for (i = 0; i < answers->n; i++)
    { mt_drop(answers->items[i].atom);
      mt_free(answers->items[i].text);
    }
    mt_free(answers->items);
  }
  mt_free(answers);
}

/* ================================================================== *
 * Bounding an evaluation
 * ================================================================== */

bool mt_limit(metta *runtime, mt_limits limits)
{ if ( !handle_ready(runtime, "mt_limit") ) return false;
  if ( !set_prolog_size_flag("stack_limit",
                             limits.stack_bytes ? limits.stack_bytes
                                                : runtime->initial_stack_bytes) )
    return false;
  runtime->limits = limits;
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

static bool number_as_double(term_t term, double *value)
{ int64_t signed_value;
  uint64_t unsigned_value;

  if ( PL_get_float(term, value) ) return true;
  if ( PL_get_int64(term, &signed_value) )
  { *value = (double)signed_value;
    return true;
  }
  if ( PL_get_uint64(term, &unsigned_value) )
  { *value = (double)unsigned_value;
    return true;
  }
  return false;
}

static bool decode_stats(term_t list, mt_stats *out)
{ term_t head = PL_new_term_ref();
  term_t tail = PL_copy_term_ref(list);
  uint64_t integers[4];
  double times[2];
  size_t i;

  if ( !head || !tail )
  { err_set(MT_NOMEM, "out of memory decoding the engine counters");
    return false;
  }
  for (i = 0; i < 6; i++)
  { if ( !PL_get_list(tail, head, tail) )
    { err_set(MT_ERROR,
              "the engine answered %zu counters where six were expected", i);
      return false;
    }
    if ( i == 1 || i == 4 )
    { size_t time_index = i == 1 ? 0 : 1;
      if ( !number_as_double(head, &times[time_index]) )
      { err_set(MT_ERROR, "engine counter %zu was not numeric", i + 1);
        return false;
      }
    } else
    { size_t integer_index = i == 0 ? 0 : i == 2 ? 1 : i == 3 ? 2 : 3;
      if ( !PL_get_uint64(head, &integers[integer_index]) )
      { err_set(MT_ERROR,
                "engine counter %zu was not an unsigned integer", i + 1);
        return false;
      }
    }
  }
  if ( !PL_get_nil(tail) )
  { err_set(MT_ERROR, "the engine answered more than six counters");
    return false;
  }
  out->inferences = integers[0];
  out->cputime = times[0];
  out->gc_count = integers[1];
  out->gc_freed = integers[2];
  out->gc_time = times[1] / 1000.0;
  out->table_bytes = integers[3];
  return true;
}

mt_stats mt_stats_now(metta *runtime)
{ fid_t f;
  term_t av;
  mt_status status;
  mt_stats out = {0, 0, 0, 0, 0, 0};

  if ( !handle_ready(runtime, "mt_stats_now") ) return out;
  if ( !(f = frame_open("mt_stats_now")) ) return out;

  av = PL_new_term_refs(1);
  status = av ? call_bridge("metta_c_stats", 1, av)
              : err_set(MT_NOMEM, "out of memory sampling the counters");
  if ( status == MT_OK && !decode_stats(av, &out) ) status = MT_ERROR;
  PL_discard_foreign_frame(f);
  if ( status != MT_OK ) return out;
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

#ifdef MT_TEST_FAULTS
/* Fault-library probes exercise engine terms that the public bridge cannot
   honestly produce. They keep invalid fixtures out of the installed surface
   while testing the conversion helpers themselves. */
static int test_handle_write(IOSTREAM *stream, atom_t atom, int flags)
{ (void)atom;
  (void)flags;
  Sfprintf(stream, "<cmetta-test-handle>");
  return TRUE;
}

static PL_blob_t test_handle_blob =
{ .magic = PL_BLOB_MAGIC,
  .flags = PL_BLOB_UNIQUE,
  .name  = "cmetta_test_handle",
  .write = test_handle_write
};

/* Construct somebody else's real SWI blob inside the fault library, decode it
   through the MT_HANDLE branch, then prove its printed name cannot be encoded
   as though it were the engine value. No public constructor is invented for a
   native value C cannot itself own.
   [tested: tests/test_internal_contracts.c,
   test_native_handle_decode_and_encode_contract;
   commit=1156a16d24228f183466264ba51f9086ce435266] */
bool mt_test_native_handle_codec_is_guarded(void)
{ static const unsigned payload = UINT32_C(0xc0decafe);
  fid_t frame = frame_open("testing a native engine handle");
  term_t encoded, destination;
  mt_atom *decoded = NULL;
  bool guarded = false;

  if ( !frame ) return false;
  encoded = PL_new_term_ref();
  destination = PL_new_term_ref();
  if ( !encoded || !destination ||
       !PL_put_blob(encoded, (void *)&payload, sizeof(payload),
                    &test_handle_blob) )
    goto done;
  decoded = decode(encoded, 0);
  if ( !decoded || decoded->kind != MT_HANDLE ||
       strcmp(decoded->u.t.text, "<cmetta-test-handle>") != 0 )
    goto done;
  mt_clear();
  guarded = !put_atom(decoded, destination) &&
            mt_error() == MT_UNSUPPORTED && mt_errmsg() &&
            strstr(mt_errmsg(), "cannot be sent back by its printed form");
done:
  mt_drop(decoded);
  frame_close(frame);
  return guarded;
}

bool mt_test_improper_apply_is_rejected(void)
{ fid_t f = frame_open("testing an improper apply list");
  term_t callable, args, head, tail, result;
  char *text = NULL;
  bool rejected = false;

  if ( !f ) return false;
  callable = PL_new_term_ref();
  args = PL_new_term_ref();
  head = PL_new_term_ref();
  tail = PL_new_term_ref();
  result = PL_new_term_ref();
  if ( callable && args && head && tail && result &&
       put_name(head, "head") && put_name(tail, "not_a_list") &&
       PL_cons_list(args, head, tail) &&
       pl_cmetta_apply(callable, args, result, 0) == FALSE )
  { term_t exception = PL_exception(0);
    if ( exception )
    { text = term_text(exception, CVT_WRITE, NULL);
      rejected = text && strstr(text, "type_error(list") != NULL;
      PL_clear_exception();
    }
  }
  mt_free(text);
  frame_close(f);
  return rejected;
}

bool mt_test_negative_count_is_rejected(void)
{ fid_t f = frame_open("testing a negative space count");
  term_t value;
  size_t count = 0;
  bool rejected;

  if ( !f ) return false;
  value = PL_new_term_ref();
  rejected = value && PL_put_int64(value, -1) &&
             !space_count_value(value, &count);
  frame_close(f);
  return rejected;
}

bool mt_test_large_stats_are_exact(void)
{ static const uint64_t first = UINT64_C(9007199254740993);
  static const uint64_t last = UINT64_C(9007199254740995);
  fid_t f = frame_open("testing exact large counters");
  term_t list, head, tail;
  mt_stats stats = {0};
  bool ok = false;
  int i;

  if ( !f ) return false;
  list = PL_new_term_ref();
  head = PL_new_term_ref();
  tail = PL_new_term_ref();
  if ( !list || !head || !tail || !PL_put_nil(list) ) goto done;
  for (i = 5; i >= 0; i--)
  { bool put = i == 0 ? PL_put_uint64(head, first)
             : i == 1 ? PL_put_float(head, 1.5)
             : i == 2 ? PL_put_uint64(head, 7)
             : i == 3 ? PL_put_uint64(head, 9)
             : i == 4 ? PL_put_float(head, 2.0)
                      : PL_put_uint64(head, last);
    if ( !put || !PL_put_term(tail, list) ||
         !PL_cons_list(list, head, tail) )
      goto done;
  }
  ok = decode_stats(list, &stats) && stats.inferences == first &&
       stats.table_bytes == last && stats.gc_count == 7 &&
       stats.gc_freed == 9 && stats.cputime == 1.5 &&
       stats.gc_time == 0.002;
done:
  frame_close(f);
  return ok;
}

bool mt_test_decode_growth_overflow_is_rejected(void)
{ decode_frame frame = { .kids = NULL, .n = SIZE_MAX, .cap = SIZE_MAX,
                         .tail = 0 };
  return !decode_frame_add(&frame, NULL);
}

size_t mt_test_stack_limit(void)
{ size_t value = 0;
  (void)prolog_size_flag("stack_limit", &value);
  return value;
}
#endif


/* ================================================================== *
 * Extending this seat
 * ================================================================== */

/* The seam's own storage. Points and rows are flat arrays because both are
   small and read far more often than written; a walk of ten rows is what a
   dispatch costs, and a hash table would be more machinery than the question
   deserves. Every string here is COPIED, so a caller may free or reuse its
   own buffer as soon as the call returns. */

static void point_release(mt_point *point)
{ mt_free((char *)point->name);
  mt_free((char *)point->fields);
  mt_free((char *)point->doc);
}

static void row_release(mt_row_entry *entry)
{ mt_seam_row *row = &entry->row;
  if ( MT_DEC(&entry->refs) != 1 ) return;
  if ( row->release ) row->release(row->value);
  mt_free((char *)row->point);
  mt_free((char *)row->name);
  mt_free(entry);
}

static void seam_release(metta *runtime)
{ size_t i;
  for (i = 0; i < runtime->nrows; i++) row_release(runtime->rows[i]);
  mt_free(runtime->rows);
  for (i = 0; i < runtime->npoints; i++) point_release(&runtime->points[i]);
  mt_free(runtime->points);
  /* The handles stay open on purpose and are not dlclose()d: a row may hold a
     function pointer into one, and unloading a library whose code is still
     reachable is a segfault with no line number on it. The process exiting is
     what releases them, which is also what sqlite3 does for a loaded
     extension. */
  mt_free(runtime->handles);
  runtime->rows = NULL;
  runtime->points = NULL;
  runtime->handles = NULL;
  runtime->nrows = runtime->cap_rows = 0;
  runtime->npoints = runtime->cap_points = 0;
  runtime->nhandles = runtime->cap_handles = 0;
}

static const char *seam_kind_str(mt_seam_kind kind)
{ switch (kind)
  { case MT_DECLARATION: return "declaration";
    case MT_OWNERSHIP:   return "ownership";
    case MT_EVENT:       return "event";
    case MT_SERVICE:     return "service";
  }
  return NULL;
}

/* Every declared point, for a refusal that names them rather than saying no. */
static void seam_name_points(metta *runtime, char *out, size_t size)
{ size_t i, at = 0;
  out[0] = '\0';
  for (i = 0; i < runtime->npoints && at + 2 < size; i++)
  { int wrote = snprintf(out + at, size - at, "%s%s",
                         at ? ", " : "", runtime->points[i].name);
    if ( wrote < 0 ) break;
    at += (size_t)wrote;
  }
  if ( at == 0 && size ) snprintf(out, size, "none");
}

bool mt_point_declare(metta *runtime, mt_point point)
{ mt_point *slot;
  const char *kind;

  if ( !handle_ready(runtime, "mt_point_declare") ||
       !registry_writable("mt_point_declare") ) return false;
  if ( !point.name || !point.fields || !point.doc )
  { err_set(MT_MISUSE,
            "an extension point needs a name, its fields and what it decides");
    return false;
  }
  if ( !(kind = seam_kind_str(point.kind)) )
  { err_set(MT_MISUSE,
            "an extension point is one of declaration, ownership, event or "
            "service; %d is not one of them", (int)point.kind);
    return false;
  }
  if ( (slot = (mt_point *)mt_point_of(runtime, point.name)) )
  { err_set(MT_MISUSE,
            "extension point %s is already declared as %s with fields %s; a "
            "point has one kind",
            point.name, seam_kind_str(slot->kind), slot->fields);
    return false;
  }
  if ( runtime->npoints == runtime->cap_points )
  { size_t cap = runtime->cap_points ? runtime->cap_points * 2 : 8;
    mt_point *grown = mt_resize(runtime->points, cap * sizeof(*grown));
    if ( !grown )
    { err_set(MT_NOMEM, "out of memory declaring an extension point");
      return false;
    }
    runtime->points = grown;
    runtime->cap_points = cap;
  }
  slot = &runtime->points[runtime->npoints];
  slot->name = mt_strdup(point.name);
  slot->fields = mt_strdup(point.fields);
  slot->doc = mt_strdup(point.doc);
  slot->kind = point.kind;
  if ( !slot->name || !slot->fields || !slot->doc )
  { point_release(slot);
    err_set(MT_NOMEM, "out of memory naming an extension point");
    return false;
  }
  runtime->npoints++;
  return true;
}

size_t mt_point_count(metta *runtime)
{ return runtime ? runtime->npoints : 0;
}

const mt_point *mt_point_at(metta *runtime, size_t index)
{ if ( !runtime || index >= runtime->npoints ) return NULL;
  return &runtime->points[index];
}

const mt_point *mt_point_of(metta *runtime, const char *name)
{ size_t i;
  if ( !runtime || !name ) return NULL;
  for (i = 0; i < runtime->npoints; i++)
    if ( strcmp(runtime->points[i].name, name) == 0 ) return &runtime->points[i];
  return NULL;
}

/* The rows against one point, without copying: an index into the flat array,
   which is what makes mt_row_at() a walk rather than a build. */
static mt_row_entry *row_entry_of(metta *runtime, const char *point, const char *name)
{ size_t i;
  for (i = 0; i < runtime->nrows; i++)
    if ( strcmp(runtime->rows[i]->row.point, point) == 0 &&
         strcmp(runtime->rows[i]->row.name, name) == 0 )
      return runtime->rows[i];
  return NULL;
}

static mt_seam_row *row_of(metta *runtime, const char *point, const char *name)
{ mt_row_entry *entry = row_entry_of(runtime, point, name);
  return entry ? &entry->row : NULL;
}

bool mt_register(metta *runtime, mt_seam_row row)
{ const mt_point *point;
  mt_seam_row *slot;
  mt_row_entry *entry;
  size_t at;
  char known[512];

  if ( !handle_ready(runtime, "mt_register") || !registry_writable("mt_register") )
    return false;
  if ( !row.point || !row.name )
  { err_set(MT_MISUSE, "a registration needs a point and a name");
    return false;
  }
  if ( !(point = mt_point_of(runtime, row.point)) )
  { seam_name_points(runtime, known, sizeof(known));
    err_set(MT_MISUSE,
            "no extension point named %s; this seat declares: %s",
            row.point, known);
    return false;
  }
  if ( point->kind == MT_SERVICE )
  { err_set(MT_MISUSE,
            "%s is a service point, which the SEAT writes; a registrant "
            "calls it rather than registering against it", row.point);
    return false;
  }
  if ( point->kind == MT_OWNERSHIP && !row.claims )
  { err_set(MT_MISUSE,
            "an ownership point is consulted through a row's claims(), so a "
            "registration against %s needs one", row.point);
    return false;
  }
  slot = row_of(runtime, row.point, row.name);
  if ( runtime->nrows == runtime->cap_rows )
  { size_t cap = runtime->cap_rows ? runtime->cap_rows * 2 : 16;
    mt_row_entry **grown = mt_resize(runtime->rows, cap * sizeof(*grown));
    if ( !grown )
    { err_set(MT_NOMEM, "out of memory recording a registration");
      return false;
    }
    runtime->rows = grown;
    runtime->cap_rows = cap;
  }
  entry = mt_calloc(1, sizeof(*entry));
  if ( !entry ) return false;
  entry->refs = 1;
  entry->row = row;
  entry->row.point = mt_strdup(row.point);
  entry->row.name = mt_strdup(row.name);
  if ( !entry->row.point || !entry->row.name )
  { mt_free((char *)entry->row.point);
    mt_free((char *)entry->row.name);
    mt_free(entry);
    err_set(MT_NOMEM, "out of memory naming a registration");
    return false;
  }
  if ( slot )
  { for (at = 0; &runtime->rows[at]->row != slot; at++) {}
    row_release(runtime->rows[at]);
    runtime->rows[at] = entry;
  } else runtime->rows[runtime->nrows++] = entry;
  return true;
}

bool mt_unregister(metta *runtime, const char *point, const char *name)
{ mt_seam_row *slot;
  size_t at;

  if ( !handle_ready(runtime, "mt_unregister") || !registry_writable("mt_unregister") )
    return false;
  if ( !point || !name ) return false;
  if ( !(slot = row_of(runtime, point, name)) ) return false;
  for (at = 0; &runtime->rows[at]->row != slot; at++) {}
  row_release(runtime->rows[at]);
  memmove(&runtime->rows[at], &runtime->rows[at + 1],
          (runtime->nrows - at - 1) * sizeof(*runtime->rows));
  runtime->nrows--;
  return true;
}

size_t mt_seam_count(metta *runtime, const char *point)
{ size_t i, total = 0;
  if ( !runtime || !point ) return 0;
  for (i = 0; i < runtime->nrows; i++)
    if ( strcmp(runtime->rows[i]->row.point, point) == 0 ) total++;
  return total;
}

const mt_seam_row *mt_seam_at(metta *runtime, const char *point, size_t index)
{ size_t i, seen = 0;
  if ( !runtime || !point ) return NULL;
  for (i = 0; i < runtime->nrows; i++)
    if ( strcmp(runtime->rows[i]->row.point, point) == 0 && seen++ == index )
      return &runtime->rows[i]->row;
  return NULL;
}

const mt_seam_row *mt_claim(metta *runtime, const char *point, void *subject,
                            void **answer)
{ size_t i;
  const mt_point *declared;

  if ( !handle_ready(runtime, "mt_claim") ) return NULL;
  if ( !(declared = mt_point_of(runtime, point)) )
  { char known[512];
    seam_name_points(runtime, known, sizeof(known));
    err_set(MT_MISUSE, "no extension point named %s; this seat declares: %s",
            point ? point : "(null)", known);
    return NULL;
  }
  if ( declared->kind != MT_OWNERSHIP )
  { err_set(MT_MISUSE,
            "%s is a %s point, so mt_claim() is not how it is read; walk its "
            "rows with mt_seam_at()", point, seam_kind_str(declared->kind));
    return NULL;
  }
  for (i = 0; i < runtime->nrows; i++)
  { mt_seam_row *row = &runtime->rows[i]->row;
    void *claimed;
    if ( strcmp(row->point, point) != 0 || !row->claims ) continue;
    if ( (claimed = row->claims(row->value, subject)) )
    { if ( answer ) *answer = claimed;
      return row;
    }
  }
  return NULL;
}

/* --- how a C object prints ---------------------------------------- */

typedef struct mt_repr_entry
{ mt_text_fn text;
  void      *user;
} mt_repr_entry_t;

bool mt_repr(metta *runtime, const char *type_name, mt_text_fn text, void *user)
{ mt_repr_entry_t *entry;
  mt_seam_row row;

  if ( !handle_ready(runtime, "mt_repr") ) return false;
  if ( !type_name || !text )
  { err_set(MT_MISUSE, "mt_repr needs a type name and a function");
    return false;
  }
  if ( !(entry = mt_alloc(sizeof(*entry))) )
  { err_set(MT_NOMEM, "out of memory registering a rendering");
    return false;
  }
  entry->text = text;
  entry->user = user;
  memset(&row, 0, sizeof(row));
  row.point = "repr";
  row.name = type_name;
  row.value = entry;
  row.release = mt_free;
  if ( !mt_register(runtime, row) )
  { mt_free(entry);
    return false;
  }
  return true;
}

/* Called from Prolog through seam:grounded_text/2, once per rendering.
   It reads the blob QUIETLY rather than through blob_box(), which raises an
   existence error for a released object: grounded_text is an ownership seam,
   where declining is failing, and raising here turned an explicit release's
   own refusal into this predicate's [tested: tests/test_cmetta.c,
   an engine alias left by explicit release is refused without a
   dereference]. */
static foreign_t pl_cmetta_repr(term_t object, term_t out)
{ mt_box_t *box = NULL;
  void *blob = NULL;
  size_t len = 0;
  PL_blob_t *blob_type = NULL;
  const mt_seam_row *row;
  const mt_repr_entry_t *entry;
  const char *text;

  if ( PL_get_blob(object, &blob, &len, &blob_type) &&
       blob_type == &mt_object_blob && blob && len == sizeof(mt_box_t) )
    box = blob;
  if ( !box || !box->value || !box->type ) return FALSE;
  if ( !(row = row_of(&g_runtime, "repr", box->type)) ) return FALSE;
  entry = row->value;
  if ( !(text = entry->text(box->value, entry->user)) ) return FALSE;
  len = strlen(text);
  if ( !valid_utf8(text, len) ) return callback_error("mt_repr", mt_errmsg());
  return PL_unify_chars(out, PL_STRING | REP_UTF8, len, text);
}

/* --- atoms held somewhere that is not the engine ------------------- */

typedef struct mt_provider_entry
{ mt_provider provider;
} mt_provider_entry_t;

static void provider_entry_release(void *value)
{ mt_provider_entry_t *entry = value;
  if ( entry->provider.release ) entry->provider.release(entry->provider.user);
  mt_free(entry);
}

typedef struct provider_registration {
  const char *space;
  mt_provider provider;
  bool transferred;
} provider_registration;

static mt_status provider_open_body(metta *runtime, void *data)
{ mt_provider_entry_t *entry;
  provider_registration *registration = data;
  const char *space = registration->space;
  mt_provider provider = registration->provider;
  mt_seam_row row;
  fid_t f;
  term_t av, capability;
  mt_status status;
  const struct { const char *name; bool present; } capabilities[] = {
    {"add", provider.add != NULL}, {"remove", provider.remove != NULL},
    {"match", true}, {"enumerate", true}, {"clear", provider.clear != NULL}
  };

  if ( !space || !provider.match )
  { err_set(MT_MISUSE,
            "a provider needs a space name and a match callback");
    return MT_MISUSE;
  }
  if ( (provider.begin || provider.commit || provider.rollback) &&
       !(provider.begin && provider.commit && provider.rollback) )
    return err_set(MT_MISUSE, "provider begin, commit and rollback must be supplied together");
  if ( !(entry = mt_alloc(sizeof(*entry))) )
    return MT_NOMEM;
  entry->provider = provider;
  if ( !(f = frame_open("mt_provider_open")) )
  { mt_free(entry); return MT_NOMEM; }
  av = PL_new_term_refs(3);
  capability = PL_new_term_ref();
  if ( !av || !capability || !put_name(av, space) ||
       !PL_put_nil(av + 1) || !PL_put_bool(av + 2, provider.begin != NULL) )
  { status = mt_ok() ? err_set(MT_NOMEM, "cannot describe a provider's capabilities")
                     : mt_error(); goto done; }
  for (size_t i = sizeof(capabilities) / sizeof(*capabilities); i > 0; i--)
  { if ( capabilities[i - 1].present &&
         (!put_name(capability, capabilities[i - 1].name) ||
          !PL_cons_list(av + 1, capability, av + 1)) )
    { status = err_set(MT_NOMEM, "cannot retain a provider capability"); goto done; }
  }
  status = call_bridge("metta_c_open_provider", 3, av);
  if ( status != MT_OK ) goto done;
  row = (mt_seam_row){.point="provider", .name=space, .value=entry,
                      .release=provider_entry_release};
  if ( mt_register(runtime, row) )
  { registration->transferred = true; entry = NULL; }
  else status = mt_error();
done:
  frame_close(f);
  mt_free(entry);
  return status;
}

bool mt_provider_open(metta *runtime, const char *space, mt_provider provider)
{ provider_registration registration = {space, provider, false};
  mt_status status = mt_transaction(runtime, provider_open_body, &registration);
  if ( !registration.transferred && provider.release ) provider.release(provider.user);
  return status == MT_OK;
}

static mt_status provider_close_body(metta *runtime, void *data)
{ const char *space = data;
  fid_t f;
  term_t av;
  mt_status status;
  if ( !space ) return err_set(MT_MISUSE, "mt_provider_close needs a space name");
  if ( !row_of(runtime, "provider", space) ) return MT_FAIL;
  if ( !(f = frame_open("mt_provider_close")) ) return MT_NOMEM;
  av = PL_new_term_refs(1);
  if ( !av || !put_name(av, space) )
    status = mt_ok() ? err_set(MT_NOMEM, "cannot name the provider to close")
                     : mt_error();
  else status = call_bridge("metta_c_close_provider", 1, av);
  frame_close(f);
  if ( status == MT_OK && !mt_unregister(runtime, "provider", space) )
    return err_set(MT_ERROR, "the provider registration disappeared while closing");
  return status;
}

bool mt_provider_close(metta *runtime, const char *space)
{ return mt_transaction(runtime, provider_close_body, (void *)space) == MT_OK; }

static mt_row_entry *provider_row(term_t space)
{ char *name;
  size_t length;
  mt_row_entry *row;
  name = term_text(space, CVT_ATOM, &length);
  if ( !name ) { PL_type_error("atom", space); return NULL; }
  row = row_entry_of(&g_runtime, "provider", name);
  mt_free(name);
  if ( !row ) PL_existence_error("cmetta_provider", space);
  return row;
}

static mt_status provider_match(mt_call *call, void *data)
{ mt_provider_entry_t *entry = data;
  mt_iterator iterator = {0};
  int64_t limit = mt_int(mt_arg(call, 1));
  mt_status status = entry->provider.match(entry->provider.user, mt_arg(call, 0),
                                          (size_t)limit, &iterator);
  if ( status != MT_OK )
  { iterator_close(&iterator); return status; }
  return mt_answer_iter(call, iterator);
}

static foreign_t pl_cmetta_provider_query(term_t space, term_t args,
                                         term_t result, control_t control)
{ mt_row_entry *row;
  if ( PL_foreign_control(control) != PL_FIRST_CALL )
    return native_continue(result, control);
  if ( !(row = provider_row(space)) ) return FALSE;
  return run_call(row->row.name, provider_match, row->row.value, NULL, row, args, result);
}

/* The same captured registration owns a query and a transaction completion.
   The private release callback identifies the box structurally; a user-created
   object with the same display name cannot become a participant.
   [source: engine/ext_points.pl:foreign_participant/3; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c] */
static void provider_capture_release(void *row)
{ row_release(row); }

static foreign_t pl_cmetta_provider_identity(term_t space, term_t identity)
{ mt_row_entry *row = provider_row(space);
  mt_provider_entry_t *entry;
  if ( !row ) return FALSE;
  entry = row->row.value;
  return entry->provider.begin && PL_unify_pointer(identity, row);
}

static foreign_t pl_cmetta_provider_capture(term_t space, term_t identity, term_t held)
{ mt_row_entry *row = provider_row(space);
  void *expected;
  mt_box_t *box;
  mt_atom *atom;
  term_t out = PL_new_term_ref();
  foreign_t result;
  if ( !row || !PL_get_pointer(identity, &expected) || expected != row )
    return PL_permission_error("capture", "replaced_provider", space);
  box = box_new(row, "CProvider", provider_capture_release, NULL, NULL);
  if ( !box ) return PL_resource_error("memory");
  MT_INC(&row->refs);
  atom = object_from_box(box);
  if ( !atom ) return PL_resource_error("memory");
  result = out && put_atom(atom, out) && PL_unify(held, out);
  mt_drop(atom);
  return result;
}

static foreign_t provider_status(mt_status status, const char *name, uint64_t before)
{ if ( status == MT_OK ) return TRUE;
  return callback_error(name, g_error_generation != before && mt_errmsg()
                       ? mt_errmsg() : "provider callback did not return MT_OK");
}

static foreign_t pl_cmetta_provider_finish(term_t held, term_t operation)
{ mt_box_t *box = blob_box(held);
  mt_row_entry *row;
  mt_provider_entry_t *entry;
  char *name;
  size_t length;
  mt_status status;
  uint64_t before = g_error_generation;
  if ( !box || box->release != provider_capture_release )
    return PL_type_error("cmetta_provider_participant", held);
  row = box->value;
  entry = row->row.value;
  name = term_text(operation, CVT_ATOM, &length);
  if ( !name ) return PL_type_error("atom", operation);
  if ( strcmp(name, "begin") == 0 ) status = entry->provider.begin(entry->provider.user);
  else if ( strcmp(name, "commit") == 0 ) status = entry->provider.commit(entry->provider.user);
  else if ( strcmp(name, "rollback") == 0 ) status = entry->provider.rollback(entry->provider.user);
  else { mt_free(name); return PL_domain_error("provider_transaction_operation", operation); }
  mt_free(name);
  return provider_status(status, row->row.name, before);
}

/* Updates carry atoms directly, including opaque objects and counted text.
   Retaining the row also permits a callback to withdraw its own registration.
   Time: O(A) conversion, A atom nodes; no print/parse round trip.
   [tested: tests/test_providers.c; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c] */
static foreign_t pl_cmetta_provider(term_t space, term_t operation,
                                    term_t payload, term_t result)
{ mt_row_entry *row = provider_row(space);
  mt_provider_entry_t *entry;
  mt_atom *atom = NULL;
  char *name;
  size_t length;
  mt_status status;
  bool removed = false;
  foreign_t answered;
  uint64_t before;
  if ( !row ) return FALSE;
  name = term_text(operation, CVT_ATOM, &length);
  if ( !name ) return PL_type_error("atom", operation);
  MT_INC(&row->refs);
  entry = row->row.value;
  before = g_error_generation;
  if ( strcmp(name, "clear") == 0 && entry->provider.clear )
    status = entry->provider.clear(entry->provider.user);
  else if ( (strcmp(name, "add") == 0 && entry->provider.add) ||
            (strcmp(name, "remove") == 0 && entry->provider.remove) )
  { atom = decode(payload, 0);
    status = atom ? (strcmp(name, "add") == 0
                     ? entry->provider.add(entry->provider.user, atom)
                     : entry->provider.remove(entry->provider.user, atom, &removed))
                  : mt_error();
  }
  else status = err_set(MT_UNSUPPORTED, "provider %s has no %s callback", row->row.name, name);
  answered = provider_status(status, row->row.name, before);
  if ( answered ) answered = PL_unify_bool(result, strcmp(name, "remove") == 0 ? removed : true);
  mt_drop(atom);
  mt_free(name);
  row_release(row);
  return answered;
}

/* Committed change callbacks share registration lifetime with providers.
   The token distinguishes retired hook clauses from a same-name replacement.
   Time: O(R + A), R registration lookup and A decoded atom nodes per event.
   Space: O(A); a notification queues nothing in this library.
   [tested: tests/test_subscriptions.c; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c] */
typedef struct subscription_entry {
  mt_subscription subscription;
  uint64_t token;
} subscription_entry;
static uint64_t subscription_token;

static void subscription_release(void *value)
{ subscription_entry *entry = value;
  if ( entry->subscription.release ) entry->subscription.release(entry->subscription.user);
  mt_drop(entry->subscription.pattern);
  mt_free((char *)entry->subscription.space);
  mt_free(entry);
}

typedef struct subscription_registration {
  const char *name;
  mt_subscription subscription;
  bool transferred;
} subscription_registration;

static mt_status subscribe_body(metta *runtime, void *data)
{ subscription_registration *r = data;
  subscription_entry *entry;
  fid_t f;
  term_t av;
  mt_status status;
  if ( !r->name || !r->subscription.space || !r->subscription.pattern || !r->subscription.notify )
    return err_set(MT_MISUSE, "mt_subscribe needs a name, space, pattern and notify callback");
  if ( row_of(runtime, "subscription", r->name) )
    return err_set(MT_MISUSE, "subscription %s already exists", r->name);
  if ( subscription_token == UINT64_MAX )
    return err_set(MT_UNSUPPORTED, "subscription identifiers are exhausted");
  entry = mt_alloc(sizeof(*entry));
  if ( !entry ) return MT_NOMEM;
  *entry = (subscription_entry){r->subscription, ++subscription_token};
  entry->subscription.space = mt_strdup(r->subscription.space);
  r->transferred = true;
  if ( !entry->subscription.space ) { subscription_release(entry); return MT_NOMEM; }
  if ( !mt_register(runtime, (mt_seam_row){.point="subscription", .name=r->name,
                           .value=entry, .release=subscription_release}) )
  { subscription_release(entry); return mt_error(); }
  f = frame_open("mt_subscribe");
  if ( !f ) return MT_NOMEM;
  av = PL_new_term_refs(4);
  if ( !av || !put_name(av, r->name) || !PL_put_uint64(av + 1, entry->token) ||
       !put_name(av + 2, r->subscription.space) ||
       !put_atom(r->subscription.pattern, av + 3) )
    status = mt_ok() ? err_set(MT_NOMEM, "cannot encode a subscription") : mt_error();
  else status = call_bridge("metta_c_subscribe", 4, av);
  frame_close(f);
  return status;
}

bool mt_subscribe(metta *runtime, const char *name, mt_subscription subscription)
{ subscription_registration r = {name, subscription, false};
  mt_status status = mt_transaction(runtime, subscribe_body, &r);
  if ( !r.transferred )
  { mt_drop(subscription.pattern);
    if ( subscription.release ) subscription.release(subscription.user);
  }
  return status == MT_OK;
}

static mt_status unsubscribe_body(metta *runtime, void *data)
{ const char *name = data;
  fid_t f;
  term_t av;
  mt_status status;
  if ( !name ) return err_set(MT_MISUSE, "mt_unsubscribe needs a name");
  if ( !row_of(runtime, "subscription", name) ) return MT_FAIL;
  f = frame_open("mt_unsubscribe");
  if ( !f ) return MT_NOMEM;
  av = PL_new_term_ref();
  status = av && put_name(av, name)
         ? call_bridge("metta_c_unsubscribe", 1, av)
         : mt_ok() ? err_set(MT_NOMEM, "cannot name a subscription") : mt_error();
  frame_close(f);
  if ( status == MT_OK && !mt_unregister(runtime, "subscription", name) )
    return err_set(MT_ERROR, "subscription disappeared during cancellation");
  return status;
}

bool mt_unsubscribe(metta *runtime, const char *name)
{ return mt_transaction(runtime, unsubscribe_body, (void *)name) == MT_OK; }

static foreign_t pl_cmetta_notify(term_t name_term, term_t token_term,
                                  term_t added_term, term_t term)
{ char *name;
  size_t length;
  uint64_t token, before;
  int added;
  mt_row_entry *row;
  subscription_entry *entry;
  mt_atom *atom;
  mt_status status;
  foreign_t result;
  if ( !PL_get_uint64(token_term, &token) || !PL_get_bool(added_term, &added) ) return FALSE;
  name = term_text(name_term, CVT_ATOM, &length);
  if ( !name ) return PL_resource_error("memory");
  row = row_entry_of(&g_runtime, "subscription", name);
  mt_free(name);
  if ( !row ) return TRUE;
  entry = row->row.value;
  if ( entry->token != token ) return TRUE;
  MT_INC(&row->refs);
  before = g_error_generation;
  atom = decode(term, 0);
  status = atom ? entry->subscription.notify(entry->subscription.user, added != 0, atom)
                : mt_error();
  result = status == MT_OK ? TRUE : callback_error(row->row.name,
           g_error_generation != before && mt_errmsg() ? mt_errmsg()
            : "subscription callback did not return MT_OK");
  mt_drop(atom);
  row_release(row);
  return result;
}

/* --- a directory of sources this library ships --------------------- */

static bool register_library(metta *runtime, const char *alias, const char *directory)
{ fid_t f;
  term_t av;
  mt_status status;
  mt_seam_row row;
  char *held;

  if ( !handle_ready(runtime, "mt_library") ) return false;
  if ( !alias || !directory )
  { err_set(MT_MISUSE, "mt_library needs an alias and a directory");
    return false;
  }
  if ( !(f = frame_open("mt_library")) ) return false;
  av = PL_new_term_refs(3);
  if ( !av || !put_name(av, alias) ||
       !put_chars(av + 1, PL_ATOM | REP_FN, (size_t)-1, directory) )
  { PL_discard_foreign_frame(f);
    if ( mt_ok() ) err_set(MT_NOMEM, "out of memory naming a library path");
    return false;
  }
  status = call_bridge("metta_c_library_path", 3, av);
  PL_discard_foreign_frame(f);
  if ( status != MT_OK ) return false;

  if ( !(held = mt_strdup(directory)) )
  { err_set(MT_NOMEM, "out of memory recording a library path");
    return false;
  }
  memset(&row, 0, sizeof(row));
  row.point = "library";
  row.name = alias;
  row.value = held;
  row.release = mt_free;
  if ( !mt_register(runtime, row) )
  { mt_free(held);
    return false;
  }
  return true;
}

typedef struct library_registration { const char *alias, *directory; } library_registration;
static mt_status library_body(metta *runtime, void *data)
{ library_registration *r = data;
  return register_library(runtime, r->alias, r->directory) ? MT_OK
         : (mt_ok() ? MT_FAIL : mt_error());
}
bool mt_library(metta *runtime, const char *alias, const char *directory)
{ library_registration r = {alias, directory};
  return mt_transaction(runtime, library_body, &r) == MT_OK;
}

/* --- loading a library that extends this seat ---------------------- */

typedef struct extension_initialization { mt_extension_fn init; const char *path; } extension_initialization;
static mt_status extension_body(metta *runtime, void *data)
{ extension_initialization *extension = data;
  if ( extension->init(runtime) ) return MT_OK;
  if ( mt_ok() ) err_set(MT_ERROR, "%s: mt_extension_init answered false", extension->path);
  return mt_error();
}

bool mt_extension(metta *runtime, const char *path)
{ void *handle;
  mt_extension_fn init;
  void **grown;

  if ( !handle_ready(runtime, "mt_extension") ) return false;
  if ( !path )
  { err_set(MT_MISUSE, "mt_extension needs a path to a shared object");
    return false;
  }
  if ( !(handle = dlopen(path, RTLD_NOW | RTLD_LOCAL)) )
  { err_set(MT_ERROR, "cannot load %s: %s", path, dlerror());
    return false;
  }
  /* The cast every POSIX dlsym() user makes, because dlsym answers void* and
     ISO C has no conversion between an object pointer and a function pointer;
     POSIX requires this one to work [source: POSIX.1-2024, dlsym, RATIONALE]. */
  *(void **)(&init) = dlsym(handle, "mt_extension_init");
  if ( !init )
  { err_set(MT_MISUSE,
            "%s exports no mt_extension_init; a library extends this seat by "
            "exporting `bool mt_extension_init(metta *runtime)` and "
            "registering from it", path);
    dlclose(handle);
    return false;
  }
  if ( runtime->nhandles == runtime->cap_handles )
  { size_t cap = runtime->cap_handles ? runtime->cap_handles * 2 : 4;
    if ( !(grown = mt_resize(runtime->handles, cap * sizeof(*grown))) )
    { err_set(MT_NOMEM, "out of memory recording a loaded extension");
      dlclose(handle);
      return false;
    }
    runtime->handles = grown;
    runtime->cap_handles = cap;
  }
  runtime->handles[runtime->nhandles++] = handle;
  { extension_initialization extension = {init, path};
    return mt_transaction(runtime, extension_body, &extension) == MT_OK;
  }
}

/* The points this seat declares at boot, so every door it already had is a
   row and "what can I extend here" is a query from the first call. */
static bool seam_declare_shipped(metta *runtime)
{ static const mt_point shipped[] = {
    { "op", MT_DECLARATION, "name arity effect fn",
      "A C function MeTTa calls by name. mt_def() writes the row." },
    { "repr", MT_DECLARATION, "type text",
      "How a C object of one type prints in MeTTa. mt_repr() writes the row." },
    { "provider", MT_DECLARATION, "space add remove match clear begin commit rollback",
      "A space whose atoms this library holds. mt_provider_open() writes the "
      "row and the engine's foreign-space seam reads it." },
    { "subscription", MT_DECLARATION, "space pattern notify user release",
      "Committed additions and removals. mt_subscribe() writes the row." },
    { "library", MT_DECLARATION, "alias directory",
      "A directory of MeTTa or Prolog sources this library ships. "
      "mt_library() writes the row." }
  };
  size_t i;
  for (i = 0; i < sizeof(shipped) / sizeof(shipped[0]); i++)
    if ( !mt_point_declare(runtime, shipped[i]) ) return false;
  return true;
}

/* ================================================================== *
 * Publishing C functions
 * ================================================================== */

/* One struct argument, so the call site names what it is passing. Designated
   initializers are what C has instead of keyword arguments, and five
   positional parameters is exactly where they start paying. */
static bool define_operation(metta *runtime, mt_op op)
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
  if ( !(published = mt_strdup(name)) )
  { err_set(MT_NOMEM, "out of memory naming an operation");
    return false;
  }

  if ( !(f = frame_open("mt_def")) )
  { mt_free(published);
    return false;
  }
  av = PL_new_term_refs(3);
  if ( !av || !put_name(av, published) ||
       !PL_put_int64(av + 1, (int64_t)arity) ||
       !put_name(av + 2, kind) )
  { PL_discard_foreign_frame(f);
    mt_free(published);
    if ( mt_ok() ) err_set(MT_NOMEM, "out of memory registering an operation");
    return false;
  }
  status = call_bridge("metta_c_register_op", 3, av);
  PL_discard_foreign_frame(f);
  if ( status != MT_OK )
  { mt_free(published);
    return false;
  }

  slot = find_op(published, arity);
  if ( !slot && runtime->nops == runtime->cap_ops )
  { size_t cap = runtime->cap_ops ? runtime->cap_ops * 2 : 8;
    mt_op_entry_t *grown = mt_resize(runtime->ops, cap * sizeof(*grown));
    if ( !grown )
    { mt_free(published);
      err_set(MT_NOMEM, "out of memory recording an operation");
      return false;
    }
    runtime->ops = grown;
    runtime->cap_ops = cap;
  }
  { /* The seam's own record of it, so mt_row_at(runtime, "op", i) walks what
       this runtime publishes. The row's name is the PUBLISHED name, which is
       what a reader wants; its value says the arity and the effect class,
       which is everything else the declaration decided. */
    mt_seam_row row;
    char said[96];
    char *held;
    snprintf(said, sizeof(said), "%zu %s", arity, kind);
    held = mt_strdup(said);
    memset(&row, 0, sizeof(row));
    row.point = "op";
    row.name = published;
    row.value = held;
    row.release = mt_free;
    if ( !held || !mt_register(runtime, row) )
    { mt_free(held);
      mt_free(published);
      return false;
    }
  }
  if ( slot ) mt_free(published);
  else
  { slot = &runtime->ops[runtime->nops++];
    slot->name = published;
  }
  slot->arity = arity;
  slot->fn = fn;
  slot->user = user;
  return true;
}

static bool undefine_operation(metta *runtime, const char *name)
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
  if ( !(published = mt_strdup(name)) )
  { err_set(MT_NOMEM, "out of memory naming an operation");
    return false;
  }

  if ( !(f = frame_open("mt_undef")) )
  { mt_free(published);
    return false;
  }
  av = PL_new_term_refs(1);
  status = ( av && put_name(av, published) )
         ? call_bridge("metta_c_unregister_op", 1, av)
         : mt_ok() ? err_set(MT_NOMEM, "out of memory withdrawing an operation")
                   : mt_error();
  PL_discard_foreign_frame(f);

  if ( status != MT_OK ) { mt_free(published); return false; }
  for (i = 0; i < runtime->nops; )
  { if ( strcmp(runtime->ops[i].name, published) == 0 )
    { mt_free(runtime->ops[i].name);
      runtime->ops[i] = runtime->ops[--runtime->nops];
    } else i++;
  }
  (void)mt_unregister(runtime, "op", published);
  mt_free(published);
  return true;
}

/* Snapshot immutable registration ownership before entering a closed goal.
   Refcounted rows retain payloads across replacement; rollback needs no
   allocation. The engine owns its database rollback separately.
   Time and space: Theta(R + S), R registrations and S copied name bytes.
   [tested: tests/test_transactions.c; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c] */
static void registry_clear(metta *registry)
{ for (size_t i = 0; i < registry->nops; i++) mt_free(registry->ops[i].name);
  mt_free(registry->ops);
  for (size_t i = 0; i < registry->npoints; i++) point_release(&registry->points[i]);
  mt_free(registry->points);
  for (size_t i = 0; i < registry->nrows; i++) row_release(registry->rows[i]);
  mt_free(registry->rows);
}

static bool registry_copy(const metta *source, metta *copy)
{ *copy = (metta){0};
  if ( source->nops )
  { copy->ops = mt_calloc(source->nops, sizeof(*copy->ops));
    if ( !copy->ops ) goto failed;
    copy->cap_ops = source->nops;
    for (size_t i = 0; i < source->nops; i++)
    { copy->ops[i] = source->ops[i];
      copy->ops[i].name = mt_strdup(source->ops[i].name);
      copy->nops++;
      if ( !copy->ops[i].name ) goto failed;
    }
  }
  if ( source->npoints )
  { copy->points = mt_calloc(source->npoints, sizeof(*copy->points));
    if ( !copy->points ) goto failed;
    copy->cap_points = source->npoints;
    for (size_t i = 0; i < source->npoints; i++)
    { mt_point *p = &copy->points[i];
      p->name = mt_strdup(source->points[i].name);
      p->fields = mt_strdup(source->points[i].fields);
      p->doc = mt_strdup(source->points[i].doc);
      p->kind = source->points[i].kind;
      copy->npoints++;
      if ( !p->name || !p->fields || !p->doc ) goto failed;
    }
  }
  if ( source->nrows )
  { copy->rows = mt_calloc(source->nrows, sizeof(*copy->rows));
    if ( !copy->rows ) goto failed;
    copy->cap_rows = source->nrows;
    for (size_t i = 0; i < source->nrows; i++)
    { copy->rows[i] = source->rows[i];
      MT_INC(&copy->rows[i]->refs);
      copy->nrows++;
    }
  }
  return true;
failed:
  registry_clear(copy);
  return false;
}

typedef struct error_state {
  mt_status status;
  uint64_t generation;
  char message[MT_ERR_MAX], remedy[MT_ERR_MAX], ground[MT_ERR_MAX];
} error_state;

static void error_save(error_state *state)
{ state->status = g_status;
  state->generation = g_error_generation;
  memcpy(state->message, g_err, sizeof(g_err));
  memcpy(state->remedy, g_remedy, sizeof(g_remedy));
  memcpy(state->ground, g_ground, sizeof(g_ground));
}

static void error_restore(const error_state *state)
{ g_status = state->status;
  g_error_generation = state->generation;
  memcpy(g_err, state->message, sizeof(g_err));
  memcpy(g_remedy, state->remedy, sizeof(g_remedy));
  memcpy(g_ground, state->ground, sizeof(g_ground));
}

struct transaction_frame {
  metta *runtime;
  mt_scope_fn body;
  void *user;
  bool called, committed;
  mt_status status;
  error_state error;
  term_t scope; /* borrowed live goal, valid only during body(runtime, user) */
};

/* Match the actual enclosing transaction, not an equal copy of its goal.
   current_transaction/1 unifies the live goal; PL_same_compound compares its
   identity while this synchronous foreign callback roots it across nested calls.
   Time and space: O(1). No goal traversal or transaction-stack enumeration.
   [source: SWI-Prolog V10.1.14 src/pl-transaction.c:current_transaction,
   src/pl-fli.c:PL_same_compound; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c] */
static bool registry_writable(const char *door)
{ fid_t f = frame_open(door);
  term_t scope = f ? PL_new_term_ref() : 0;
  bool writable = false;
  if ( scope && call_bridge("metta_c_transaction_scope", 1, scope) == MT_OK )
  { writable = PL_get_nil(scope) ||
               (g_transaction && g_transaction->scope &&
                PL_same_compound(scope, g_transaction->scope));
    if ( !writable )
      err_set(MT_MISUSE, "%s needs a C rollback owner; enter every enclosing "
                        "transaction through mt_transaction before changing C registrations", door);
  }
  frame_close(f);
  return writable;
}

static bool transaction_ticket(term_t ticket)
{ void *pointer;
  return g_transaction && PL_get_pointer(ticket, &pointer) && pointer == g_transaction;
}

static foreign_t pl_cmetta_tx_body(term_t ticket)
{ transaction_frame *frame = g_transaction;
  uint64_t before = g_error_generation;
  if ( !transaction_ticket(ticket) || frame->called )
    return PL_permission_error("call", "cmetta_transaction", ticket);
  frame->called = true;
  frame->scope = PL_new_term_ref();
  if ( !frame->scope || call_bridge("metta_c_transaction_scope", 1, frame->scope) != MT_OK )
    frame->status = mt_ok() ? err_set(MT_NOMEM, "cannot retain the C transaction's scope") : mt_error();
  else
    frame->status = frame->body(frame->runtime, frame->user);
  frame->scope = 0;
  if ( frame->status == MT_OK ) return TRUE;
  if ( frame->status == MT_FAIL ) return FALSE;
  if ( frame->status < MT_ERROR || frame->status > MT_LIMIT )
    frame->status = err_set(MT_MISUSE, "transaction callback returned a nonterminal status");
  else if ( before == g_error_generation )
    err_set(frame->status, "transaction callback returned %s", mt_status_str(frame->status));
  error_save(&frame->error);
  return PL_resource_error("cmetta_transaction_callback");
}

static foreign_t pl_cmetta_tx_outcome(term_t ticket, term_t committed)
{ int value;
  if ( !transaction_ticket(ticket) || !PL_get_bool(committed, &value) ) return FALSE;
  g_transaction->committed = value != 0;
  return TRUE;
}

static mt_status transaction_run(metta *runtime, mt_scope_fn body, void *user,
                                  const char *predicate)
{ metta saved;
  transaction_frame frame = {.runtime = runtime, .body = body, .user = user};
  transaction_frame *previous = g_transaction;
  error_state prior_error;
  fid_t f;
  term_t ticket;
  mt_status status;
  if ( !handle_ready(runtime, predicate) ) return MT_MISUSE;
  if ( !body ) return err_set(MT_MISUSE, "a transaction needs a callback");
  if ( !registry_writable(predicate) ) return mt_error();
  f = frame_open(predicate);
  if ( !f ) return MT_NOMEM;
  ticket = PL_new_term_ref();
  if ( !registry_copy(runtime, &saved) ) { frame_close(f); return MT_NOMEM; }
  error_save(&prior_error);
  g_transaction = &frame;
  status = PL_put_pointer(ticket, &frame) ? call_bridge(predicate, 1, ticket)
                                        : err_set(MT_NOMEM, "cannot hold a transaction ticket");
  g_transaction = previous;
  if ( frame.committed ) registry_clear(&saved);
  else
  { registry_clear(runtime);
    runtime->ops = saved.ops; runtime->nops = saved.nops; runtime->cap_ops = saved.cap_ops;
    runtime->points = saved.points; runtime->npoints = saved.npoints; runtime->cap_points = saved.cap_points;
    runtime->rows = saved.rows; runtime->nrows = saved.nrows; runtime->cap_rows = saved.cap_rows;
  }
  frame_close(f);
  if ( frame.called && frame.status >= MT_ERROR )
  { error_restore(&frame.error); return frame.status; }
  if ( frame.called && frame.status == MT_FAIL )
  { error_restore(&prior_error); return MT_FAIL; }
  return status;
}

mt_status mt_transaction(metta *runtime, mt_scope_fn body, void *user)
{ return transaction_run(runtime, body, user, "metta_c_transaction");
}

mt_status mt_speculate(metta *runtime, mt_scope_fn body, void *user)
{ return transaction_run(runtime, body, user, "metta_c_speculate");
}

static mt_status define_body(metta *runtime, void *value)
{ return define_operation(runtime, *(mt_op *)value) ? MT_OK : mt_error(); }

bool mt_def(metta *runtime, mt_op op)
{ return mt_transaction(runtime, define_body, &op) == MT_OK; }

static mt_status undefine_body(metta *runtime, void *value)
{ return undefine_operation(runtime, value) ? MT_OK : mt_error(); }

bool mt_undef(metta *runtime, const char *name)
{ return mt_transaction(runtime, undefine_body, (void *)name) == MT_OK; }
