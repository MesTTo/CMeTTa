/* Purpose: drive the PeTTa engine from a C program. Boot it, build and read
 *   MeTTa terms as C values, run programs, pull answers one at a time, and
 *   publish C functions the language can call.
 *
 * Assumes:
 *   - SWI-Prolog 10 with its development headers, threads enabled
 *     [source: /usr/lib/swi-prolog/include/SWI-Prolog.h, PLVERSION 100113]
 *   - the engine tree (engine/, lib/, backends/) is reachable, either at the
 *     path given to cetta_open() or at $PETTA_PATH
 *   - the process has not already called PL_initialise(); see cetta_open()
 *
 * Guarantees:
 *   - every function that can fail says so in its return value; none of them
 *     print, exit, or longjmp, and no Prolog exception crosses this header
 *   - an atom is immutable and refcounted, so a term built once may be run
 *     many times and shared between threads without copying
 *   - building and reading atoms starts no engine: cetta_sym(), cetta_expr()
 *     and the accessors are pure C on C memory, the same split the Python
 *     binding guarantees for its own term builders
 *   - cetta_eval() computes one answer per cetta_answers_step(), so a caller
 *     that stops pulling leaves the rest of an infinite stream uncomputed
 *
 * Owns resources: one Prolog runtime per process, released by cetta_close();
 *   one engine per open cetta_answers_t from cetta_eval(), released by
 *   cetta_answers_free(); one malloc'ed block per atom, released when its
 *   last reference goes.
 *
 * Decides:
 *   - THE OWNERSHIP LAW, and it is carried by C's own type system: a function
 *     taking `const cetta_atom_t *` BORROWS it and the caller still owns it;
 *     a function taking `cetta_atom_t *` (non-const) STEALS it and the caller
 *     must not release it afterwards. Every constructor returns a reference
 *     the caller owns. Every accessor returns a borrowed pointer valid only
 *     while its parent lives. There are no other rules to remember.
 *   - a MeTTa Number splits into CETTA_INT and CETTA_FLOAT here, because C
 *     has two types where the wire codec has one tag, and MeTTa tells 2 from
 *     2.0 apart. Values outside int64 and rationals get their own kinds
 *     rather than being rounded into one that fits; see cetta_kind_t.
 *   - the last error is thread-local and read with cetta_errmsg(), the shape
 *     dlerror() and strerror() already established for C, so a function that
 *     returns a pointer can fail without an out-parameter for the reason.
 *
 * Fails when: the caller wants two independent runtimes in one process
 *   (PL_initialise is process-wide), or wants to hold an engine term rather
 *   than a materialised copy. Both are named in ai-cetta-c-constraints.md.
 *
 * Guarded by: nothing, deliberately, and here is exactly what that means.
 *   An atom is immutable after construction and its refcount is atomic, so
 *   building, sharing and releasing atoms is safe from any thread. The error
 *   text is thread-local. What is NOT guarded is the operation table:
 *   cetta_op() and cetta_op_remove() mutate it without a lock, so publish
 *   every operation before the threads that evaluate start, the same
 *   restriction sqlite3_create_function() carries. Evaluation itself is the
 *   engine's business and each thread needs its own engine; see
 *   cetta_thread_attach().
 */

#ifndef CETTA_H
#define CETTA_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CETTA_API
#define CETTA_API extern
#endif

/* ------------------------------------------------------------------ *
 * Status
 * ------------------------------------------------------------------ */

/* Every fallible call answers one of these. CETTA_ROW and CETTA_DONE are
   answers rather than problems: they are how a cursor reports progress, the
   split sqlite3_step() established. */
typedef enum cetta_status {
  CETTA_OK = 0,          /* the call did what it said */
  CETTA_ROW = 1,         /* a cursor produced an answer */
  CETTA_DONE = 2,        /* a cursor is exhausted */
  CETTA_FAIL = 3,        /* the engine had no answer; not an error */
  CETTA_ERROR = 4,       /* the engine raised; cetta_errmsg() has its words */
  CETTA_NOMEM = 5,       /* allocation failed */
  CETTA_MISUSE = 6,      /* this library's contract was broken */
  CETTA_UNSUPPORTED = 7, /* a real value C has no type for; refused by name */
  CETTA_LIMIT = 8        /* a bound stopped it; you did this, it did not break */
} cetta_status_t;

/* A stable English name for a status, for a caller's own diagnostics. */
CETTA_API const char *cetta_status_str(cetta_status_t status);

/* The last failure on THIS thread, or NULL if the last call succeeded. The
   returned text is owned by the library and is overwritten by the next
   failing call on this thread. */
CETTA_API const char *cetta_errmsg(void);

/* The binding's version, matching the engine tree it was built against. */
CETTA_API const char *cetta_version(void);

/* ------------------------------------------------------------------ *
 * Atoms
 * ------------------------------------------------------------------ */

/* An immutable MeTTa term. Refcounted; see THE OWNERSHIP LAW above. */
typedef struct cetta_atom cetta_atom_t;

/* What an atom is. The nine wire tags of CODEC.md, with the one tag C splits:
   `n` becomes CETTA_INT, CETTA_FLOAT, CETTA_BIGINT and CETTA_RATIONAL,
   because C has distinct types for the first two and no type at all for the
   last two, and silently rounding either is the failure this split exists to
   prevent. */
typedef enum cetta_kind {
  CETTA_NONE = -1,/* not an atom at all; what cetta_kind(NULL) answers  */
  CETTA_SYMBOL,   /* `s`: a name that denotes itself                    */
  CETTA_STRING,   /* `g`: a grounded value carried as text              */
  CETTA_INT,      /* `n`: an exact integer that fits int64_t            */
  CETTA_FLOAT,    /* `n`: a float                                       */
  CETTA_BIGINT,   /* `n`: an exact integer too wide for int64_t         */
  CETTA_RATIONAL, /* `n`: an exact ratio, read with cetta_rational()    */
  CETTA_BOOL,     /* `b`: True or False, which are not symbols          */
  CETTA_VARIABLE, /* `v`: a variable, its name an identity in its term  */
  CETTA_EXPR,     /* `e`: an expression; the empty one is unit          */
  CETTA_SPACE,    /* `p`: an executable space reference, by name        */
  CETTA_OBJECT,   /* `o`: a live C value crossing by reference          */
  CETTA_HANDLE    /* `h`: a native engine value held by reference       */
} cetta_kind_t;

CETTA_API const char *cetta_kind_str(cetta_kind_t kind);

/* --- building. None of these start the engine. --- */

/* A symbol. `cetta_sym("foo")` is the name foo, which is NOT the string
   "foo"; that is cetta_str(). */
CETTA_API cetta_atom_t *cetta_sym(const char *name);

/* A variable. The name is an identity within one term: two variables of one
   name in one expression are one variable. "_" is fresh at every occurrence. */
CETTA_API cetta_atom_t *cetta_var(const char *name);

/* A string, as MeTTa's grounded text. */
CETTA_API cetta_atom_t *cetta_str(const char *text);

/* Text that is not NUL-terminated, or that contains NULs. */
CETTA_API cetta_atom_t *cetta_strn(const char *text, size_t length);

CETTA_API cetta_atom_t *cetta_int(int64_t value);
CETTA_API cetta_atom_t *cetta_float(double value);
CETTA_API cetta_atom_t *cetta_bool(bool value);

/* An exact integer wider than int64_t, written as decimal digits with an
   optional leading '-'. Returns NULL and sets cetta_errmsg() on any other
   spelling. */
CETTA_API cetta_atom_t *cetta_bigint(const char *decimal);

/* An exact ratio. A zero denominator is refused. */
CETTA_API cetta_atom_t *cetta_rational(int64_t numerator, int64_t denominator);

/* A space reference by its portable engine name, which must begin with '&'. */
CETTA_API cetta_atom_t *cetta_space_ref(const char *name);

/* An expression of `count` children, each STOLEN. Building nests without
   leaking:
       cetta_expr(3, cetta_sym("+"), cetta_int(1), cetta_int(2))
   If any argument is NULL the whole call fails, releases the arguments it was
   given, and returns NULL, so a failed inner constructor cannot leak through
   an outer one. */
CETTA_API cetta_atom_t *cetta_expr(size_t count, ...);

/* The same from an array. The children are stolen; the array is not. */
CETTA_API cetta_atom_t *cetta_exprv(size_t count, cetta_atom_t **children);

/* The empty expression, which is unit. Not a missing value, and not "". */
CETTA_API cetta_atom_t *cetta_unit(void);

/* --- lifetime --- */

/* Take a reference. Returns its argument so it composes inline. NULL-safe. */
CETTA_API cetta_atom_t *cetta_retain(const cetta_atom_t *atom);

/* Drop a reference. NULL-safe. */
CETTA_API void cetta_release(const cetta_atom_t *atom);

/* --- reading --- */

/* What an atom is, and CETTA_NONE for NULL, so the result of cetta_child() or
   cetta_answers_atom() can be asked directly without a guard first. */
CETTA_API cetta_kind_t cetta_kind(const cetta_atom_t *atom);

/* The name of a CETTA_SYMBOL, CETTA_VARIABLE or CETTA_SPACE; the text of a
   CETTA_STRING; the exact decimal digits of a CETTA_BIGINT. NULL for every
   other kind. Borrowed: valid while the atom lives. */
CETTA_API const char *cetta_name(const cetta_atom_t *atom);

/* The byte length behind cetta_name(), for text that may contain NULs. */
CETTA_API size_t cetta_name_len(const cetta_atom_t *atom);

/* CETTA_OK, or CETTA_MISUSE when the atom is another kind. The out-parameter
   is untouched on failure. */
CETTA_API cetta_status_t cetta_int_value(const cetta_atom_t *atom, int64_t *out);
CETTA_API cetta_status_t cetta_float_value(const cetta_atom_t *atom, double *out);
CETTA_API cetta_status_t cetta_bool_value(const cetta_atom_t *atom, bool *out);
CETTA_API cetta_status_t cetta_rational_value(const cetta_atom_t *atom,
                                              int64_t *numerator,
                                              int64_t *denominator);

/* The child count of a CETTA_EXPR; 0 for every other kind. */
CETTA_API size_t cetta_len(const cetta_atom_t *atom);

/* Child `index` of a CETTA_EXPR, BORROWED: valid while the parent lives, and
   not to be released. NULL when the atom is not an expression or the index is
   past its end. Retain it to keep it longer. */
CETTA_API const cetta_atom_t *cetta_child(const cetta_atom_t *atom, size_t index);

/* Structural equality, the same question MeTTa's == asks of ground terms.
   Two variables are equal when their names are. */
CETTA_API bool cetta_eq(const cetta_atom_t *a, const cetta_atom_t *b);

/* The live C pointer behind a CETTA_OBJECT, or NULL. */
CETTA_API void *cetta_object_value(const cetta_atom_t *atom);

/* ------------------------------------------------------------------ *
 * The runtime
 * ------------------------------------------------------------------ */

typedef struct cetta cetta_t;

typedef struct cetta_config {
  /* The engine tree holding engine/, lib/ and backends/. NULL takes
     $PETTA_PATH, then the tree this library was built beside. */
  const char *path;
  /* Prolog stack limit in bytes. 0 takes the engine's own default. */
  size_t stack_limit;
  /* Let the engine print each form's compiled goal, as the CLI does. */
  bool verbose;
} cetta_config_t;

/* Boot the engine. `config` may be NULL for every default.

   One runtime per process: PL_initialise() sets up the process's single
   Prolog heap, so a second cetta_open() with a matching configuration hands
   back the same runtime and one with a different path answers CETTA_MISUSE
   rather than pretending. */
CETTA_API cetta_status_t cetta_open(const cetta_config_t *config, cetta_t **out);

/* Shut the runtime down and release everything it owns. Atoms outlive it:
   they are C memory and stay valid until their own references go. */
CETTA_API void cetta_close(cetta_t *runtime);

/* Whether the engine prints compiled forms. Returns the previous setting. */
CETTA_API bool cetta_set_verbose(cetta_t *runtime, bool verbose);

/* A thread other than the one that called cetta_open() must attach before it
   touches the engine, and detach before it exits. Atom construction and
   reading need neither. */
CETTA_API cetta_status_t cetta_thread_attach(cetta_t *runtime);
CETTA_API void cetta_thread_detach(cetta_t *runtime);

/* --- text, through the engine's own reader and writer --- */

/* Read one MeTTa form. The engine's reader is the only reader; this binding
   grows no second one. */
CETTA_API cetta_status_t cetta_parse(cetta_t *runtime, const char *source,
                                     cetta_atom_t **out);

/* Write an atom the way the engine writes it. The result is a NUL-terminated
   string the caller frees with cetta_free(). */
CETTA_API char *cetta_show(cetta_t *runtime, const cetta_atom_t *atom);

/* Free a string this library returned. */
CETTA_API void cetta_free(void *pointer);

/* ------------------------------------------------------------------ *
 * Spaces
 * ------------------------------------------------------------------ */

typedef struct cetta_space cetta_space_t;

/* The runtime's own &self. Borrowed: it lives as long as the runtime and must
   not be freed. */
CETTA_API cetta_space_t *cetta_self(cetta_t *runtime);

/* The queryable reflection space, &petta. Borrowed. */
CETTA_API cetta_space_t *cetta_catalog(cetta_t *runtime);

/* Create or open a space by name. Names begin with '&'. */
CETTA_API cetta_status_t cetta_space_open(cetta_t *runtime, const char *name,
                                          cetta_space_t **out);

CETTA_API void cetta_space_free(cetta_space_t *space);
CETTA_API const char *cetta_space_name(const cetta_space_t *space);

/* Add one atom. The atom is borrowed. */
CETTA_API cetta_status_t cetta_add(cetta_space_t *space, const cetta_atom_t *atom);

/* Remove one exact atom. `*removed` says whether it was there; pass NULL if
   the answer does not matter. */
CETTA_API cetta_status_t cetta_remove(cetta_space_t *space,
                                      const cetta_atom_t *atom, bool *removed);

/* How many atoms the space holds. */
CETTA_API cetta_status_t cetta_space_count(cetta_space_t *space, size_t *out);

/* Empty the space. */
CETTA_API cetta_status_t cetta_space_clear(cetta_space_t *space);

/* ------------------------------------------------------------------ *
 * Answers
 * ------------------------------------------------------------------ */

/* A cursor over answers. Stepped, not drained: the shape sqlite3_step() gave
   C, and the reason an infinite MeTTa stream is usable from here. */
typedef struct cetta_answers cetta_answers_t;

/* Run MeTTa source. Every `!` form contributes a group of answers, and the
   groups arrive in source order; cetta_answers_group() says which group the
   current answer belongs to.

   Eager: the engine's run door computes the whole program before the first
   step, because that is what running a program means. cetta_eval() is the
   lazy door. */
CETTA_API cetta_status_t cetta_run(cetta_t *runtime, const char *source,
                                   cetta_answers_t **out);

/* Load a file through the same door `import!` uses, so a reload replaces the
   first load's definitions rather than doubling them. */
CETTA_API cetta_status_t cetta_load(cetta_t *runtime, const char *path,
                                    cetta_answers_t **out);

/* Evaluate one atom in a space, LAZILY: each cetta_answers_step() computes at
   most one answer, and abandoning the cursor leaves the rest uncomputed. */
CETTA_API cetta_status_t cetta_eval(cetta_space_t *space,
                                    const cetta_atom_t *goal,
                                    cetta_answers_t **out);

/* Every atom in the space matching a pattern, lazily. */
CETTA_API cetta_status_t cetta_match(cetta_space_t *space,
                                     const cetta_atom_t *pattern,
                                     cetta_answers_t **out);

/* Every atom in the space, lazily. */
CETTA_API cetta_status_t cetta_space_atoms(cetta_space_t *space,
                                           cetta_answers_t **out);

/* Every door above sets *out to NULL before it can fail, so a caller reusing
   one variable never frees a stale cursor and a failed call leaves nothing to
   release. cetta_answers_free(NULL) is a no-op. */

/* Advance. CETTA_ROW when an answer is ready, CETTA_DONE at the end,
   CETTA_ERROR when the engine raised. Stepping past CETTA_DONE keeps
   answering CETTA_DONE. */
CETTA_API cetta_status_t cetta_answers_step(cetta_answers_t *answers);

/* The current answer, BORROWED: it belongs to the cursor and is released by
   the next step or by cetta_answers_free(). Retain it to keep it. NULL before
   the first CETTA_ROW. */
CETTA_API const cetta_atom_t *cetta_answers_atom(const cetta_answers_t *answers);

/* The engine's own rendering of the current answer. Borrowed on the same
   terms. This is presentation, and it can show a value cetta_answers_atom()
   refuses: a host-only value or a non-finite float renders here rather than
   failing the whole answer. */
CETTA_API const char *cetta_answers_text(const cetta_answers_t *answers);

/* Which `!` form produced the current answer, counting from 0. Always 0 for
   the lazy doors, which evaluate one goal. */
CETTA_API size_t cetta_answers_group(const cetta_answers_t *answers);

/* Release the cursor and, for a lazy one, the engine behind it. Idempotent
   against a cursor already exhausted. NULL-safe. */
CETTA_API void cetta_answers_free(cetta_answers_t *answers);

/* ------------------------------------------------------------------ *
 * Bounding an evaluation
 * ------------------------------------------------------------------ */

/* What an evaluation may spend. Zero on a field means no bound there.

   A bound that stops an evaluation stops it MID-WAY, so writes it already
   made stand. That is the honest semantics of every timeout, and a caller who
   needs all-or-nothing wraps the work in a transaction rather than expecting a
   bound to unwind it. */
typedef struct cetta_limits {
  double   seconds;      /* wall seconds one call, or one step, may take    */
  uint64_t inferences;   /* engine steps it may spend                       */
  size_t   stack_bytes;  /* SWI's combined stack ceiling, which a runaway
                            recursion hits; NOT MeTTa's reduction depth,
                            which is the max-stack-depth pragma in the
                            program text                                    */
} cetta_limits_t;

/* Bounds for every later call on this runtime. Passing NULL clears them.

   On a lazy cursor the INFERENCE bound is a CUMULATIVE budget for the whole
   cursor: the engine's own counter is read before and after every step and the
   deltas are added up, so a cursor stops once it has spent what it was given,
   however many steps that took. Measured 2026-08-27 on an endless generator:
   budgets of 1,000 / 5,000 / 20,000 / 100,000 stopped after spending 1,004 /
   5,004 / 20,004 / 100,004.

   The WALL bound applies per step, so time spent between steps, while the
   caller is doing something else, does not count against it.

   An eager cetta_run() or cetta_load() is bounded as one call, both ways.

   The inference counter is the engine's, and the engine is one per process, so
   a cursor stepped while another thread is also evaluating charges that work
   to whichever cursor happened to be stepping. That is the same reading
   cetta_stats() carries and the same one the Python seat's counters carry. */
CETTA_API cetta_status_t cetta_set_limits(cetta_t *runtime,
                                          const cetta_limits_t *limits);

/* What is in force now. */
CETTA_API void cetta_get_limits(const cetta_t *runtime, cetta_limits_t *out);

/* ------------------------------------------------------------------ *
 * Measuring
 * ------------------------------------------------------------------ */

/* The engine's own counters. Inferences are DETERMINISTIC where wall clock is
   not, which is why anything measured in this tree is gated on them: five runs
   of one workload on a loaded machine gave the same inference count every
   time while wall clock swung several percent. */
typedef struct cetta_stats {
  uint64_t inferences;   /* engine steps                                    */
  double   cputime;      /* engine CPU seconds                              */
  uint64_t gc_count;     /* collections                                     */
  uint64_t gc_freed;     /* bytes freed by them                             */
  double   gc_time;      /* seconds spent collecting                        */
  uint64_t table_bytes;  /* answer-table bytes, which is tabling's memory   */
} cetta_stats_t;

/* Sample the counters now. Take two and subtract with cetta_stats_delta():
   two samples and a subtraction is the shape getrusage() and clock_gettime()
   already gave C, and it needs no block construct C does not have.

   The engine is one per process, so a measurement spanning other threads'
   engine work counts that work too. The honest reading is "what the engine
   did between these two samples". */
CETTA_API cetta_status_t cetta_stats(cetta_t *runtime, cetta_stats_t *out);

/* after - before, field by field. */
CETTA_API void cetta_stats_delta(const cetta_stats_t *before,
                                 const cetta_stats_t *after,
                                 cetta_stats_t *out);

/* ------------------------------------------------------------------ *
 * Publishing C functions to MeTTa
 * ------------------------------------------------------------------ */

/* The five ranked effect classes. Naming an operation's class is required,
   not advisory: the engine reasons about caching, reordering and transactions
   from it, and a wrong answer here is a wrong program. */
typedef enum cetta_effect {
  CETTA_PURE_STRUCTURAL,          /* same answer always, no reads, no writes */
  CETTA_READ_ONLY_LOOKUP,         /* reads state, writes none               */
  CETTA_NONDETERMINISTIC_READ_ONLY, /* reads, may answer differently        */
  CETTA_WRITES_STATE,             /* changes something                      */
  CETTA_ORACLE_IO                 /* reaches the world                      */
} cetta_effect_t;

CETTA_API const char *cetta_effect_str(cetta_effect_t effect);

/* One application of a published C function. */
typedef struct cetta_call cetta_call_t;

/* How many arguments this application carries. */
CETTA_API size_t cetta_call_arity(const cetta_call_t *call);

/* Argument `index`, BORROWED for the duration of the call. */
CETTA_API const cetta_atom_t *cetta_call_arg(const cetta_call_t *call, size_t index);

/* The runtime this call is running inside, for cetta_parse and cetta_show. */
CETTA_API cetta_t *cetta_call_runtime(const cetta_call_t *call);

/* Answer with an atom. STEALS it. Calling this twice in one application is
   CETTA_MISUSE; a function that answers many values answers one expression
   and lets MeTTa's own superpose spread it. */
CETTA_API cetta_status_t cetta_call_return(cetta_call_t *call, cetta_atom_t *atom);

/* Refuse this application with words the engine reports as an error. */
CETTA_API void cetta_call_error(cetta_call_t *call, const char *message);

/* A published C function. Answer CETTA_OK having called cetta_call_return(),
   CETTA_FAIL to say there is no answer for these arguments, or CETTA_ERROR
   having called cetta_call_error(). */
typedef cetta_status_t (*cetta_op_fn)(cetta_call_t *call, void *user);

/* Publish `fn` under `name` at exactly `arity`, so `(name a b)` in MeTTa
   calls it.

   The name reaches MeTTa as written, with one map: C spells a compound name
   with underscores and MeTTa spells it with hyphens, so `car_atom` publishes
   `car-atom`. A name outside C's identifier grammar (`prime?`, `%Undefined%`)
   is written literally and crosses untouched. */
CETTA_API cetta_status_t cetta_op(cetta_t *runtime, const char *name,
                                  size_t arity, cetta_effect_t effect,
                                  cetta_op_fn fn, void *user);

/* Withdraw a published function at every arity, giving the name back. */
CETTA_API cetta_status_t cetta_op_remove(cetta_t *runtime, const char *name);

/* --- carrying a C value through MeTTa untouched --- */

/* Called when the last reference to a CETTA_OBJECT goes, so a C value handed
   to the language can own heap memory. NULL means the value owns nothing. */
typedef void (*cetta_object_free_fn)(void *value);

/* Wrap a C pointer as a grounded atom. MeTTa carries it by reference, never
   serialises it, and hands it back to a published function unchanged. */
CETTA_API cetta_atom_t *cetta_object(void *value, const char *type_name,
                                     cetta_object_free_fn release);

/* The type name given at construction, for a function checking what it got. */
CETTA_API const char *cetta_object_type(const cetta_atom_t *atom);

/* A C function as a VALUE rather than a name: the atom itself is applicable,
   so `($f 2)` calls it wherever the atom lands. This is what C answers to the
   Python seat's "any callable is a grounded atom"; cetta_op() is the other
   half, a function reached by the name it was published under. */
CETTA_API cetta_atom_t *cetta_function(cetta_op_fn fn, void *user,
                                       cetta_object_free_fn release);

#ifdef __cplusplus
}
#endif

#endif /* CETTA_H */
