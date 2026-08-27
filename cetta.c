/* Purpose: the C half of the C binding. Boot SWI-Prolog in this process,
 *   consult the engine, and move values between C structures and engine terms
 *   directly, with no wire encoding in between.
 *
 * Assumes:
 *   - SWI-Prolog 10 with threads [source: PLVERSION 100113]
 *   - bindings/cetta/bridge.pl is loaded by bindings/cetta/decider.pl, which
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

#define CETTA_ERR_MAX 2048
static CETTA_TLS char g_err[CETTA_ERR_MAX];
static CETTA_TLS bool g_err_set = false;

static void err_clear(void)
{ g_err[0] = '\0';
  g_err_set = false;
}

static cetta_status_t err_set(cetta_status_t status, const char *fmt, ...)
{ va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_err, sizeof(g_err), fmt, ap);
  va_end(ap);
  g_err_set = true;
  return status;
}

const char *cetta_errmsg(void)
{ return g_err_set ? g_err : NULL;
}

const char *cetta_status_str(cetta_status_t status)
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

const char *cetta_kind_str(cetta_kind_t kind)
{ switch ( kind )
  { case CETTA_NONE:     return "None";
    case CETTA_SYMBOL:   return "Symbol";
    case CETTA_STRING:   return "String";
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

const char *cetta_effect_str(cetta_effect_t effect)
{ switch ( effect )
  { case CETTA_PURE_STRUCTURAL:            return "pureStructural";
    case CETTA_READ_ONLY_LOOKUP:           return "readOnlyLookup";
    case CETTA_NONDETERMINISTIC_READ_ONLY: return "nondeterministicReadOnly";
    case CETTA_WRITES_STATE:               return "writesState";
    case CETTA_ORACLE_IO:                  return "oracleIO";
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
  cetta_object_free_fn  release;
  cetta_op_fn           apply;
  void                 *user;
} cetta_box_t;

struct cetta_atom
{ CETTA_ATOMIC unsigned refs;
  cetta_kind_t          kind;
  union
  { struct { char *text; size_t len; }        t;  /* sym var str space bigint */
    int64_t                                   i;
    double                                    f;
    bool                                      b;
    struct { int64_t num, den; }               r;
    struct { cetta_atom_t **kids; size_t n; }  e;
    cetta_box_t                               *box;
  } u;
};

static cetta_atom_t *atom_alloc(cetta_kind_t kind)
{ cetta_atom_t *a = calloc(1, sizeof(*a));
  if ( !a )
  { err_set(CETTA_NOMEM, "out of memory allocating an atom");
    return NULL;
  }
  a->refs = 1;
  a->kind = kind;
  return a;
}

static cetta_atom_t *atom_text(cetta_kind_t kind, const char *text, size_t len)
{ cetta_atom_t *a;
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

cetta_atom_t *cetta_retain(const cetta_atom_t *atom)
{ cetta_atom_t *a = (cetta_atom_t *)atom;
  if ( a ) CETTA_INC(&a->refs);
  return a;
}

void cetta_release(const cetta_atom_t *atom)
{ cetta_atom_t *a = (cetta_atom_t *)atom;
  if ( !a ) return;
  if ( CETTA_DEC(&a->refs) != 1 ) return;

  switch ( a->kind )
  { case CETTA_SYMBOL:
    case CETTA_VARIABLE:
    case CETTA_STRING:
    case CETTA_SPACE:
    case CETTA_BIGINT:
    case CETTA_HANDLE:
      free(a->u.t.text);
      break;
    case CETTA_EXPR:
    { size_t i;
      for (i = 0; i < a->u.e.n; i++) cetta_release(a->u.e.kids[i]);
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

cetta_atom_t *cetta_sym(const char *name)
{ return name ? atom_text(CETTA_SYMBOL, name, strlen(name)) : NULL;
}

cetta_atom_t *cetta_var(const char *name)
{ return name ? atom_text(CETTA_VARIABLE, name, strlen(name)) : NULL;
}

cetta_atom_t *cetta_str(const char *text)
{ return text ? atom_text(CETTA_STRING, text, strlen(text)) : NULL;
}

cetta_atom_t *cetta_strn(const char *text, size_t length)
{ return atom_text(CETTA_STRING, text, length);
}

cetta_atom_t *cetta_int(int64_t value)
{ cetta_atom_t *a = atom_alloc(CETTA_INT);
  if ( a ) a->u.i = value;
  return a;
}

cetta_atom_t *cetta_float(double value)
{ cetta_atom_t *a = atom_alloc(CETTA_FLOAT);
  if ( a ) a->u.f = value;
  return a;
}

cetta_atom_t *cetta_bool(bool value)
{ cetta_atom_t *a = atom_alloc(CETTA_BOOL);
  if ( a ) a->u.b = value;
  return a;
}

cetta_atom_t *cetta_bigint(const char *decimal)
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

cetta_atom_t *cetta_rational(int64_t numerator, int64_t denominator)
{ cetta_atom_t *a;
  if ( denominator == 0 )
  { err_set(CETTA_MISUSE, "a rational cannot have a zero denominator");
    return NULL;
  }
  if ( !(a = atom_alloc(CETTA_RATIONAL)) ) return NULL;
  a->u.r.num = numerator;
  a->u.r.den = denominator;
  return a;
}

cetta_atom_t *cetta_space_ref(const char *name)
{ if ( !name || name[0] != '&' )
  { err_set(CETTA_MISUSE,
            "a space reference is written with a leading ampersand; %s is not",
            name ? name : "NULL");
    return NULL;
  }
  return atom_text(CETTA_SPACE, name, strlen(name));
}

cetta_atom_t *cetta_exprv(size_t count, cetta_atom_t **children)
{ cetta_atom_t *a;
  size_t i;
  bool bad = false;

  for (i = 0; i < count; i++)
    if ( !children[i] ) bad = true;

  if ( bad || !(a = atom_alloc(CETTA_EXPR)) )
  { /* Steal-on-success, release-on-failure: a NULL from an inner constructor
       must not leak the siblings that did succeed. */
    for (i = 0; i < count; i++) cetta_release(children[i]);
    if ( bad )
      err_set(CETTA_MISUSE,
              "an expression child was NULL; the constructor that made it "
              "failed and cetta_errmsg() said why at the time");
    return NULL;
  }
  if ( count > 0 )
  { if ( !(a->u.e.kids = malloc(count * sizeof(*a->u.e.kids))) )
    { free(a);
      for (i = 0; i < count; i++) cetta_release(children[i]);
      err_set(CETTA_NOMEM, "out of memory building an expression of %zu", count);
      return NULL;
    }
    memcpy(a->u.e.kids, children, count * sizeof(*a->u.e.kids));
  }
  a->u.e.n = count;
  return a;
}

cetta_atom_t *cetta_expr(size_t count, ...)
{ cetta_atom_t **kids = NULL;
  cetta_atom_t *out;
  va_list ap;
  size_t i;

  if ( count > 0 && !(kids = malloc(count * sizeof(*kids))) )
  { /* The arguments cannot be released without reading them, so read them. */
    va_start(ap, count);
    for (i = 0; i < count; i++) cetta_release(va_arg(ap, cetta_atom_t *));
    va_end(ap);
    err_set(CETTA_NOMEM, "out of memory building an expression of %zu", count);
    return NULL;
  }
  va_start(ap, count);
  for (i = 0; i < count; i++) kids[i] = va_arg(ap, cetta_atom_t *);
  va_end(ap);

  out = cetta_exprv(count, kids);
  free(kids);
  return out;
}

cetta_atom_t *cetta_unit(void)
{ return cetta_exprv(0, NULL);
}

cetta_kind_t cetta_kind(const cetta_atom_t *atom)
{ return atom ? atom->kind : CETTA_NONE;
}

const char *cetta_name(const cetta_atom_t *atom)
{ if ( !atom ) return NULL;
  switch ( atom->kind )
  { case CETTA_SYMBOL:
    case CETTA_VARIABLE:
    case CETTA_STRING:
    case CETTA_SPACE:
    case CETTA_BIGINT:
    case CETTA_HANDLE:
      return atom->u.t.text;
    default:
      return NULL;
  }
}

size_t cetta_name_len(const cetta_atom_t *atom)
{ return cetta_name(atom) ? atom->u.t.len : 0;
}

cetta_status_t cetta_int_value(const cetta_atom_t *atom, int64_t *out)
{ if ( !atom || atom->kind != CETTA_INT )
    return err_set(CETTA_MISUSE, "cetta_int_value wants a Number that fits "
                   "int64_t; this is %s",
                   atom ? cetta_kind_str(atom->kind) : "NULL");
  *out = atom->u.i;
  return CETTA_OK;
}

cetta_status_t cetta_float_value(const cetta_atom_t *atom, double *out)
{ if ( !atom || atom->kind != CETTA_FLOAT )
    return err_set(CETTA_MISUSE, "cetta_float_value wants a float; this is %s",
                   atom ? cetta_kind_str(atom->kind) : "NULL");
  *out = atom->u.f;
  return CETTA_OK;
}

cetta_status_t cetta_bool_value(const cetta_atom_t *atom, bool *out)
{ if ( !atom || atom->kind != CETTA_BOOL )
    return err_set(CETTA_MISUSE, "cetta_bool_value wants a Bool; this is %s",
                   atom ? cetta_kind_str(atom->kind) : "NULL");
  *out = atom->u.b;
  return CETTA_OK;
}

cetta_status_t cetta_rational_value(const cetta_atom_t *atom,
                                    int64_t *numerator, int64_t *denominator)
{ if ( !atom || atom->kind != CETTA_RATIONAL )
    return err_set(CETTA_MISUSE, "cetta_rational_value wants a Rational; "
                   "this is %s", atom ? cetta_kind_str(atom->kind) : "NULL");
  *numerator = atom->u.r.num;
  *denominator = atom->u.r.den;
  return CETTA_OK;
}

size_t cetta_len(const cetta_atom_t *atom)
{ return ( atom && atom->kind == CETTA_EXPR ) ? atom->u.e.n : 0;
}

const cetta_atom_t *cetta_child(const cetta_atom_t *atom, size_t index)
{ if ( !atom || atom->kind != CETTA_EXPR || index >= atom->u.e.n ) return NULL;
  return atom->u.e.kids[index];
}

bool cetta_eq(const cetta_atom_t *a, const cetta_atom_t *b)
{ size_t i;
  if ( a == b ) return true;
  if ( !a || !b || a->kind != b->kind ) return false;

  switch ( a->kind )
  { case CETTA_SYMBOL:
    case CETTA_VARIABLE:
    case CETTA_STRING:
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

void *cetta_object_value(const cetta_atom_t *atom)
{ return ( atom && atom->kind == CETTA_OBJECT ) ? atom->u.box->value : NULL;
}

const char *cetta_object_type(const cetta_atom_t *atom)
{ return ( atom && atom->kind == CETTA_OBJECT ) ? atom->u.box->type : NULL;
}

static cetta_atom_t *object_from_box(cetta_box_t *box)
{ cetta_atom_t *a = atom_alloc(CETTA_OBJECT);
  if ( !a )
  { box_release(box);
    return NULL;
  }
  a->u.box = box;
  return a;
}

static cetta_box_t *box_new(void *value, const char *type_name,
                            cetta_object_free_fn release,
                            cetta_op_fn apply, void *user)
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

cetta_atom_t *cetta_object(void *value, const char *type_name,
                           cetta_object_free_fn release)
{ cetta_box_t *box = box_new(value, type_name, release, NULL, NULL);
  return box ? object_from_box(box) : NULL;
}

cetta_atom_t *cetta_function(cetta_op_fn fn, void *user,
                             cetta_object_free_fn release)
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

typedef struct cetta_op
{ char           *name;
  size_t          arity;
  cetta_op_fn     fn;
  void           *user;
} cetta_op_entry_t;

struct cetta
{ bool              open;
  char             *path;
  bool              verbose;
  cetta_limits_t    limits;
  cetta_op_entry_t *ops;
  size_t            nops, cap_ops;
};

static struct cetta g_runtime;
static bool         g_open = false;

struct cetta_space
{ cetta_t *runtime;
  char    *name;
  bool     borrowed;   /* &self and &petta live with the runtime */
};

static cetta_space_t g_self, g_catalog;

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

static cetta_status_t call_bridge(const char *name, int arity, term_t av);

/* Whether this atom is a space, asked of the engine and of the term itself:
   no text conversion, and no list of names to rebuild per answer.
   petta_c_space_operand/1 is petta_space_operand/1, the test the engine's own
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
    space_operand = PL_predicate("petta_c_space_operand", 1, "user");
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

static cetta_atom_t *decode(term_t t, term_t names);

static cetta_atom_t *decode_list(term_t t, term_t names)
{ cetta_atom_t **kids = NULL;
  size_t n = 0, cap = 4;
  term_t head = PL_new_term_ref();
  term_t tail = PL_copy_term_ref(t);

  if ( !(kids = malloc(cap * sizeof(*kids))) )
  { err_set(CETTA_NOMEM, "out of memory decoding an expression");
    return NULL;
  }
  while ( PL_get_list(tail, head, tail) )
  { cetta_atom_t *kid = decode(head, names);
    if ( !kid )
    { size_t i;
      for (i = 0; i < n; i++) cetta_release(kids[i]);
      free(kids);
      return NULL;
    }
    if ( n == cap )
    { cetta_atom_t **grown = realloc(kids, (cap *= 2) * sizeof(*kids));
      if ( !grown )
      { size_t i;
        cetta_release(kid);
        for (i = 0; i < n; i++) cetta_release(kids[i]);
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
    for (i = 0; i < n; i++) cetta_release(kids[i]);
    free(kids);
    err_set(CETTA_UNSUPPORTED,
            "a partial list is not a MeTTa expression; the engine handed back "
            "a term with an unbound or non-list tail");
    return NULL;
  }
  { cetta_atom_t *out = cetta_exprv(n, kids);   /* steals the children */
    free(kids);
    return out;
  }
}

static cetta_atom_t *decode_number(term_t t)
{ int64_t i;
  double d;

  if ( PL_is_integer(t) )
  { if ( PL_get_int64(t, &i) ) return cetta_int(i);
    { size_t len;
      char *text = term_text(t, CVT_INTEGER, &len);
      cetta_atom_t *a;
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
  { if ( PL_get_float(t, &d) ) return cetta_float(d);
    err_set(CETTA_UNSUPPORTED, "a float the C boundary cannot read");
    return NULL;
  }
  if ( PL_is_rational(t) )
  { fid_t f = PL_open_foreign_frame();
    term_t av = PL_new_term_refs(3);
    cetta_atom_t *a = NULL;
    int64_t num, den;
    if ( PL_unify(av, t) &&
         call_bridge("petta_c_rational_parts", 3, av) == CETTA_OK &&
         PL_get_int64(av + 1, &num) && PL_get_int64(av + 2, &den) )
      a = cetta_rational(num, den);
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

static cetta_atom_t *decode(term_t t, term_t names)
{ if ( PL_is_variable(t) )
  { char *name = variable_name(names, t);
    cetta_atom_t *a;
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
    cetta_atom_t *a;
    if ( !text )
    { err_set(CETTA_NOMEM, "out of memory reading a string");
      return NULL;
    }
    a = atom_text(CETTA_STRING, text, len);
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
      cetta_atom_t *a;
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
    cetta_atom_t *a;

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

static bool encode(const cetta_atom_t *a, term_t out, encode_ctx *ctx)
{ if ( !a )
  { err_set(CETTA_MISUSE, "cannot encode a NULL atom");
    return false;
  }
  switch ( a->kind )
  { case CETTA_SYMBOL:
    case CETTA_SPACE:
      return PL_put_atom_nchars(out, a->u.t.len, a->u.t.text);
    case CETTA_STRING:
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

static bool put_atom(const cetta_atom_t *a, term_t out)
{ encode_ctx ctx = {0};
  bool ok = encode(a, out, &ctx);
  encode_ctx_free(&ctx);
  return ok;
}

/* The same, plus the Name-Var pairs the encode collected, which is what the
   engine's writer needs to print $x as $x rather than $_0. The list is built
   in the caller's frame and stays valid as long as `out` does. */
static bool put_atom_named(const cetta_atom_t *a, term_t out, term_t names)
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
  predicate_t p = PL_predicate("petta_c_error_text", 2, "user");
  qid_t q;

  if ( PL_unify(av, ball) &&
       (q = PL_open_query(NULL, PL_Q_CATCH_EXCEPTION, p, av)) )
  { if ( PL_next_solution(q) == TRUE )
    { char *text = term_text(av + 1, CVT_ATOM | CVT_STRING, NULL);
      if ( text )
      { snprintf(g_err, sizeof(g_err), "%s", text);
        g_err_set = true;
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
    g_err_set = true;
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
  predicate_t p = PL_predicate("petta_c_limit_ball", 3, "user");
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
static cetta_status_t call_bridge(const char *name, int arity, term_t av)
{ predicate_t p = PL_predicate(name, arity, "user");
  qid_t q = PL_open_query(NULL, PL_Q_CATCH_EXCEPTION, p, av);
  int rc;
  cetta_status_t status;

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
        cetta_status_t kind = CETTA_ERROR;
        if ( PL_recorded(saved, ball) )
        { render_ball(ball);
          if ( ball_is_limit(ball) ) kind = CETTA_LIMIT;
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
  err_clear();
  return status;
}

/* ================================================================== *
 * Foreign predicates the bridge calls back into
 * ================================================================== */

static foreign_t pl_cetta_present(void)
{ return TRUE;
}

struct cetta_call
{ cetta_t            *runtime;
  const cetta_atom_t **args;
  size_t              arity;
  cetta_atom_t       *result;
  bool                answered;
  char                error[CETTA_ERR_MAX];
  bool                failed;
};

size_t cetta_call_arity(const cetta_call_t *call)
{ return call->arity;
}

const cetta_atom_t *cetta_call_arg(const cetta_call_t *call, size_t index)
{ return index < call->arity ? call->args[index] : NULL;
}

cetta_t *cetta_call_runtime(const cetta_call_t *call)
{ return call->runtime;
}

cetta_status_t cetta_call_return(cetta_call_t *call, cetta_atom_t *atom)
{ if ( call->answered )
  { cetta_release(atom);
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

void cetta_call_error(cetta_call_t *call, const char *message)
{ snprintf(call->error, sizeof(call->error), "%s",
           message ? message : "the C function refused this application");
  call->failed = true;
}

/* Run one C function against decoded arguments and unify its answer. Shared by
   a named operation and an applied function value. */
static foreign_t run_call(const char *name, cetta_op_fn fn, void *user,
                          term_t args, term_t result)
{ struct cetta_call call;
  cetta_atom_t **decoded = NULL;
  size_t n = 0, cap = 4, i;
  term_t head = PL_new_term_ref();
  term_t tail = PL_copy_term_ref(args);
  cetta_status_t status;
  foreign_t rc = FALSE;

  memset(&call, 0, sizeof(call));
  if ( !(decoded = malloc(cap * sizeof(*decoded))) )
    return PL_resource_error("memory");

  while ( PL_get_list(tail, head, tail) )
  { cetta_atom_t *a = decode(head, 0);
    if ( !a )
    { rc = PL_permission_error("read", "argument", head);
      goto done;
    }
    if ( n == cap )
    { cetta_atom_t **grown = realloc(decoded, (cap *= 2) * sizeof(*decoded));
      if ( !grown ) { cetta_release(a); rc = PL_resource_error("memory"); goto done; }
      decoded = grown;
    }
    decoded[n++] = a;
  }

  call.runtime = &g_runtime;
  call.args = (const cetta_atom_t **)decoded;
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
  cetta_release(call.result);
  for (i = 0; i < n; i++) cetta_release(decoded[i]);
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
{ const char *env = getenv("PETTA_PATH");
  return strdup(env && *env ? env : CETTA_ENGINE_PATH);
}

cetta_status_t cetta_open(const cetta_config_t *config, cetta_t **out)
{ static char *argv[] = { (char *)"cetta", (char *)"-q",
                          (char *)"--no-signals", NULL };
  cetta_config_t defaults = {0};
  char *path;
  char *buf;
  size_t bufsz;

  err_clear();
  if ( !config ) config = &defaults;

  if ( g_open )
  { /* One runtime per process; PL_initialise sets up the process's single
       Prolog heap and there is no second one to hand out. */
    if ( config->path && strcmp(config->path, g_runtime.path) != 0 )
      return err_set(CETTA_MISUSE,
                     "the engine was booted from %s and cannot be reopened "
                     "from %s: this process holds one runtime",
                     g_runtime.path, config->path);
    *out = &g_runtime;
    return CETTA_OK;
  }

  path = config->path ? strdup(config->path) : default_path();
  if ( !path ) return err_set(CETTA_NOMEM, "out of memory recording the path");

  if ( !PL_is_initialised(NULL, NULL) && !PL_initialise(3, argv) )
  { free(path);
    return err_set(CETTA_ERROR, "SWI-Prolog would not initialise");
  }

  /* Registered BEFORE the consult, because engine/metta.pl globs
     bindings/ * /decider.pl while it loads and this seat's decider asks
     whether '$cetta_present'/0 exists. */
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
    return err_set(CETTA_NOMEM, "out of memory building the boot goals");
  }

  if ( config->stack_limit )
  { snprintf(buf, bufsz, "set_prolog_flag(stack_limit, %zu)",
             config->stack_limit);
    if ( !goal(buf) )
    { free(path); free(buf);
      return err_set(CETTA_ERROR, "the stack limit %zu was refused",
                     config->stack_limit);
    }
  }

  /* `backends` opts the engine into globbing backends/ * /decider.pl, and
     `silent` is how a host with no command line asks for quiet, because
     engine/filereader.pl reads argv at load time [C2]. */
  if ( !goal(config->verbose ? "set_prolog_flag(argv, [backends])"
                             : "set_prolog_flag(argv, [silent, backends])") )
  { free(path); free(buf);
    return err_set(CETTA_ERROR, "the engine refused its argv");
  }

  snprintf(buf, bufsz, "consult('%s/engine/metta.pl')", path);
  if ( !goal(buf) )
  { cetta_status_t refused =
      err_set(CETTA_ERROR,
              "the engine would not load from %s; set config.path or "
              "PETTA_PATH to the tree holding engine/, lib/ and "
              "backends/", path);
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
  g_catalog.name = (char *)"&petta";
  g_catalog.borrowed = true;

  cetta_set_verbose(&g_runtime, config->verbose);
  *out = &g_runtime;
  return CETTA_OK;
}

void cetta_close(cetta_t *runtime)
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
  PL_cleanup(0);
}

bool cetta_set_verbose(cetta_t *runtime, bool verbose)
{ bool was = runtime->verbose;
  fid_t f = PL_open_foreign_frame();
  term_t av = PL_new_term_refs(1);
  if ( PL_put_atom_chars(av, verbose ? "false" : "true") &&
       call_bridge("petta_c_set_silent", 1, av) == CETTA_OK )
    runtime->verbose = verbose;
  PL_discard_foreign_frame(f);
  return was;
}

cetta_status_t cetta_thread_attach(cetta_t *runtime)
{ (void)runtime;
  err_clear();
  if ( PL_thread_attach_engine(NULL) < 0 )
    return err_set(CETTA_ERROR, "this thread could not attach a Prolog engine");
  return CETTA_OK;
}

void cetta_thread_detach(cetta_t *runtime)
{ (void)runtime;
  PL_thread_destroy_engine();
}

/* ================================================================== *
 * Text
 * ================================================================== */

cetta_status_t cetta_parse(cetta_t *runtime, const char *source,
                           cetta_atom_t **out)
{ fid_t f;
  term_t av;
  cetta_status_t status;

  (void)runtime;
  err_clear();
  *out = NULL;
  if ( !source ) return err_set(CETTA_MISUSE, "cetta_parse needs source text");

  f = PL_open_foreign_frame();
  av = PL_new_term_refs(3);
  if ( !PL_put_string_chars(av, source) )
  { PL_discard_foreign_frame(f);
    return err_set(CETTA_NOMEM, "out of memory holding the source");
  }
  status = call_bridge("petta_c_read", 3, av);
  if ( status == CETTA_OK )
  { *out = decode(av + 1, av + 2);
    if ( !*out ) status = CETTA_UNSUPPORTED;
  }
  PL_discard_foreign_frame(f);
  return status;
}

char *cetta_show(cetta_t *runtime, const cetta_atom_t *atom)
{ fid_t f;
  term_t av;
  char *text = NULL;

  (void)runtime;
  err_clear();
  f = PL_open_foreign_frame();
  av = PL_new_term_refs(3);
  if ( put_atom_named(atom, av, av + 1) &&
       call_bridge("petta_c_show", 3, av) == CETTA_OK )
    text = term_text(av + 2, CVT_ATOM | CVT_STRING, NULL);
  PL_discard_foreign_frame(f);
  return text;
}

void cetta_free(void *pointer)
{ free(pointer);
}

/* ================================================================== *
 * Spaces
 * ================================================================== */

cetta_space_t *cetta_self(cetta_t *runtime)
{ (void)runtime;
  return &g_self;
}

cetta_space_t *cetta_catalog(cetta_t *runtime)
{ (void)runtime;
  return &g_catalog;
}

const char *cetta_space_name(const cetta_space_t *space)
{ return space->name;
}

cetta_status_t cetta_space_open(cetta_t *runtime, const char *name,
                                cetta_space_t **out)
{ cetta_space_t *s;

  err_clear();
  if ( !name || name[0] != '&' )
    return err_set(CETTA_MISUSE,
                   "a space is named with a leading ampersand; %s is not",
                   name ? name : "NULL");
  if ( strcmp(name, "&self") == 0 )  { *out = &g_self;    return CETTA_OK; }
  if ( strcmp(name, "&petta") == 0 ) { *out = &g_catalog; return CETTA_OK; }

  if ( !(s = calloc(1, sizeof(*s))) )
    return err_set(CETTA_NOMEM, "out of memory opening a space");
  if ( !(s->name = strdup(name)) )
  { free(s);
    return err_set(CETTA_NOMEM, "out of memory naming a space");
  }
  s->runtime = runtime;
  *out = s;
  return CETTA_OK;
}

void cetta_space_free(cetta_space_t *space)
{ if ( !space || space->borrowed ) return;
  free(space->name);
  free(space);
}

static cetta_status_t space_call(const char *pred, cetta_space_t *space,
                                 const cetta_atom_t *atom, int arity,
                                 term_t *avp, fid_t *fp)
{ fid_t f = PL_open_foreign_frame();
  term_t av = PL_new_term_refs(arity);
  cetta_status_t status;

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

cetta_status_t cetta_add(cetta_space_t *space, const cetta_atom_t *atom)
{ err_clear();
  return space_call("petta_c_add", space, atom, 2, NULL, NULL);
}

cetta_status_t cetta_remove(cetta_space_t *space, const cetta_atom_t *atom,
                            bool *removed)
{ fid_t f;
  term_t av;
  cetta_status_t status;

  err_clear();
  status = space_call("petta_c_remove", space, atom, 3, &av, &f);
  if ( status == CETTA_OK && removed )
  { char *text = term_text(av + 2, CVT_ATOM, NULL);
    *removed = text && strcmp(text, "true") == 0;
    free(text);
  }
  PL_discard_foreign_frame(f);
  return status;
}

cetta_status_t cetta_space_count(cetta_space_t *space, size_t *out)
{ fid_t f;
  term_t av;
  cetta_status_t status;
  int64_t n;

  err_clear();
  status = space_call("petta_c_count", space, NULL, 2, &av, &f);
  if ( status == CETTA_OK )
  { if ( PL_get_int64(av + 1, &n) ) *out = (size_t)n;
    else status = err_set(CETTA_ERROR, "the space did not answer a count");
  }
  PL_discard_foreign_frame(f);
  return status;
}

cetta_status_t cetta_space_clear(cetta_space_t *space)
{ err_clear();
  return space_call("petta_c_clear", space, NULL, 1, NULL, NULL);
}

/* ================================================================== *
 * Answers
 * ================================================================== */

/* A cursor is one of two things wearing one face: a table of answers a run
   already computed, or an engine suspended between them. */
struct cetta_answers
{ cetta_t      *runtime;
  bool          lazy;
  int64_t       cursor_id;      /* lazy: the bridge's engine id     */
  cetta_atom_t **rows;          /* eager: every answer, in order    */
  char        **texts;
  size_t       *groups;
  size_t        n, at;
  bool          started, done;
  cetta_atom_t *current;
  char         *current_text;
  size_t        current_group;
};

static cetta_answers_t *answers_alloc(cetta_t *runtime)
{ cetta_answers_t *a = calloc(1, sizeof(*a));
  if ( !a ) err_set(CETTA_NOMEM, "out of memory opening a cursor");
  else a->runtime = runtime;
  return a;
}

/* Read the engine's Groups term: a list of groups, each a list of answers. */
static cetta_status_t collect_groups(term_t groups, cetta_answers_t *out)
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
      cetta_atom_t *atom = NULL;
      char *text = NULL;

      if ( PL_unify(av, answer) &&
           call_bridge("petta_c_answer_parts", 4, av) == CETTA_OK )
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

static cetta_status_t run_or_load(cetta_t *runtime, const char *pred,
                                  const char *argument, const char *space,
                                  cetta_answers_t **out)
{ fid_t f;
  term_t av;
  cetta_answers_t *answers;
  cetta_status_t status;

  err_clear();
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

cetta_status_t cetta_run(cetta_t *runtime, const char *source,
                         cetta_answers_t **out)
{ return run_or_load(runtime, "petta_c_run", source, "&self", out);
}

cetta_status_t cetta_load(cetta_t *runtime, const char *path,
                          cetta_answers_t **out)
{ return run_or_load(runtime, "petta_c_load", path, "&self", out);
}

static cetta_status_t open_cursor(cetta_space_t *space, const char *pred,
                                  const cetta_atom_t *atom,
                                  cetta_answers_t **out)
{ fid_t f;
  term_t av;
  cetta_answers_t *answers;
  cetta_status_t status;
  int64_t id;

  err_clear();
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

cetta_status_t cetta_eval(cetta_space_t *space, const cetta_atom_t *goal,
                          cetta_answers_t **out)
{ return open_cursor(space, "petta_c_open_eval", goal, out);
}

cetta_status_t cetta_match(cetta_space_t *space, const cetta_atom_t *pattern,
                           cetta_answers_t **out)
{ return open_cursor(space, "petta_c_open_match", pattern, out);
}

cetta_status_t cetta_space_atoms(cetta_space_t *space, cetta_answers_t **out)
{ cetta_atom_t *any = cetta_var("_");
  cetta_status_t status;
  if ( !any ) return CETTA_NOMEM;
  status = open_cursor(space, "petta_c_open_match", any, out);
  cetta_release(any);
  return status;
}

static void clear_current(cetta_answers_t *answers)
{ if ( answers->lazy )
  { cetta_release(answers->current);
    free(answers->current_text);
  }
  answers->current = NULL;
  answers->current_text = NULL;
}

cetta_status_t cetta_answers_step(cetta_answers_t *answers)
{ fid_t f;
  term_t av;
  cetta_status_t status;
  term_t head, tail;

  err_clear();
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
  status = call_bridge("petta_c_next", 3, av);
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
         call_bridge("petta_c_answer_parts", 4, parts) == CETTA_OK )
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

const cetta_atom_t *cetta_answers_atom(const cetta_answers_t *answers)
{ return answers ? answers->current : NULL;
}

const char *cetta_answers_text(const cetta_answers_t *answers)
{ return answers ? answers->current_text : NULL;
}

size_t cetta_answers_group(const cetta_answers_t *answers)
{ return answers ? answers->current_group : 0;
}

void cetta_answers_free(cetta_answers_t *answers)
{ size_t i;
  if ( !answers ) return;

  if ( answers->lazy )
  { fid_t f = PL_open_foreign_frame();
    term_t av = PL_new_term_refs(1);
    if ( PL_put_int64(av, answers->cursor_id) )
      call_bridge("petta_c_close", 1, av);
    PL_discard_foreign_frame(f);
    clear_current(answers);
  } else
  { for (i = 0; i < answers->n; i++)
    { cetta_release(answers->rows[i]);
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

cetta_status_t cetta_set_limits(cetta_t *runtime, const cetta_limits_t *limits)
{ static const cetta_limits_t none = {0, 0, 0};

  err_clear();
  if ( !runtime ) return err_set(CETTA_MISUSE, "cetta_set_limits needs a runtime");
  runtime->limits = limits ? *limits : none;

  if ( runtime->limits.stack_bytes )
  { char goal_text[96];
    snprintf(goal_text, sizeof(goal_text),
             "set_prolog_flag(stack_limit, %zu)", runtime->limits.stack_bytes);
    if ( !goal(goal_text) )
      return err_set(CETTA_ERROR, "the stack ceiling %zu was refused",
                     runtime->limits.stack_bytes);
  }
  return CETTA_OK;
}

void cetta_get_limits(const cetta_t *runtime, cetta_limits_t *out)
{ *out = runtime->limits;
}

/* ================================================================== *
 * Measuring
 * ================================================================== */

cetta_status_t cetta_stats(cetta_t *runtime, cetta_stats_t *out)
{ fid_t f;
  term_t av, head, tail;
  cetta_status_t status;
  double values[6] = {0, 0, 0, 0, 0, 0};
  size_t i = 0;

  (void)runtime;
  err_clear();
  if ( !out ) return err_set(CETTA_MISUSE, "cetta_stats needs somewhere to put them");

  f = PL_open_foreign_frame();
  av = PL_new_term_refs(1);
  status = call_bridge("petta_c_stats", 1, av);
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
  if ( status != CETTA_OK ) return status;

  out->inferences  = (uint64_t)values[0];
  out->cputime     = values[1];
  out->gc_count    = (uint64_t)values[2];
  out->gc_freed    = (uint64_t)values[3];
  out->gc_time     = values[4] / 1000.0;   /* the engine reports milliseconds */
  out->table_bytes = (uint64_t)values[5];
  return CETTA_OK;
}

void cetta_stats_delta(const cetta_stats_t *before, const cetta_stats_t *after,
                       cetta_stats_t *out)
{ out->inferences  = after->inferences  - before->inferences;
  out->cputime     = after->cputime     - before->cputime;
  out->gc_count    = after->gc_count    - before->gc_count;
  out->gc_freed    = after->gc_freed    - before->gc_freed;
  out->gc_time     = after->gc_time     - before->gc_time;
  out->table_bytes = after->table_bytes - before->table_bytes;
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

cetta_status_t cetta_op(cetta_t *runtime, const char *name, size_t arity,
                        cetta_effect_t effect, cetta_op_fn fn, void *user)
{ fid_t f;
  term_t av;
  cetta_status_t status;
  char *published;
  const char *kind = cetta_effect_str(effect);
  cetta_op_entry_t *slot;

  err_clear();
  if ( !name || !fn )
    return err_set(CETTA_MISUSE, "cetta_op needs a name and a function");
  if ( !kind )
    return err_set(CETTA_MISUSE,
                   "an operation must name one of the five effect classes; "
                   "%d is not one of them", (int)effect);
  if ( !(published = metta_name(name)) )
    return err_set(CETTA_NOMEM, "out of memory naming an operation");

  f = PL_open_foreign_frame();
  av = PL_new_term_refs(3);
  if ( !PL_put_atom_chars(av, published) ||
       !PL_put_int64(av + 1, (int64_t)arity) ||
       !PL_put_atom_chars(av + 2, kind) )
  { PL_discard_foreign_frame(f);
    free(published);
    return err_set(CETTA_NOMEM, "out of memory registering an operation");
  }
  status = call_bridge("petta_c_register_op", 3, av);
  PL_discard_foreign_frame(f);
  if ( status != CETTA_OK )
  { free(published);
    return status;
  }

  if ( (slot = find_op(published, arity)) )
  { slot->fn = fn;
    slot->user = user;
    free(published);
    return CETTA_OK;
  }
  if ( runtime->nops == runtime->cap_ops )
  { size_t cap = runtime->cap_ops ? runtime->cap_ops * 2 : 8;
    cetta_op_entry_t *grown = realloc(runtime->ops, cap * sizeof(*grown));
    if ( !grown )
    { free(published);
      return err_set(CETTA_NOMEM, "out of memory recording an operation");
    }
    runtime->ops = grown;
    runtime->cap_ops = cap;
  }
  slot = &runtime->ops[runtime->nops++];
  slot->name = published;
  slot->arity = arity;
  slot->fn = fn;
  slot->user = user;
  return CETTA_OK;
}

cetta_status_t cetta_op_remove(cetta_t *runtime, const char *name)
{ fid_t f;
  term_t av;
  cetta_status_t status;
  char *published;
  size_t i;

  err_clear();
  if ( !name ) return err_set(CETTA_MISUSE, "cetta_op_remove needs a name");
  if ( !(published = metta_name(name)) )
    return err_set(CETTA_NOMEM, "out of memory naming an operation");

  f = PL_open_foreign_frame();
  av = PL_new_term_refs(1);
  status = PL_put_atom_chars(av, published)
         ? call_bridge("petta_c_unregister_op", 1, av)
         : err_set(CETTA_NOMEM, "out of memory withdrawing an operation");
  PL_discard_foreign_frame(f);

  for (i = 0; i < runtime->nops; )
  { if ( strcmp(runtime->ops[i].name, published) == 0 )
    { free(runtime->ops[i].name);
      runtime->ops[i] = runtime->ops[--runtime->nops];
    } else i++;
  }
  free(published);
  return status;
}
