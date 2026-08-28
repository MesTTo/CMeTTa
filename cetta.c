/* Purpose: the C half of the C binding. Boot SWI-Prolog in this process,
 *   consult the engine, and move values between C structures and engine terms
 *   directly, with no wire encoding in between.
 *
 * Assumes:
 *   - SWI-Prolog 10 with threads [source: PLVERSION 100113]
 *   - extensions/cetta/bridge.pl is loaded by extensions/cetta/extension.pl, which
 *     the engine globs at boot, and which finds this file because cetta_open()
 *     registers '$cetta_present'/0 BEFORE it consults engine/metta.pl
 *   - a term handed out by the bridge is valid only inside the foreign frame
 *     the call opened, so every decode completes before the frame is discarded
 *     [source: SWI-Prolog.h:432-435; C1 in ai-cetta-c-constraints.md]
 *
 * Guarantees:
 *   - no Prolog exception crosses into a caller: every query runs under
 *     PL_Q_CATCH_EXCEPTION, and the ball is rendered by the bridge into the
 *     thread-local error text
 *   - an engine term with no MeTTa reading is REFUSED by name rather than
 *     stringified into something that cannot go home again
 *   - an ampersand-prefixed atom becomes CETTA_SPACE only when the engine
 *     says it is a space, which is a question this seat can ask and the
 *     out-of-process seats cannot [C5 in ai-cetta-c-constraints.md]
 *
 * Owns resources: the process's Prolog runtime, released by cetta_close(); the
 *   op table; one malloc'ed box per live cetta_object, released when both the
 *   C atom and the engine blob have let go.
 *
 * Guarded by: g_lock for the runtime singleton and the op table. Atoms are
 *   immutable after construction and their refcount is atomic, so reading one
 *   takes no lock.
 *
 * Decides: variables decode to their SOURCE names when the engine supplies a
 *   name state for them, and to SWI's written form otherwise, because a
 *   caller who wrote $x wants $x back and a caller reading an internal
 *   variable wants something stable rather than a lie.
 *
 * Open Obligations:
 *   To Do: None
 *   Hacks: None
 *   Future Enhancements: None
 */

/* strdup is POSIX 2008 rather than C11, and -std=c11 hides it. */
#define _POSIX_C_SOURCE 200809L

#include "cetta.h"

#include <SWI-Prolog.h>
#include <SWI-Stream.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SWI takes a foreign predicate as pl_function_t, which is void *. ISO C does
   not guarantee a function pointer converts to an object pointer, so the two
   are punned through a union: POSIX requires the conversion to work (dlsym
   returns void * for code), and a union member read is the one spelling that
   says so without a diagnostic. */
typedef void (*cetta_anyfn)(void);

static pl_function_t as_pl_function(cetta_anyfn fn)
{ union { cetta_anyfn code; pl_function_t data; } u;
  u.code = fn;
  return u.data;
}

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && \
    !defined(__STDC_NO_ATOMICS__)
#include <stdatomic.h>
#define CETTA_ATOMIC _Atomic
#define CETTA_INC(p) atomic_fetch_add_explicit((p), 1u, memory_order_relaxed)
#define CETTA_DEC(p) atomic_fetch_sub_explicit((p), 1u, memory_order_acq_rel)
#else
#define CETTA_ATOMIC
#define CETTA_INC(p) ((*(p))++)
#define CETTA_DEC(p) ((*(p))--)
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && \
    !defined(__STDC_NO_THREADS__)
#define CETTA_TLS _Thread_local
#elif defined(__GNUC__)
#define CETTA_TLS __thread
#else
#define CETTA_TLS
#endif

/* ================================================================== *
 * Error state
 * ================================================================== */

/* errno's contract: SET on failure, NOT cleared on success, so a run of calls
   is checked once at the end rather than one `if` per call. The state is
   thread-local, which is what errno itself became once threads existed. */
#define CETTA_ERR_MAX 2048
static CETTA_TLS char g_err[CETTA_ERR_MAX];
static CETTA_TLS cetta_status g_status = CETTA_OK;

static cetta_status err_set(cetta_status status, const char *fmt, ...)
{ va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_err, sizeof(g_err), fmt, ap);
  va_end(ap);
  g_status = status;
  return status;
}

/* The failing constructors answer NULL, so this spelling lets them set the
   reason and return in one line. */
static void *err_null(cetta_status status, const char *fmt, ...)
{ va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_err, sizeof(g_err), fmt, ap);
  va_end(ap);
  g_status = status;
  return NULL;
}

void cetta_clear(void)
{ g_err[0] = '\0';
  g_status = CETTA_OK;
}

cetta_status cetta_error(void)
{ return g_status;
}

bool cetta_ok(void)
{ return g_status == CETTA_OK;
}

const char *cetta_errmsg(void)
{ return g_status == CETTA_OK ? NULL : g_err;
}

const char *cetta_status_str(cetta_status status)
{ switch ( status )
  { case CETTA_OK:          return "ok";
    case CETTA_ROW:         return "row";
    case CETTA_DONE:        return "done";
    case CETTA_FAIL:        return "no answer";
    case CETTA_ERROR:       return "engine error";
    case CETTA_NOMEM:       return "out of memory";
    case CETTA_MISUSE:      return "misuse";
    case CETTA_UNSUPPORTED: return "unsupported value";
    case CETTA_LIMIT:       return "stopped by a bound";
  }
  return "unknown status";
}

const char *cetta_version(void)
{ return "0.1.0";
}

const char *cetta_kind_str(cetta_kind kind)
{ switch ( kind )
  { case CETTA_NONE:     return "None";
    case CETTA_SYMBOL:   return "Symbol";
    case CETTA_TEXT:     return "String";
    case CETTA_INT:      return "Number";
    case CETTA_FLOAT:    return "Number";
    case CETTA_BIGINT:   return "BigInt";
    case CETTA_RATIONAL: return "Rational";
    case CETTA_BOOL:     return "Bool";
    case CETTA_VARIABLE: return "Variable";
    case CETTA_EXPR:     return "Expression";
    case CETTA_SPACE:    return "Space";
    case CETTA_OBJECT:   return "Grounded";
    case CETTA_HANDLE:   return "Grounded";
  }
  return "unknown kind";
}

const char *cetta_effect_str(cetta_effect effect)
{ switch ( effect )
  { case CETTA_PURE:            return "pureStructural";
    case CETTA_LOOKUP:           return "readOnlyLookup";
    case CETTA_NONDET: return "nondeterministicReadOnly";
    case CETTA_WRITES:               return "writesState";
    case CETTA_IO:                  return "oracleIO";
  }
  return NULL;
}

/* ================================================================== *
 * Atoms
 * ================================================================== */

/* A live C value the language carries by reference. Two owners share one box:
   the C atom that names it and, once it has crossed, the engine blob. Each
   drops a reference; the last one out runs the caller's release. */
typedef struct cetta_box
{ CETTA_ATOMIC unsigned refs;
  void                 *value;
  char                 *type;
  cetta_free_fn  release;
  cetta_fn           apply;
  void                 *user;
} cetta_box_t;

struct cetta_atom
{ CETTA_ATOMIC unsigned refs;
  cetta_kind          kind;
  union
  { struct { char *text; size_t len; }        t;  /* sym var str space bigint */
    int64_t                                   i;
    double                                    f;
    bool                                      b;
    struct { int64_t num, den; }               r;
    struct { cetta_atom **kids; size_t n; }  e;
    cetta_box_t                               *box;
  } u;
};

static cetta_atom *atom_alloc(cetta_kind kind)
{ cetta_atom *a = calloc(1, sizeof(*a));
  if ( !a )
  { err_set(CETTA_NOMEM, "out of memory allocating an atom");
    return NULL;
  }
  a->refs = 1;
  a->kind = kind;
  return a;
}

static cetta_atom *atom_text(cetta_kind kind, const char *text, size_t len)
{ cetta_atom *a;
  if ( !text )
  { err_set(CETTA_MISUSE, "%s needs text, not NULL", cetta_kind_str(kind));
    return NULL;
  }
  if ( !(a = atom_alloc(kind)) ) return NULL;
  if ( !(a->u.t.text = malloc(len + 1)) )
  { free(a);
    err_set(CETTA_NOMEM, "out of memory copying %zu bytes of text", len);
    return NULL;
  }
  memcpy(a->u.t.text, text, len);
  a->u.t.text[len] = '\0';
  a->u.t.len = len;
  return a;
}

static void box_release(cetta_box_t *box)
{ if ( !box ) return;
  if ( CETTA_DEC(&box->refs) == 1 )
  { if ( box->release ) box->release(box->value);
    free(box->type);
    free(box);
  }
}

cetta_atom *cetta_keep(const cetta_atom *atom)
{ cetta_atom *a = (cetta_atom *)atom;
  if ( a ) CETTA_INC(&a->refs);
  return a;
}

void cetta_drop(const cetta_atom *atom)
{ cetta_atom *a = (cetta_atom *)atom;
  if ( !a ) return;
  if ( CETTA_DEC(&a->refs) != 1 ) return;

  switch ( a->kind )
  { case CETTA_SYMBOL:
    case CETTA_VARIABLE:
    case CETTA_TEXT:
    case CETTA_SPACE:
    case CETTA_BIGINT:
    case CETTA_HANDLE:
      free(a->u.t.text);
      break;
    case CETTA_EXPR:
    { size_t i;
      for (i = 0; i < a->u.e.n; i++) cetta_drop(a->u.e.kids[i]);
      free(a->u.e.kids);
      break;
    }
    case CETTA_OBJECT:
      box_release(a->u.box);
      break;
    default:
      break;
  }
  free(a);
}

cetta_atom *cetta_sym(const char *name)
{ return name ? atom_text(CETTA_SYMBOL, name, strlen(name)) : NULL;
}

cetta_atom *cetta_var(const char *name)
{ return name ? atom_text(CETTA_VARIABLE, name, strlen(name)) : NULL;
}

cetta_atom *cetta_text(const char *text)
{ return text ? atom_text(CETTA_TEXT, text, strlen(text)) : NULL;
}

cetta_atom *cetta_textn(const char *text, size_t length)
{ return atom_text(CETTA_TEXT, text, length);
}

cetta_atom *cetta_num(int64_t value)
{ cetta_atom *a = atom_alloc(CETTA_INT);
  if ( a ) a->u.i = value;
  return a;
}

cetta_atom *cetta_real(double value)
{ cetta_atom *a = atom_alloc(CETTA_FLOAT);
  if ( a ) a->u.f = value;
  return a;
}

cetta_atom *cetta_bool(bool value)
{ cetta_atom *a = atom_alloc(CETTA_BOOL);
  if ( a ) a->u.b = value;
  return a;
}

cetta_atom *cetta_bigint(const char *decimal)
{ const char *p = decimal;
  if ( !decimal )
  { err_set(CETTA_MISUSE, "cetta_bigint needs decimal digits, not NULL");
    return NULL;
  }
  if ( *p == '-' ) p++;
  if ( !*p )
  { err_set(CETTA_MISUSE, "%s is not an integer", decimal);
    return NULL;
  }
  for (; *p; p++)
  { if ( *p < '0' || *p > '9' )
    { err_set(CETTA_MISUSE,
              "%s is not an integer: only decimal digits and a leading "
              "minus are read here", decimal);
      return NULL;
    }
  }
  return atom_text(CETTA_BIGINT, decimal, strlen(decimal));
}

cetta_atom *cetta_ratio(int64_t numerator, int64_t denominator)
{ cetta_atom *a;
  if ( denominator == 0 )
  { err_set(CETTA_MISUSE, "a rational cannot have a zero denominator");
    return NULL;
  }
  if ( !(a = atom_alloc(CETTA_RATIONAL)) ) return NULL;
  a->u.r.num = numerator;
  a->u.r.den = denominator;
  return a;
}

cetta_atom *cetta_spaceref(const char *name)
{ if ( !name || name[0] != '&' )
  { err_set(CETTA_MISUSE,
            "a space reference is written with a leading ampersand; %s is not",
            name ? name : "NULL");
    return NULL;
  }
  return atom_text(CETTA_SPACE, name, strlen(name));
}

cetta_atom *cetta_exprv(size_t count, cetta_atom **children)
{ cetta_atom *a;
  size_t i;
  bool bad = false;

  for (i = 0; i < count; i++)
    if ( !children[i] ) bad = true;

  if ( bad || !(a = atom_alloc(CETTA_EXPR)) )
  { /* Steal-on-success, release-on-failure: a NULL from an inner constructor
       must not leak the siblings that did succeed. */
    for (i = 0; i < count; i++) cetta_drop(children[i]);
    if ( bad )
      err_set(CETTA_MISUSE,
              "an expression child was NULL; the constructor that made it "
              "failed and cetta_errmsg() said why at the time");
    return NULL;
  }
  if ( count > 0 )
  { if ( !(a->u.e.kids = malloc(count * sizeof(*a->u.e.kids))) )
    { free(a);
      for (i = 0; i < count; i++) cetta_drop(children[i]);
      err_set(CETTA_NOMEM, "out of memory building an expression of %zu", count);
      return NULL;
    }
    memcpy(a->u.e.kids, children, count * sizeof(*a->u.e.kids));
  }
  a->u.e.n = count;
  return a;
}

/* cetta_atom_of() widens every integer type to long long and every floating
   type to long double before it dispatches, so there is one branch to land on
   rather than nine. These are those landings. */
cetta_atom *cetta_num_(long long value)      { return cetta_num((int64_t)value); }
cetta_atom *cetta_real_(long double value)   { return cetta_real((double)value); }
cetta_atom *cetta_same(cetta_atom *atom)     { return atom; }
cetta_atom *cetta_same_c(const cetta_atom *atom) { return (cetta_atom *)atom; }

cetta_atom *cetta_unit(void)
{ return cetta_exprv(0, NULL);
}

cetta_kind cetta_kind_of(const cetta_atom *atom)
{ return atom ? atom->kind : CETTA_NONE;
}

const char *cetta_name(const cetta_atom *atom)
{ if ( !atom ) return NULL;
  switch ( atom->kind )
  { case CETTA_SYMBOL:
    case CETTA_VARIABLE:
    case CETTA_TEXT:
    case CETTA_SPACE:
    case CETTA_BIGINT:
    case CETTA_HANDLE:
      return atom->u.t.text;
    default:
      return NULL;
  }
}

size_t cetta_name_len(const cetta_atom *atom)
{ return cetta_name(atom) ? atom->u.t.len : 0;
}

int64_t cetta_int(const cetta_atom *atom)
{ if ( !atom || atom->kind != CETTA_INT )
  { err_set(CETTA_MISUSE,
            "cetta_int wants an exact integer that fits int64_t; this is %s. "
            "A Float is not rounded here and a BigInt does not fit by "
            "definition; read those with cetta_float or cetta_name",
            atom ? cetta_kind_str(atom->kind) : "NULL");
    return 0;
  }
  return atom->u.i;
}

/* Promotes where nothing is lost and refuses where something would be, which
   is the lattice reading in decision 5 of the header. 2^53 is where a double
   stops holding every integer. */
#define CETTA_EXACT_IN_DOUBLE 9007199254740992LL

double cetta_float(const cetta_atom *atom)
{ if ( !atom )
  { err_set(CETTA_MISUSE, "cetta_float wants a Number; this is NULL");
    return 0.0;
  }
  switch ( atom->kind )
  { case CETTA_FLOAT:
      return atom->u.f;
    case CETTA_INT:
      if ( atom->u.i <= -CETTA_EXACT_IN_DOUBLE ||
           atom->u.i >= CETTA_EXACT_IN_DOUBLE )
      { err_set(CETTA_UNSUPPORTED,
                "%lld does not fit a double exactly, and rounding it here "
                "would answer a different number; read it with cetta_int",
                (long long)atom->u.i);
        return 0.0;
      }
      return (double)atom->u.i;
    case CETTA_RATIONAL:
      return (double)atom->u.r.num / (double)atom->u.r.den;
    default:
      err_set(CETTA_MISUSE, "cetta_float wants a Number; this is %s",
              cetta_kind_str(atom->kind));
      return 0.0;
  }
}

bool cetta_truth(const cetta_atom *atom)
{ if ( !atom || atom->kind != CETTA_BOOL )
  { err_set(CETTA_MISUSE, "cetta_truth wants a Bool; this is %s",
            atom ? cetta_kind_str(atom->kind) : "NULL");
    return false;
  }
  return atom->u.b;
}

bool cetta_ratio_of(const cetta_atom *atom,
                    int64_t *numerator, int64_t *denominator)
{ if ( !atom || atom->kind != CETTA_RATIONAL )
  { err_set(CETTA_MISUSE, "cetta_ratio_of wants a Rational; this is %s",
            atom ? cetta_kind_str(atom->kind) : "NULL");
    return false;
  }
  *numerator = atom->u.r.num;
  *denominator = atom->u.r.den;
  return true;
}

size_t cetta_len(const cetta_atom *atom)
{ return ( atom && atom->kind == CETTA_EXPR ) ? atom->u.e.n : 0;
}

const cetta_atom *cetta_at(const cetta_atom *atom, size_t index)
{ if ( !atom || atom->kind != CETTA_EXPR || index >= atom->u.e.n ) return NULL;
  return atom->u.e.kids[index];
}

bool cetta_eq(const cetta_atom *a, const cetta_atom *b)
{ size_t i;
  if ( a == b ) return true;
  if ( !a || !b || a->kind != b->kind ) return false;

  switch ( a->kind )
  { case CETTA_SYMBOL:
    case CETTA_VARIABLE:
    case CETTA_TEXT:
    case CETTA_SPACE:
    case CETTA_BIGINT:
    case CETTA_HANDLE:
      return a->u.t.len == b->u.t.len &&
             memcmp(a->u.t.text, b->u.t.text, a->u.t.len) == 0;
    case CETTA_INT:      return a->u.i == b->u.i;
    case CETTA_FLOAT:    return a->u.f == b->u.f;
    case CETTA_BOOL:     return a->u.b == b->u.b;
    case CETTA_RATIONAL: return a->u.r.num == b->u.r.num &&
                                a->u.r.den == b->u.r.den;
    case CETTA_EXPR:
      if ( a->u.e.n != b->u.e.n ) return false;
      for (i = 0; i < a->u.e.n; i++)
        if ( !cetta_eq(a->u.e.kids[i], b->u.e.kids[i]) ) return false;
      return true;
    case CETTA_OBJECT:
      /* By identity: the whole point of a live value is that its contents
         never become comparable text. */
      return a->u.box == b->u.box;
    case CETTA_NONE:
      /* Unreachable: both atoms were proven non-NULL above. Named rather than
         defaulted so a kind added later is a compile error here. */
      break;
  }
  return false;
}

void *cetta_value(const cetta_atom *atom)
{ return ( atom && atom->kind == CETTA_OBJECT ) ? atom->u.box->value : NULL;
}

const char *cetta_type(const cetta_atom *atom)
{ return ( atom && atom->kind == CETTA_OBJECT ) ? atom->u.box->type : NULL;
}

static cetta_atom *object_from_box(cetta_box_t *box)
{ cetta_atom *a = atom_alloc(CETTA_OBJECT);
  if ( !a )
  { box_release(box);
    return NULL;
  }
  a->u.box = box;
  return a;
}

static cetta_box_t *box_new(void *value, const char *type_name,
                            cetta_free_fn release,
                            cetta_fn apply, void *user)
{ cetta_box_t *box = calloc(1, sizeof(*box));
  if ( !box )
  { err_set(CETTA_NOMEM, "out of memory boxing a C value");
    return NULL;
  }
  box->refs = 1;
  box->value = value;
  box->release = release;
  box->apply = apply;
  box->user = user;
  if ( type_name && !(box->type = strdup(type_name)) )
  { free(box);
    err_set(CETTA_NOMEM, "out of memory copying a type name");
    return NULL;
  }
  return box;
}

cetta_atom *cetta_object(void *value, const char *type_name,
                           cetta_free_fn release)
{ cetta_box_t *box = box_new(value, type_name, release, NULL, NULL);
  return box ? object_from_box(box) : NULL;
}

cetta_atom *cetta_function(cetta_fn fn, void *user,
                             cetta_free_fn release)
{ cetta_box_t *box;
  if ( !fn )
  { err_set(CETTA_MISUSE, "cetta_function needs a function, not NULL");
    return NULL;
  }
  box = box_new(user, "Function", release, fn, user);
  return box ? object_from_box(box) : NULL;
}

/* ================================================================== *
 * The runtime
 * ================================================================== */

typedef struct cetta_op_entry
{ char           *name;
  size_t          arity;
  cetta_fn     fn;
  void           *user;
} cetta_op_entry_t;

struct cetta
{ bool              open;
  char             *path;
  bool              verbose;
  cetta_limits    limits;
  cetta_op_entry_t *ops;
  size_t            nops, cap_ops;
};

static struct cetta g_runtime;
static bool         g_open = false;

struct cetta_space
{ cetta *runtime;
  char    *name;
  bool     borrowed;   /* &self and &metta live with the runtime */
};

static cetta_space g_self, g_catalog;

/* --- blob type for a live C value --------------------------------- */

static int object_write(IOSTREAM *s, atom_t a, int flags)
{ cetta_box_t *box = PL_blob_data(a, NULL, NULL);
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

static PL_blob_t cetta_object_blob =
{ .magic   = PL_BLOB_MAGIC,
  .flags   = PL_BLOB_NOCOPY,
  .name    = "cetta_object",
  .release = object_release_blob,
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
   tests/test_cetta.c, test_a_bound_stops_a_runaway_and_says_so;
   commit=0c544dba163996ab34fec1cb574f5f4faf8b53f0]. Releasing from
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

static cetta_status call_bridge(const char *name, int arity, term_t av);

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
   (PL_initialise is process-wide, see cetta.h's "Fails when") and a
   predicate_t stays valid for the life of that process. */
static bool is_space(term_t t)
{ static predicate_t space_operand = NULL;
  fid_t f;
  int rc;

  if ( !space_operand )
    space_operand = PL_predicate("metta_c_space_operand", 1, "user");
  f = PL_open_foreign_frame();
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

static cetta_atom *decode(term_t t, term_t names);

static cetta_atom *decode_list(term_t t, term_t names)
{ cetta_atom **kids = NULL;
  size_t n = 0, cap = 4;
  term_t head = PL_new_term_ref();
  term_t tail = PL_copy_term_ref(t);

  if ( !(kids = malloc(cap * sizeof(*kids))) )
  { err_set(CETTA_NOMEM, "out of memory decoding an expression");
    return NULL;
  }
  while ( PL_get_list(tail, head, tail) )
  { cetta_atom *kid = decode(head, names);
    if ( !kid )
    { size_t i;
      for (i = 0; i < n; i++) cetta_drop(kids[i]);
      free(kids);
      return NULL;
    }
    if ( n == cap )
    { cetta_atom **grown = realloc(kids, (cap *= 2) * sizeof(*kids));
      if ( !grown )
      { size_t i;
        cetta_drop(kid);
        for (i = 0; i < n; i++) cetta_drop(kids[i]);
        free(kids);
        err_set(CETTA_NOMEM, "out of memory decoding an expression");
        return NULL;
      }
      kids = grown;
    }
    kids[n++] = kid;
  }
  if ( !PL_get_nil(tail) )
  { size_t i;
    for (i = 0; i < n; i++) cetta_drop(kids[i]);
    free(kids);
    err_set(CETTA_UNSUPPORTED,
            "a partial list is not a MeTTa expression; the engine handed back "
            "a term with an unbound or non-list tail");
    return NULL;
  }
  { cetta_atom *out = cetta_exprv(n, kids);   /* steals the children */
    free(kids);
    return out;
  }
}

static cetta_atom *decode_number(term_t t)
{ int64_t i;
  double d;

  if ( PL_is_integer(t) )
  { if ( PL_get_int64(t, &i) ) return cetta_num(i);
    { size_t len;
      char *text = term_text(t, CVT_INTEGER, &len);
      cetta_atom *a;
      if ( !text )
      { err_set(CETTA_NOMEM, "out of memory reading a wide integer");
        return NULL;
      }
      a = atom_text(CETTA_BIGINT, text, len);
      free(text);
      return a;
    }
  }
  if ( PL_is_float(t) )
  { if ( PL_get_float(t, &d) ) return cetta_real(d);
    err_set(CETTA_UNSUPPORTED, "a float the C boundary cannot read");
    return NULL;
  }
  if ( PL_is_rational(t) )
  { fid_t f = PL_open_foreign_frame();
    term_t av = PL_new_term_refs(3);
    cetta_atom *a = NULL;
    int64_t num, den;
    if ( PL_unify(av, t) &&
         call_bridge("metta_c_rational_parts", 3, av) == CETTA_OK &&
         PL_get_int64(av + 1, &num) && PL_get_int64(av + 2, &den) )
      a = cetta_ratio(num, den);
    else
      err_set(CETTA_UNSUPPORTED,
              "a rational whose halves do not fit int64_t; C has no type for "
              "it and rounding it would be a different number");
    PL_discard_foreign_frame(f);
    return a;
  }
  err_set(CETTA_UNSUPPORTED, "a number of no kind this binding reads");
  return NULL;
}

static cetta_atom *decode(term_t t, term_t names)
{ if ( PL_is_variable(t) )
  { char *name = variable_name(names, t);
    cetta_atom *a;
    if ( !name )
    { err_set(CETTA_NOMEM, "out of memory naming a variable");
      return NULL;
    }
    a = atom_text(CETTA_VARIABLE, name, strlen(name));
    free(name);
    return a;
  }

  /* Before the atom test: [] is a list in SWI 7 and later, and the empty
     expression is unit rather than a name. */
  if ( PL_get_nil(t) || PL_is_list(t) ) return decode_list(t, names);

  if ( PL_is_integer(t) || PL_is_float(t) || PL_is_rational(t) )
    return decode_number(t);

  if ( PL_is_string(t) )
  { size_t len;
    char *text = term_text(t, CVT_STRING, &len);
    cetta_atom *a;
    if ( !text )
    { err_set(CETTA_NOMEM, "out of memory reading a string");
      return NULL;
    }
    a = atom_text(CETTA_TEXT, text, len);
    free(text);
    return a;
  }

  /* Before the atom branch, and it has to be: every SWI atom is a blob
     underneath, but PL_is_atom() is FALSE for a blob whose type does not
     carry PL_BLOB_TEXT, so a native value asked about that way is neither an
     atom nor anything else and falls off the end
     [measured 2026-08-27: a cetta_object reached the refusal branch and the
     dispatcher answered "No permission to read argument `<counter>'";
     tested: tests/test_cetta.c, test_a_c_value_crosses_by_reference;
     commit=0c544dba163996ab34fec1cb574f5f4faf8b53f0].
     The PL_BLOB_TEXT mask is the other half: without it an ordinary symbol
     reads as a native value instead. */
  { void *blob;
    PL_blob_t *type;
    if ( PL_get_blob(t, &blob, NULL, &type) && !(type->flags & PL_BLOB_TEXT) )
    { size_t len;
      char *text;
      cetta_atom *a;
      if ( type == &cetta_object_blob )
      { cetta_box_t *box = blob;
        CETTA_INC(&box->refs);
        return object_from_box(box);
      }
      /* Somebody else's blob: a native engine value. It crosses by reference
         and prints as itself, which is the `h` tag's whole contract. */
      text = term_text(t, CVT_WRITE, &len);
      if ( !text )
      { err_set(CETTA_NOMEM, "out of memory naming a native value");
        return NULL;
      }
      a = atom_text(CETTA_HANDLE, text, len);
      free(text);
      return a;
    }
  }

  if ( PL_is_atom(t) )
  { size_t len;
    char *text;
    cetta_atom *a;

    if ( !(text = term_text(t, CVT_ATOM, &len)) )
    { err_set(CETTA_NOMEM, "out of memory reading a symbol");
      return NULL;
    }
    if ( strcmp(text, "true") == 0 || strcmp(text, "false") == 0 )
    { a = cetta_bool(text[0] == 't');
      free(text);
      return a;
    }
    a = atom_text(is_space(t) ? CETTA_SPACE : CETTA_SYMBOL,
                  text, len);
    free(text);
    return a;
  }

  { size_t len;
    char *text = term_text(t, CVT_WRITE, &len);
    err_set(CETTA_UNSUPPORTED,
            "the engine answered %s, which is a Prolog term with no MeTTa "
            "reading; this binding refuses it rather than turning it into a "
            "symbol that cannot go home again",
            text ? text : "a term this binding could not even print");
    free(text);
    return NULL;
  }
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

static bool encode(const cetta_atom *a, term_t out, encode_ctx *ctx)
{ if ( !a )
  { err_set(CETTA_MISUSE, "cannot encode a NULL atom");
    return false;
  }
  switch ( a->kind )
  { case CETTA_SYMBOL:
    case CETTA_SPACE:
      return PL_put_atom_nchars(out, a->u.t.len, a->u.t.text);
    case CETTA_TEXT:
      return PL_put_string_nchars(out, a->u.t.len, a->u.t.text);
    case CETTA_VARIABLE:
      return encode_var(ctx, a->u.t.text, out);
    case CETTA_INT:
      return PL_put_int64(out, a->u.i);
    case CETTA_FLOAT:
      return PL_put_float(out, a->u.f);
    case CETTA_BOOL:
      return PL_put_atom_chars(out, a->u.b ? "true" : "false");
    case CETTA_BIGINT:
      return PL_put_term_from_chars(out, REP_UTF8, a->u.t.len, a->u.t.text);
    case CETTA_RATIONAL:
    { char buf[64];
      int n = snprintf(buf, sizeof(buf), "%lldr%lld",
                       (long long)a->u.r.num, (long long)a->u.r.den);
      return n > 0 && PL_put_term_from_chars(out, REP_UTF8, (size_t)n, buf);
    }
    case CETTA_OBJECT:
      CETTA_INC(&a->u.box->refs);
      return PL_put_blob(out, a->u.box, sizeof(*a->u.box), &cetta_object_blob);
    case CETTA_HANDLE:
      err_set(CETTA_UNSUPPORTED,
              "a native engine value cannot be sent back by its printed form: "
              "%s names it but is not it. Keep the answer's own atom and pass "
              "that instead", a->u.t.text);
      return false;
    case CETTA_NONE:
      err_set(CETTA_MISUSE, "cannot encode a NULL atom");
      return false;
    case CETTA_EXPR:
    { size_t i;
      term_t list = PL_new_term_ref();
      term_t item = PL_new_term_ref();
      if ( !PL_put_nil(list) ) return false;
      for (i = a->u.e.n; i > 0; i--)
      { if ( !encode(a->u.e.kids[i - 1], item, ctx) ) return false;
        if ( !PL_cons_list(list, item, list) ) return false;
      }
      return PL_put_term(out, list);
    }
  }
  err_set(CETTA_MISUSE, "an atom of no kind this binding writes");
  return false;
}

static bool put_atom(const cetta_atom *a, term_t out)
{ encode_ctx ctx = {0};
  bool ok = encode(a, out, &ctx);
  encode_ctx_free(&ctx);
  return ok;
}

/* The same, plus the Name-Var pairs the encode collected, which is what the
   engine's writer needs to print $x as $x rather than $_0. The list is built
   in the caller's frame and stays valid as long as `out` does. */
static bool put_atom_named(const cetta_atom *a, term_t out, term_t names)
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
{ fid_t f = PL_open_foreign_frame();
  term_t av = PL_new_term_refs(2);
  predicate_t p = PL_predicate("metta_c_error_text", 2, "user");
  qid_t q;

  if ( PL_unify(av, ball) &&
       (q = PL_open_query(NULL, PL_Q_CATCH_EXCEPTION, p, av)) )
  { if ( PL_next_solution(q) == TRUE )
    { char *text = term_text(av + 1, CVT_ATOM | CVT_STRING, NULL);
      if ( text )
      { snprintf(g_err, sizeof(g_err), "%s", text);
        g_status = CETTA_ERROR;
        free(text);
        PL_cut_query(q);
        PL_discard_foreign_frame(f);
        return;
      }
    }
    PL_cut_query(q);
  }
  { char *text = term_text(ball, CVT_WRITE, NULL);
    snprintf(g_err, sizeof(g_err), "%s",
             text ? text : "the engine raised a term this binding could not print");
    g_status = CETTA_ERROR;
    free(text);
  }
  PL_discard_foreign_frame(f);
}

/* Whether a caught ball is one of this binding's own bounds rather than a
   fault. A caller wants to tell "I stopped it" from "it broke", and those need
   different answers even though both arrive as exceptions. */
static bool ball_is_limit(term_t ball)
{ fid_t f = PL_open_foreign_frame();
  term_t av = PL_new_term_refs(3);
  predicate_t p = PL_predicate("metta_c_limit_ball", 3, "user");
  qid_t q;
  bool yes = false;

  if ( PL_unify(av, ball) &&
       (q = PL_open_query(NULL, PL_Q_CATCH_EXCEPTION, p, av)) )
  { yes = PL_next_solution(q) == TRUE;
    PL_cut_query(q);
  }
  PL_discard_foreign_frame(f);
  return yes;
}

/* Call a bridge predicate for its first solution, KEEPING its bindings, so the
   caller can read the output arguments out of av. The caller owns the frame. */
static cetta_status call_bridge(const char *name, int arity, term_t av)
{ predicate_t p = PL_predicate(name, arity, "user");
  qid_t q = PL_open_query(NULL, PL_Q_CATCH_EXCEPTION, p, av);
  int rc;
  cetta_status status;

  if ( !q )
    return err_set(CETTA_NOMEM, "could not open a query for %s/%d", name, arity);

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
      if ( saved )
      { fid_t f = PL_open_foreign_frame();
        term_t ball = PL_new_term_ref();
        cetta_status kind = CETTA_ERROR;
        if ( PL_recorded(saved, ball) )
        { render_ball(ball);
          if ( ball_is_limit(ball) ) kind = CETTA_LIMIT;
          /* render_ball() records the words under CETTA_ERROR, because that
             is all it can know. The classification happens here, and the
             STICKY status is what cetta_error() reads, so it has to carry the
             refined answer rather than the one the renderer left behind. */
          g_status = kind;
        }
        else err_set(CETTA_ERROR, "%s/%d raised a term that could not be "
                     "read back", name, arity);
        PL_discard_foreign_frame(f);
        PL_erase(saved);
        return kind;
      } else
        err_set(CETTA_ERROR, "%s/%d raised, and the ball could not be copied "
                "out of the query to be read", name, arity);
      return CETTA_ERROR;
    }
    PL_cut_query(q);
    return err_set(CETTA_FAIL, "%s/%d had no answer", name, arity);
  }
  status = CETTA_OK;
  /* Cut rather than close: close undoes the bindings the caller is about to
     read [source: SWI-Prolog manual, PL_cut_query vs PL_close_query]. */
  PL_cut_query(q);
  return status;
}

/* ================================================================== *
 * Foreign predicates the bridge calls back into
 * ================================================================== */

static foreign_t pl_cetta_present(void)
{ return TRUE;
}

struct cetta_call
{ cetta            *runtime;
  const cetta_atom **args;
  size_t              arity;
  cetta_atom       *result;
  bool                answered;
  char                error[CETTA_ERR_MAX];
  bool                failed;
};

size_t cetta_arity(const cetta_call *call)
{ return call->arity;
}

const cetta_atom *cetta_arg(const cetta_call *call, size_t index)
{ return index < call->arity ? call->args[index] : NULL;
}

cetta *cetta_of(const cetta_call *call)
{ return call->runtime;
}

cetta_status cetta_answer(cetta_call *call, cetta_atom *atom)
{ if ( call->answered )
  { cetta_drop(atom);
    return err_set(CETTA_MISUSE,
                   "this application already answered; a function that has "
                   "many answers returns one expression and lets superpose "
                   "spread it");
  }
  if ( !atom ) return err_set(CETTA_MISUSE, "cannot answer with a NULL atom");
  call->result = atom;
  call->answered = true;
  return CETTA_OK;
}

/* Returns CETTA_ERROR so an op can spell its refusal as one line:
       if ( !cetta_ok() ) return cetta_fail(call, "wanted two numbers"); */
cetta_status cetta_fail(cetta_call *call, const char *message)
{ snprintf(call->error, sizeof(call->error), "%s",
           message ? message : "the C function refused this application");
  call->failed = true;
  return CETTA_ERROR;
}

/* Run one C function against decoded arguments and unify its answer. Shared by
   a named operation and an applied function value. */
static foreign_t run_call(const char *name, cetta_fn fn, void *user,
                          term_t args, term_t result)
{ struct cetta_call call;
  cetta_atom **decoded = NULL;
  size_t n = 0, cap = 4, i;
  term_t head = PL_new_term_ref();
  term_t tail = PL_copy_term_ref(args);
  cetta_status status;
  foreign_t rc = FALSE;

  memset(&call, 0, sizeof(call));
  if ( !(decoded = malloc(cap * sizeof(*decoded))) )
    return PL_resource_error("memory");

  while ( PL_get_list(tail, head, tail) )
  { cetta_atom *a = decode(head, 0);
    if ( !a )
    { rc = PL_permission_error("read", "argument", head);
      goto done;
    }
    if ( n == cap )
    { cetta_atom **grown = realloc(decoded, (cap *= 2) * sizeof(*decoded));
      if ( !grown ) { cetta_drop(a); rc = PL_resource_error("memory"); goto done; }
      decoded = grown;
    }
    decoded[n++] = a;
  }

  call.runtime = &g_runtime;
  call.args = (const cetta_atom **)decoded;
  call.arity = n;

  status = fn(&call, user);

  if ( status == CETTA_OK && call.answered )
  { term_t out = PL_new_term_ref();
    rc = put_atom(call.result, out) && PL_unify(result, out);
  } else if ( status == CETTA_FAIL )
  { rc = FALSE;
  } else
  { /* An ISO error(Formal, Context) pair rather than a bare term, so SWI's own
       machinery carries it and bridge.pl's prolog:message//1 renders it. A
       bare cetta_error(...) printed as "Unknown message: ..."
       [measured 2026-08-27]. */
    const char *why = call.failed ? call.error
                    : (cetta_errmsg() ? cetta_errmsg()
                    : "the C function answered nothing");
    term_t ball = PL_new_term_ref();
    if ( PL_unify_term(ball,
                       PL_FUNCTOR_CHARS, "error", 2,
                         PL_FUNCTOR_CHARS, "cetta_operation_failed", 2,
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
  cetta_drop(call.result);
  for (i = 0; i < n; i++) cetta_drop(decoded[i]);
  free(decoded);
  return rc;
}

static cetta_op_entry_t *find_op(const char *name, size_t arity)
{ size_t i;
  for (i = 0; i < g_runtime.nops; i++)
    if ( g_runtime.ops[i].arity == arity &&
         strcmp(g_runtime.ops[i].name, name) == 0 )
      return &g_runtime.ops[i];
  return NULL;
}

static foreign_t pl_cetta_dispatch(term_t name, term_t args, term_t result)
{ char *text;
  size_t len;
  cetta_op_entry_t *op;
  size_t arity = 0;
  foreign_t rc;

  if ( PL_skip_list(args, 0, &arity) != PL_LIST )
    return PL_type_error("list", args);
  if ( !(text = term_text(name, CVT_ATOM | CVT_STRING, &len)) )
    return PL_type_error("atom", name);

  op = find_op(text, arity);
  if ( !op )
  { rc = PL_existence_error("cetta_operation", name);
    free(text);
    return rc;
  }
  { foreign_t answered = run_call(op->name, op->fn, op->user, args, result);
    free(text);
    return answered;
  }
}

static cetta_box_t *blob_box(term_t t)
{ void *blob;
  PL_blob_t *type;
  if ( PL_get_blob(t, &blob, NULL, &type) && type == &cetta_object_blob )
    return blob;
  return NULL;
}

static foreign_t pl_cetta_object_callable(term_t t)
{ cetta_box_t *box = blob_box(t);
  return ( box && box->apply ) ? TRUE : FALSE;
}

static foreign_t pl_cetta_apply(term_t t, term_t args, term_t result)
{ cetta_box_t *box = blob_box(t);
  if ( !box || !box->apply ) return FALSE;
  return run_call(box->type ? box->type : "function",
                  box->apply, box->user, args, result);
}

/* ================================================================== *
 * Boot
 * ================================================================== */

static bool goal(const char *text)
{ fid_t f = PL_open_foreign_frame();
  term_t t = PL_new_term_ref();
  bool ok = PL_chars_to_term(text, t) && PL_call(t, NULL);
  PL_discard_foreign_frame(f);
  return ok;
}

static char *default_path(void)
{ const char *env = getenv("METTA_PATH");
  return strdup(env && *env ? env : CETTA_ENGINE_PATH);
}

cetta *cetta_open(const cetta_config *config)
{ static char *argv[] = { (char *)"cetta", (char *)"-q",
                          (char *)"--no-signals", NULL };
  cetta_config defaults = {0};
  char *path;
  char *buf;
  size_t bufsz;

  if ( !config ) config = &defaults;

  if ( g_open )
  { /* One runtime per process; PL_initialise sets up the process's single
       Prolog heap and there is no second one to hand out. */
    if ( config->path && strcmp(config->path, g_runtime.path) != 0 )
      return err_null(CETTA_MISUSE,
                      "the engine was booted from %s and cannot be reopened "
                      "from %s: this process holds one runtime",
                      g_runtime.path, config->path);
    return &g_runtime;
  }

  path = config->path ? strdup(config->path) : default_path();
  if ( !path ) return err_null(CETTA_NOMEM, "out of memory recording the path");

  if ( !PL_is_initialised(NULL, NULL) && !PL_initialise(3, argv) )
  { free(path);
    return err_null(CETTA_ERROR, "SWI-Prolog would not initialise");
  }

  /* Registered BEFORE the consult, because engine/metta.pl reads
     extensions/ * /extension.pl while it loads and this seat's control file
     declares needs(predicate('$cetta_present'/0)). */
  PL_register_foreign("$cetta_present", 0,
                      as_pl_function((cetta_anyfn)pl_cetta_present), 0);
  PL_register_foreign("$cetta_dispatch", 3,
                      as_pl_function((cetta_anyfn)pl_cetta_dispatch), 0);
  PL_register_foreign("$cetta_object_callable", 1,
                      as_pl_function((cetta_anyfn)pl_cetta_object_callable), 0);
  PL_register_foreign("$cetta_apply", 3,
                      as_pl_function((cetta_anyfn)pl_cetta_apply), 0);
  PL_register_blob_type(&cetta_object_blob);

  bufsz = strlen(path) + 128;
  if ( !(buf = malloc(bufsz)) )
  { free(path);
    return err_null(CETTA_NOMEM, "out of memory building the boot goals");
  }

  if ( config->stack_limit )
  { snprintf(buf, bufsz, "set_prolog_flag(stack_limit, %zu)",
             config->stack_limit);
    if ( !goal(buf) )
    { free(path); free(buf);
      return err_null(CETTA_ERROR, "the stack limit %zu was refused",
                     config->stack_limit);
    }
  }

  /* `extensions` opts the engine into reading extensions/ * /extension.pl,
     and `silent` is how a host with no command line asks for quiet, because
     engine/filereader.pl reads argv at load time [C2]. */
  if ( !goal(config->verbose ? "set_prolog_flag(argv, [extensions])"
                             : "set_prolog_flag(argv, [silent, extensions])") )
  { free(path); free(buf);
    return err_null(CETTA_ERROR, "the engine refused its argv");
  }

  snprintf(buf, bufsz, "consult('%s/engine/metta.pl')", path);
  if ( !goal(buf) )
  { void *refused =
      err_null(CETTA_ERROR,
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

  g_self.runtime = &g_runtime;
  g_self.name = (char *)"&self";
  g_self.borrowed = true;
  g_catalog.runtime = &g_runtime;
  g_catalog.name = (char *)"&metta";
  g_catalog.borrowed = true;

  cetta_verbose(&g_runtime, config->verbose);
  return &g_runtime;
}

/* Defined with the show ring below; declared here because cetta_close comes
   first in the file and both lifecycle exits release it. */
static void show_ring_release(void);

void cetta_close(cetta *runtime)
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

bool cetta_verbose(cetta *runtime, bool verbose)
{ bool was = runtime->verbose;
  fid_t f = PL_open_foreign_frame();
  term_t av = PL_new_term_refs(1);
  /* The engine's own door, not a bridge predicate: bridge.pl carried a
     private copy of the engine's retract-then-assert until C2 was taken
     engine-side as metta_host_set_silent/1. filereader.pl exports it, so
     it resolves in `user` the way every other engine predicate this file
     reaches does. */
  if ( PL_put_atom_chars(av, verbose ? "false" : "true") &&
       call_bridge("metta_host_set_silent", 1, av) == CETTA_OK )
    runtime->verbose = verbose;
  PL_discard_foreign_frame(f);
  return was;
}

/* No runtime argument: there is one per process, so passing it said nothing. */
bool cetta_thread_attach(void)
{ if ( PL_thread_attach_engine(NULL) < 0 )
  { err_set(CETTA_ERROR, "this thread could not attach a Prolog engine");
    return false;
  }
  return true;
}

void cetta_thread_detach(void)
{ show_ring_release();
  PL_thread_destroy_engine();
}

/* ================================================================== *
 * Text
 * ================================================================== */

/* No runtime argument on the text doors either: they need the ENGINE, and
   there is one of those per process. Threading a handle through them was
   ceremony that never chose anything. */
cetta_atom *cetta_parse(const char *source)
{ fid_t f;
  term_t av;
  cetta_atom *out = NULL;

  if ( !source ) return err_null(CETTA_MISUSE, "cetta_parse needs source text");

  f = PL_open_foreign_frame();
  av = PL_new_term_refs(3);
  if ( !PL_put_string_chars(av, source) )
  { PL_discard_foreign_frame(f);
    return err_null(CETTA_NOMEM, "out of memory holding the source");
  }
  if ( call_bridge("metta_c_read", 3, av) == CETTA_OK )
    out = decode(av + 1, av + 2);
  PL_discard_foreign_frame(f);
  return out;
}

char *cetta_show_dup(const cetta_atom *atom)
{ fid_t f;
  term_t av;
  char *text = NULL;

  f = PL_open_foreign_frame();
  av = PL_new_term_refs(3);
  if ( put_atom_named(atom, av, av + 1) &&
       call_bridge("metta_c_show", 3, av) == CETTA_OK )
    text = term_text(av + 2, CVT_ATOM | CVT_STRING, NULL);
  PL_discard_foreign_frame(f);
  return text;
}

/* A rotating per-thread buffer, so the common use needs no free:

       printf("%s -> %s\n", cetta_show(pattern), cetta_show(answer));

   strerror(), inet_ntoa() and ctime() all hand back storage they own on the
   same terms. The ring is CETTA_SHOW_SLOTS deep rather than one slot deep so
   several renderings can be live in one printf, which one slot would not
   survive. */
static CETTA_TLS char *g_show[CETTA_SHOW_SLOTS];
static CETTA_TLS unsigned g_show_at;

const char *cetta_show(const cetta_atom *atom)
{ char *text = cetta_show_dup(atom);
  unsigned slot = g_show_at++ % CETTA_SHOW_SLOTS;

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
  for (i = 0; i < CETTA_SHOW_SLOTS; i++)
  { free(g_show[i]);
    g_show[i] = NULL;
  }
  g_show_at = 0;
}

void cetta_free(void *pointer)
{ free(pointer);
}

/* ================================================================== *
 * Spaces
 * ================================================================== */

cetta_space *cetta_self(cetta *runtime)
{ (void)runtime;
  return &g_self;
}

cetta_space *cetta_catalog(cetta *runtime)
{ (void)runtime;
  return &g_catalog;
}

const char *cetta_space_name(const cetta_space *space)
{ return space->name;
}

cetta_space *cetta_space_open(cetta *runtime, const char *name)
{ cetta_space *s;

  if ( !name || name[0] != '&' )
    return err_null(CETTA_MISUSE,
                    "a space is named with a leading ampersand; %s is not",
                    name ? name : "NULL");
  if ( strcmp(name, "&self") == 0 )  return &g_self;
  if ( strcmp(name, "&metta") == 0 ) return &g_catalog;

  if ( !(s = calloc(1, sizeof(*s))) )
    return err_null(CETTA_NOMEM, "out of memory opening a space");
  if ( !(s->name = strdup(name)) )
  { free(s);
    return err_null(CETTA_NOMEM, "out of memory naming a space");
  }
  s->runtime = runtime;
  return s;
}

void cetta_space_close(cetta_space *space)
{ if ( !space || space->borrowed ) return;
  free(space->name);
  free(space);
}

static cetta_status space_call(const char *pred, cetta_space *space,
                                 const cetta_atom *atom, int arity,
                                 term_t *avp, fid_t *fp)
{ fid_t f = PL_open_foreign_frame();
  term_t av = PL_new_term_refs(arity);
  cetta_status status;

  if ( !PL_put_atom_chars(av, space->name) ||
       ( atom && !put_atom(atom, av + 1) ) )
  { PL_discard_foreign_frame(f);
    return err_set(CETTA_MISUSE, "%s", cetta_errmsg() ? cetta_errmsg()
                                     : "the atom could not be written");
  }
  status = call_bridge(pred, arity, av);
  /* The vector is only handed back when the caller also takes the frame:
     a term_t outlives nothing once its frame is discarded. */
  if ( fp )
  { *fp = f;
    if ( avp ) *avp = av;
  } else
    PL_discard_foreign_frame(f);
  return status;
}

/* These TAKE their atom. Building one inline is the common shape, so the door
   that consumes it owns it; a caller keeping a term hands over cetta_keep(t).
   The atom is dropped whatever happens, including on failure, so no path
   leaks it. */
bool cetta_space_add(cetta_space *space, cetta_atom *atom)
{ cetta_status status = space_call("metta_c_add", space, atom, 2, NULL, NULL);
  cetta_drop(atom);
  return status == CETTA_OK;
}

bool cetta_space_del(cetta_space *space, cetta_atom *atom)
{ fid_t f;
  term_t av;
  cetta_status status = space_call("metta_c_remove", space, atom, 3, &av, &f);
  bool removed = false;

  if ( status == CETTA_OK )
  { char *text = term_text(av + 2, CVT_ATOM, NULL);
    removed = text && strcmp(text, "true") == 0;
    free(text);
  }
  PL_discard_foreign_frame(f);
  cetta_drop(atom);
  return removed;
}

size_t cetta_space_count(cetta_space *space)
{ fid_t f;
  term_t av;
  cetta_status status = space_call("metta_c_count", space, NULL, 2, &av, &f);
  int64_t n = 0;

  if ( status == CETTA_OK && !PL_get_int64(av + 1, &n) )
    err_set(CETTA_ERROR, "the space did not answer a count");
  PL_discard_foreign_frame(f);
  return status == CETTA_OK ? (size_t)n : 0;
}

bool cetta_space_wipe(cetta_space *space)
{ return space_call("metta_c_clear", space, NULL, 1, NULL, NULL) == CETTA_OK;
}

/* The &self halves of the same verbs, which is what a `cetta *` receiver
   reaches. Written out rather than generated so each one is greppable. */
bool cetta_self_add(cetta *runtime, cetta_atom *atom)
{ return cetta_space_add(cetta_self(runtime), atom); }
bool cetta_self_del(cetta *runtime, cetta_atom *atom)
{ return cetta_space_del(cetta_self(runtime), atom); }
size_t cetta_self_count(cetta *runtime)
{ return cetta_space_count(cetta_self(runtime)); }
bool cetta_self_wipe(cetta *runtime)
{ return cetta_space_wipe(cetta_self(runtime)); }

/* ================================================================== *
 * Answers
 * ================================================================== */

/* A cursor is one of two things wearing one face: a table of answers a run
   already computed, or an engine suspended between them. */
struct cetta_answers
{ cetta      *runtime;
  bool          lazy;
  int64_t       cursor_id;      /* lazy: the bridge's engine id     */
  cetta_atom **rows;          /* eager: every answer, in order    */
  char        **texts;
  size_t       *groups;
  size_t        n, at;
  bool          started, done;
  cetta_atom *current;
  char         *current_text;
  size_t        current_group;
};

static cetta_answers *answers_alloc(cetta *runtime)
{ cetta_answers *a = calloc(1, sizeof(*a));
  if ( !a ) err_set(CETTA_NOMEM, "out of memory opening a cursor");
  else a->runtime = runtime;
  return a;
}

/* Read the engine's Groups term: a list of groups, each a list of answers. */
static cetta_status collect_groups(term_t groups, cetta_answers *out)
{ term_t group = PL_new_term_ref();
  term_t gtail = PL_copy_term_ref(groups);
  term_t answer = PL_new_term_ref();
  size_t cap = 8, index = 0;

  if ( !(out->rows = malloc(cap * sizeof(*out->rows))) ||
       !(out->texts = malloc(cap * sizeof(*out->texts))) ||
       !(out->groups = malloc(cap * sizeof(*out->groups))) )
    return err_set(CETTA_NOMEM, "out of memory collecting answers");

  while ( PL_get_list(gtail, group, gtail) )
  { term_t atail = PL_copy_term_ref(group);
    while ( PL_get_list(atail, answer, atail) )
    { fid_t f = PL_open_foreign_frame();
      term_t av = PL_new_term_refs(4);
      cetta_atom *atom = NULL;
      char *text = NULL;

      if ( PL_unify(av, answer) &&
           call_bridge("metta_c_answer_parts", 4, av) == CETTA_OK )
      { atom = decode(av + 1, av + 2);
        text = term_text(av + 3, CVT_ATOM | CVT_STRING, NULL);
      }
      PL_discard_foreign_frame(f);

      if ( !atom )
      { free(text);
        return CETTA_UNSUPPORTED;
      }
      if ( out->n == cap )
      { cap *= 2;
        out->rows = realloc(out->rows, cap * sizeof(*out->rows));
        out->texts = realloc(out->texts, cap * sizeof(*out->texts));
        out->groups = realloc(out->groups, cap * sizeof(*out->groups));
        if ( !out->rows || !out->texts || !out->groups )
          return err_set(CETTA_NOMEM, "out of memory collecting answers");
      }
      out->rows[out->n] = atom;
      out->texts[out->n] = text;
      out->groups[out->n] = index;
      out->n++;
    }
    index++;
  }
  return CETTA_OK;
}

static cetta_status run_or_load(cetta *runtime, const char *pred,
                                  const char *argument, const char *space,
                                  cetta_answers **out)
{ fid_t f;
  term_t av;
  cetta_answers *answers;
  cetta_status status;

  *out = NULL;   /* zeroed FIRST: a caller reusing one variable across calls
                    would otherwise still hold the last cursor's pointer after
                    a failure, and free it twice. */
  if ( !argument ) return err_set(CETTA_MISUSE, "%s needs an argument", pred);
  if ( !(answers = answers_alloc(runtime)) ) return CETTA_NOMEM;

  f = PL_open_foreign_frame();
  av = PL_new_term_refs(5);
  if ( !PL_put_string_chars(av, argument) ||
       !PL_put_atom_chars(av + 1, space) ||
       !PL_put_float(av + 2, runtime->limits.seconds) ||
       !PL_put_int64(av + 3, (int64_t)runtime->limits.inferences) )
  { PL_discard_foreign_frame(f);
    cetta_answers_free(answers);
    return err_set(CETTA_NOMEM, "out of memory holding the argument");
  }
  status = call_bridge(pred, 5, av);
  if ( status == CETTA_OK ) status = collect_groups(av + 4, answers);
  PL_discard_foreign_frame(f);

  if ( status != CETTA_OK )
  { cetta_answers_free(answers);
    return status;
  }
  *out = answers;
  return CETTA_OK;
}

cetta_answers *cetta_run(cetta *runtime, const char *source)
{ cetta_answers *out = NULL;
  run_or_load(runtime, "metta_c_run", source, "&self", &out);
  return out;
}

cetta_answers *cetta_load(cetta *runtime, const char *path)
{ cetta_answers *out = NULL;
  run_or_load(runtime, "metta_c_load", path, "&self", &out);
  return out;
}

static cetta_status open_cursor(cetta_space *space, const char *pred,
                                  const cetta_atom *atom,
                                  cetta_answers **out)
{ fid_t f;
  term_t av;
  cetta_answers *answers;
  cetta_status status;
  int64_t id;

  *out = NULL;   /* see run_or_load: zeroed before anything can fail. */
  if ( !(answers = answers_alloc(space->runtime)) ) return CETTA_NOMEM;

  f = PL_open_foreign_frame();
  av = PL_new_term_refs(4);
  if ( !put_atom(atom, av) || !PL_put_atom_chars(av + 1, space->name) ||
       !PL_put_int64(av + 2, (int64_t)space->runtime->limits.inferences) )
  { PL_discard_foreign_frame(f);
    cetta_answers_free(answers);
    return err_set(CETTA_MISUSE, "%s", cetta_errmsg() ? cetta_errmsg()
                                     : "the goal could not be written");
  }
  status = call_bridge(pred, 4, av);
  if ( status == CETTA_OK && PL_get_int64(av + 3, &id) )
  { answers->lazy = true;
    answers->cursor_id = id;
  } else if ( status == CETTA_OK )
  { status = err_set(CETTA_ERROR, "the bridge did not answer a cursor id");
  }
  PL_discard_foreign_frame(f);

  if ( status != CETTA_OK )
  { cetta_answers_free(answers);
    return status;
  }
  *out = answers;
  return CETTA_OK;
}

/* These TAKE their atom, on the same reasoning as the write verbs: a goal is
   almost always built at the call site, and a door that consumes it is what
   makes cetta_eval(m, cetta_expr("+", 1, 2)) leak nothing. */
cetta_answers *cetta_space_eval(cetta_space *space, cetta_atom *goal)
{ cetta_answers *out = NULL;
  open_cursor(space, "metta_c_open_eval", goal, &out);
  cetta_drop(goal);
  return out;
}

cetta_answers *cetta_space_match(cetta_space *space, cetta_atom *pattern)
{ cetta_answers *out = NULL;
  open_cursor(space, "metta_c_open_match", pattern, &out);
  cetta_drop(pattern);
  return out;
}

cetta_answers *cetta_space_atoms(cetta_space *space)
{ /* Every stored atom is the match a fresh variable makes. */
  return cetta_space_match(space, cetta_var("_"));
}

cetta_answers *cetta_self_eval(cetta *runtime, cetta_atom *goal)
{ return cetta_space_eval(cetta_self(runtime), goal); }
cetta_answers *cetta_self_match(cetta *runtime, cetta_atom *pattern)
{ return cetta_space_match(cetta_self(runtime), pattern); }
cetta_answers *cetta_self_atoms(cetta *runtime)
{ return cetta_space_atoms(cetta_self(runtime)); }

static void clear_current(cetta_answers *answers)
{ if ( answers->lazy )
  { cetta_drop(answers->current);
    free(answers->current_text);
  }
  answers->current = NULL;
  answers->current_text = NULL;
}

static cetta_status answers_step(cetta_answers *answers)
{ fid_t f;
  term_t av;
  cetta_status status;
  term_t head, tail;

  if ( !answers ) return err_set(CETTA_MISUSE, "cetta_answers_step needs a cursor");
  if ( answers->done ) return CETTA_DONE;

  if ( !answers->lazy )
  { if ( answers->at >= answers->n )
    { answers->done = true;
      answers->current = NULL;
      answers->current_text = NULL;
      return CETTA_DONE;
    }
    answers->current = answers->rows[answers->at];
    answers->current_text = answers->texts[answers->at];
    answers->current_group = answers->groups[answers->at];
    answers->at++;
    return CETTA_ROW;
  }

  clear_current(answers);

  f = PL_open_foreign_frame();
  av = PL_new_term_refs(3);
  if ( !PL_put_int64(av, answers->cursor_id) ||
       !PL_put_float(av + 1, answers->runtime->limits.seconds) )
  { PL_discard_foreign_frame(f);
    return err_set(CETTA_NOMEM, "out of memory stepping a cursor");
  }
  status = call_bridge("metta_c_next", 3, av);
  if ( status != CETTA_OK )
  { PL_discard_foreign_frame(f);
    answers->done = true;
    return status;
  }

  head = PL_new_term_ref();
  tail = PL_copy_term_ref(av + 2);
  if ( !PL_get_list(tail, head, tail) )
  { PL_discard_foreign_frame(f);
    answers->done = true;
    return CETTA_DONE;
  }

  { term_t parts = PL_new_term_refs(4);
    if ( PL_unify(parts, head) &&
         call_bridge("metta_c_answer_parts", 4, parts) == CETTA_OK )
    { answers->current = decode(parts + 1, parts + 2);
      answers->current_text = term_text(parts + 3, CVT_ATOM | CVT_STRING, NULL);
    }
  }
  PL_discard_foreign_frame(f);

  if ( !answers->current )
  { answers->done = true;
    return CETTA_UNSUPPORTED;
  }
  answers->started = true;
  return CETTA_ROW;
}

/* One call per answer instead of step-then-read, so the loop condition and
   the value are the same expression. NULL ends the walk; cetta_ok() says
   whether that was exhaustion or a failure. */
const cetta_atom *cetta_next(cetta_answers *answers)
{ if ( !answers ) return NULL;
  return answers_step(answers) == CETTA_ROW ? answers->current : NULL;
}

/* The first answer, owned, with the cursor closed behind it. Consuming the
   cursor is what lets this compose in one expression. */
cetta_atom *cetta_first(cetta_answers *answers)
{ const cetta_atom *found;
  cetta_atom *owned = NULL;

  if ( !answers ) return NULL;
  if ( (found = cetta_next(answers)) != NULL ) owned = cetta_keep(found);
  cetta_answers_free(answers);
  return owned;
}

/* Every answer as one owned array, for a caller who wants them all rather
   than a walk. */
cetta_atom **cetta_all(cetta_answers *answers, size_t *n_out)
{ cetta_atom **items = NULL;
  size_t n = 0, cap = 0;
  const cetta_atom *found;

  if ( n_out ) *n_out = 0;
  if ( !answers ) return NULL;
  while ( (found = cetta_next(answers)) != NULL )
  { if ( n == cap )
    { size_t grown_cap = cap ? cap * 2 : 8;
      cetta_atom **grown = realloc(items, grown_cap * sizeof(*grown));
      if ( !grown )
      { cetta_atoms_free(items, n);
        cetta_answers_free(answers);
        return err_null(CETTA_NOMEM, "out of memory collecting answers");
      }
      items = grown;
      cap = grown_cap;
    }
    items[n++] = cetta_keep(found);
  }
  cetta_answers_free(answers);
  if ( n_out ) *n_out = n;
  return items;
}

/* Exactly one, or a recorded failure. Pulling the second answer is what makes
   the claim real, and it costs one step of a lazy cursor. */
cetta_atom *cetta_one(cetta_answers *answers)
{ const cetta_atom *found;
  cetta_atom *owned = NULL;

  if ( !answers ) return NULL;
  if ( (found = cetta_next(answers)) != NULL ) owned = cetta_keep(found);
  if ( !owned )
  { if ( cetta_ok() ) err_set(CETTA_FAIL, "the question had no answer");
  } else if ( cetta_next(answers) != NULL )
  { err_set(CETTA_MISUSE,
            "the question answered more than once, and cetta_one is a claim "
            "that it would not; use cetta_first to take the first, or "
            "cetta_each to walk them all");
    cetta_drop(owned);
    owned = NULL;
  }
  cetta_answers_free(answers);
  return owned;
}

/* Ask, read, and let go, which is the shape almost every question has: the
   caller wants the number, not an atom to look after. Each of these closes
   the cursor and drops the atom, so nothing is left owned. */
#define CETTA_ONE(name, type, read, zero)                                    \
  type name(cetta_answers *answers)                                          \
  { cetta_atom *a = cetta_one(answers);                                      \
    type value;                                                              \
    if ( !a ) return zero;                                                   \
    value = read(a);                                                         \
    cetta_drop(a);                                                           \
    return value;                                                            \
  }

CETTA_ONE(cetta_one_int,   int64_t, cetta_int,   0)
CETTA_ONE(cetta_one_float, double,  cetta_float, 0.0)
CETTA_ONE(cetta_one_truth, bool,    cetta_truth, false)
#undef CETTA_ONE

/* The text goes into the same rotating buffer cetta_show() writes, so the
   atom can be released here and the caller still has something to print. */
const char *cetta_one_name(cetta_answers *answers)
{ cetta_atom *a = cetta_one(answers);
  const char *shown;

  if ( !a ) return NULL;
  shown = cetta_show(a);
  cetta_drop(a);
  return shown;
}

void cetta_atoms_free(cetta_atom **atoms, size_t count)
{ size_t i;
  if ( !atoms ) return;
  for (i = 0; i < count; i++) cetta_drop(atoms[i]);
  free(atoms);
}

const char *cetta_answer_text(const cetta_answers *answers)
{ return answers ? answers->current_text : NULL;
}

size_t cetta_group(const cetta_answers *answers)
{ return answers ? answers->current_group : 0;
}

void cetta_answers_free(cetta_answers *answers)
{ size_t i;
  if ( !answers ) return;

  if ( answers->lazy )
  { fid_t f = PL_open_foreign_frame();
    term_t av = PL_new_term_refs(1);
    if ( PL_put_int64(av, answers->cursor_id) )
      call_bridge("metta_c_close", 1, av);
    PL_discard_foreign_frame(f);
    clear_current(answers);
  } else
  { for (i = 0; i < answers->n; i++)
    { cetta_drop(answers->rows[i]);
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

bool cetta_limit(cetta *runtime, const cetta_limits *limits)
{ static const cetta_limits none = {0, 0, 0};

  if ( !runtime ) { err_set(CETTA_MISUSE, "cetta_limit needs a runtime"); return false; }
  runtime->limits = limits ? *limits : none;

  if ( runtime->limits.stack_bytes )
  { char goal_text[96];
    snprintf(goal_text, sizeof(goal_text),
             "set_prolog_flag(stack_limit, %zu)", runtime->limits.stack_bytes);
    if ( !goal(goal_text) )
    { err_set(CETTA_ERROR, "the stack ceiling %zu was refused",
              runtime->limits.stack_bytes);
      return false;
    }
  }
  return true;
}

/* Returned by value. A three-scalar struct is cheaper to copy than to
   out-parameter, and the call site reads as an expression. */
cetta_limits cetta_limits_of(const cetta *runtime)
{ return runtime->limits;
}

/* ================================================================== *
 * Measuring
 * ================================================================== */

cetta_stats cetta_stats_now(cetta *runtime)
{ fid_t f;
  term_t av, head, tail;
  cetta_status status;
  double values[6] = {0, 0, 0, 0, 0, 0};
  cetta_stats out = {0, 0, 0, 0, 0, 0};
  size_t i = 0;

  (void)runtime;

  f = PL_open_foreign_frame();
  av = PL_new_term_refs(1);
  status = call_bridge("metta_c_stats", 1, av);
  if ( status == CETTA_OK )
  { head = PL_new_term_ref();
    tail = PL_copy_term_ref(av);
    while ( i < 6 && PL_get_list(tail, head, tail) )
    { int64_t whole;
      if ( PL_get_int64(head, &whole) ) values[i] = (double)whole;
      else if ( !PL_get_float(head, &values[i]) ) values[i] = 0;
      i++;
    }
    if ( i < 6 )
      status = err_set(CETTA_ERROR,
                       "the engine answered %zu counters where six were "
                       "expected", i);
  }
  PL_discard_foreign_frame(f);
  if ( status != CETTA_OK ) return out;

  out.inferences  = (uint64_t)values[0];
  out.cputime     = values[1];
  out.gc_count    = (uint64_t)values[2];
  out.gc_freed    = (uint64_t)values[3];
  out.gc_time     = values[4] / 1000.0;   /* the engine reports milliseconds */
  out.table_bytes = (uint64_t)values[5];
  return out;
}

cetta_stats cetta_stats_since(cetta_stats before, cetta_stats after)
{ cetta_stats spent;
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
bool cetta_def(cetta *runtime, cetta_op op)
{ fid_t f;
  term_t av;
  cetta_status status;
  char *published;
  const char *name = op.name;
  size_t arity = op.arity;
  cetta_fn fn = op.fn;
  void *user = op.user;
  const char *kind = cetta_effect_str(op.effect);
  cetta_op_entry_t *slot;

  if ( !name || !fn )
  { err_set(CETTA_MISUSE, "cetta_def needs a name and a function");
    return false;
  }
  if ( !kind )
  { err_set(CETTA_MISUSE,
            "an operation must name one of the five effect classes; "
            "%d is not one of them", (int)op.effect);
    return false;
  }
  if ( !(published = metta_name(name)) )
  { err_set(CETTA_NOMEM, "out of memory naming an operation");
    return false;
  }

  f = PL_open_foreign_frame();
  av = PL_new_term_refs(3);
  if ( !PL_put_atom_chars(av, published) ||
       !PL_put_int64(av + 1, (int64_t)arity) ||
       !PL_put_atom_chars(av + 2, kind) )
  { PL_discard_foreign_frame(f);
    free(published);
    err_set(CETTA_NOMEM, "out of memory registering an operation");
    return false;
  }
  status = call_bridge("metta_c_register_op", 3, av);
  PL_discard_foreign_frame(f);
  if ( status != CETTA_OK )
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
    cetta_op_entry_t *grown = realloc(runtime->ops, cap * sizeof(*grown));
    if ( !grown )
    { free(published);
      err_set(CETTA_NOMEM, "out of memory recording an operation");
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

bool cetta_undef(cetta *runtime, const char *name)
{ fid_t f;
  term_t av;
  cetta_status status;
  char *published;
  size_t i;

  if ( !name )
  { err_set(CETTA_MISUSE, "cetta_undef needs a name");
    return false;
  }
  if ( !(published = metta_name(name)) )
  { err_set(CETTA_NOMEM, "out of memory naming an operation");
    return false;
  }

  f = PL_open_foreign_frame();
  av = PL_new_term_refs(1);
  status = PL_put_atom_chars(av, published)
         ? call_bridge("metta_c_unregister_op", 1, av)
         : err_set(CETTA_NOMEM, "out of memory withdrawing an operation");
  PL_discard_foreign_frame(f);

  for (i = 0; i < runtime->nops; )
  { if ( strcmp(runtime->ops[i].name, published) == 0 )
    { free(runtime->ops[i].name);
      runtime->ops[i] = runtime->ops[--runtime->nops];
    } else i++;
  }
  free(published);
  return status == CETTA_OK;
}
