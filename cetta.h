/* Purpose: drive the PeTTa engine from C. Boot it, build and read MeTTa terms
 *   as C values, run programs, pull answers one at a time, publish C functions
 *   the language calls, bound an evaluation and measure one.
 *
 * Assumes:
 *   - SWI-Prolog 10 with its development headers, threads enabled
 *     [source: /usr/lib/swi-prolog/include/SWI-Prolog.h, PLVERSION 100113]
 *   - C11. _Generic carries the overloads and the argument coercions. Without
 *     it every long-named function still works and the macros do not.
 *   - the engine tree is reachable, either at the path given to cetta_open()
 *     or at $PETTA_PATH
 *
 * Guarantees:
 *   - every function that can fail says so, and none of them print, exit or
 *     longjmp; no Prolog exception crosses this header
 *   - an atom is immutable and refcounted, so a term built once may be run
 *     many times and shared between threads without copying
 *   - building and reading atoms starts no engine
 *     [tested: tests/test_cetta.c, test_atoms_need_no_engine; commit=56dcac4afc074dce9e401174c65cedc3071075ae]
 *   - cetta_eval() computes one answer per step, so a caller that stops
 *     pulling leaves the rest of an infinite stream uncomputed, and
 *     cetta_each() closes the cursor on `break` as well as on exhaustion
 *     [tested: tests/test_cetta.c, test_the_walk_closes_its_cursor_on_break;
 *     commit=56dcac4afc074dce9e401174c65cedc3071075ae]
 *
 * Owns resources: one Prolog runtime per process, released by cetta_close();
 *   one engine per open cursor, released by cetta_answers_free(), which
 *   cetta_each() calls for you; one malloc'ed block per atom, released when
 *   its last reference goes.
 *
 * Decides, and these six are the whole contract:
 *
 *   1. THE OWNERSHIP LAW, carried by C's own type system. A function taking
 *      `const cetta_atom *` BORROWS it and you still own it. A function
 *      taking `cetta_atom *` (non-const) TAKES it and you must not drop it
 *      afterwards. Constructors hand you one reference. Accessors hand back
 *      borrowed pointers that live as long as their parent.
 *
 *      Every door you pass a freshly built term to TAKES it, so the common
 *      shape leaks nothing and needs no cleanup line:
 *
 *          cetta_add(kb, cetta_expr("edge", "a", "b"));
 *
 *      To pass a term you mean to keep, hand over a new reference with
 *      cetta_keep(). That is the one thing to remember:
 *
 *          cetta_atom *p = cetta_expr("edge", "a", cetta_var("y"));
 *          while (...) cetta_each (row, cetta_match(kb, cetta_keep(p))) ...
 *          cetta_drop(p);
 *
 *   2. ERRORS ARE errno-SHAPED. A function that produces a value returns it,
 *      or NULL, or a documented zero. cetta_error() and cetta_errmsg() say
 *      what went wrong. Like errno they are SET on failure and NOT cleared on
 *      success, so a run of calls is checked once, where it suits you:
 *
 *          cetta_clear();
 *          double x = cetta_float(cetta_arg(c, 0));
 *          double y = cetta_float(cetta_arg(c, 1));
 *          if ( !cetta_ok() ) return cetta_fail(c, "wanted two numbers");
 *
 *      [tested: tests/test_cetta.c, test_the_error_state_is_errno_shaped;
 *      commit=56dcac4afc074dce9e401174c65cedc3071075ae]
 *
 *   3. ONE VERB, EITHER RECEIVER. cetta_eval, cetta_match, cetta_atoms,
 *      cetta_add, cetta_del, cetta_count and cetta_wipe each take a `cetta *`,
 *      meaning its &self, or a `cetta_space *`. _Generic picks; the pair it
 *      picks between is declared above each macro for anyone who wants it.
 *      [tested: tests/test_cetta.c, test_one_verb_takes_either_receiver;
 *      commit=56dcac4afc074dce9e401174c65cedc3071075ae]
 *
 *   4. A MeTTa Number splits into CETTA_INT and CETTA_FLOAT, because C has two
 *      types where the wire codec has one tag and MeTTa tells 2 from 2.0
 *      apart. Values outside int64 and rationals get their own kinds rather
 *      than being rounded into one that fits.
 *
 *   5. READING PROMOTES WHERE IT IS LOSSLESS AND REFUSES WHERE IT IS NOT.
 *      cetta_float() of an Int answers that integer, because the conversion
 *      loses nothing below 2^53 and is refused above it. cetta_int() of a
 *      Float does NOT round. This is the promotion-lattice reading upstream
 *      Hyperon's own bridging note argues for: "if a promotion path for a
 *      value exists to get to the requested Inner Type, then the accessor
 *      seamlessly works. If a promotion path does not exist then the accessor
 *      will fail."
 *      [tested: tests/test_cetta.c,
 *      test_reading_promotes_only_where_it_is_lossless; commit=56dcac4afc074dce9e401174c65cedc3071075ae]
 *
 *   6. A BARE C STRING IN TERM POSITION IS A SYMBOL. cetta_expr("+", 1, 2) is
 *      (+ 1 2), not ("+" 1 2). MeTTa source writes a symbol bare and a string
 *      quoted; in C everything is quoted, so the default is the one MeTTa
 *      writes bare. Text is cetta_text("..."), which is never ambiguous.
 *
 * Fails when: the caller wants two independent runtimes in one process
 *   (PL_initialise is process-wide), or wants to hold an engine term rather
 *   than a materialised copy. Both are in ai-cetta-c-constraints.md.
 *
 * Guarded by: nothing, deliberately. An atom is immutable and its refcount is
 *   atomic, so building, sharing and dropping atoms is safe from any thread,
 *   and the error state is thread-local. The operation table is NOT guarded:
 *   publish every operation before the threads that evaluate start, the same
 *   restriction sqlite3_create_function() carries.
 *
 * Open Obligations:
 *   To Do: None
 *   Hacks: None
 *   Future Enhancements: None
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

/* ================================================================== *
 * Status
 * ================================================================== */

/* CETTA_ROW and CETTA_DONE are answers rather than problems: they are how a
   cursor reports progress, the split sqlite3_step() established. */
typedef enum cetta_status {
  CETTA_OK = 0,          /* the call did what it said                      */
  CETTA_ROW = 1,         /* a cursor produced an answer                    */
  CETTA_DONE = 2,        /* a cursor is exhausted                          */
  CETTA_FAIL = 3,        /* the engine had no answer; not an error         */
  CETTA_ERROR = 4,       /* the engine raised                              */
  CETTA_NOMEM = 5,       /* allocation failed                              */
  CETTA_MISUSE = 6,      /* this library's contract was broken             */
  CETTA_UNSUPPORTED = 7, /* a real value C has no type for, refused by name */
  CETTA_LIMIT = 8        /* a bound stopped it; you did that, it did not break */
} cetta_status;

/* The last failure on THIS thread. Set on failure, NOT cleared on success,
   exactly as errno is, so a run of calls can be checked once at the end. */
CETTA_API cetta_status cetta_error(void);

/* Its words, or NULL if nothing has failed since the last cetta_clear(). The
   text is owned by the library and overwritten by the next failure here. */
CETTA_API const char *cetta_errmsg(void);

/* Whether nothing has failed on this thread since the last cetta_clear(). */
CETTA_API bool cetta_ok(void);

/* Forget the last failure. Call this before a run you intend to check. */
CETTA_API void cetta_clear(void);

/* A stable English name for a status, for your own diagnostics. */
CETTA_API const char *cetta_status_str(cetta_status status);

CETTA_API const char *cetta_version(void);

/* ================================================================== *
 * Atoms
 * ================================================================== */

typedef struct cetta_atom cetta_atom;

/* The nine wire tags of CODEC.md, with the one tag C splits four ways. */
typedef enum cetta_kind {
  CETTA_NONE = -1,/* not an atom; what cetta_kind_of(NULL) answers       */
  CETTA_SYMBOL,   /* `s`: a name that denotes itself                    */
  CETTA_TEXT,     /* `g`: a grounded value carried as text              */
  CETTA_INT,      /* `n`: an exact integer that fits int64_t            */
  CETTA_FLOAT,    /* `n`: a float                                       */
  CETTA_BIGINT,   /* `n`: an exact integer too wide for int64_t         */
  CETTA_RATIONAL, /* `n`: an exact ratio                                */
  CETTA_BOOL,     /* `b`: True or False, which are not symbols          */
  CETTA_VARIABLE, /* `v`: a variable, its name an identity in its term  */
  CETTA_EXPR,     /* `e`: an expression; the empty one is unit          */
  CETTA_SPACE,    /* `p`: an executable space reference                 */
  CETTA_OBJECT,   /* `o`: a live C value crossing by reference          */
  CETTA_HANDLE    /* `h`: a native engine value held by reference       */
} cetta_kind;

CETTA_API const char *cetta_kind_str(cetta_kind kind);

/* --- building. None of these start the engine. --- */

CETTA_API cetta_atom *cetta_sym(const char *name);
CETTA_API cetta_atom *cetta_var(const char *name);
CETTA_API cetta_atom *cetta_text(const char *text);
CETTA_API cetta_atom *cetta_textn(const char *text, size_t length);
CETTA_API cetta_atom *cetta_num(int64_t value);
CETTA_API cetta_atom *cetta_real(double value);
CETTA_API cetta_atom *cetta_bool(bool value);
CETTA_API cetta_atom *cetta_unit(void);

/* An exact integer wider than int64_t, as decimal digits with an optional
   leading minus. NULL on any other spelling. */
CETTA_API cetta_atom *cetta_bigint(const char *decimal);

/* An exact ratio. A zero denominator is refused. */
CETTA_API cetta_atom *cetta_ratio(int64_t numerator, int64_t denominator);

/* A space reference by its portable engine name, which begins with '&'. */
CETTA_API cetta_atom *cetta_spaceref(const char *name);

/* An expression from an array. The children are TAKEN; the array is not. */
CETTA_API cetta_atom *cetta_exprv(size_t count, cetta_atom **children);

/* The widened forms cetta_atom_of dispatches to. Call cetta_num or cetta_real
   directly rather than these. */
CETTA_API cetta_atom *cetta_num_(long long value);
CETTA_API cetta_atom *cetta_real_(long double value);
CETTA_API cetta_atom *cetta_same(cetta_atom *atom);
CETTA_API cetta_atom *cetta_same_c(const cetta_atom *atom);

/* Turn one C value into an atom: an integer becomes a Number, a float a
   Number, a bare string a SYMBOL (decision 6), and an atom itself.

   The `1 ? (x) : (x)` is what makes a string literal work. _Generic does not
   decay an array, so `char[4]` would match no branch; a conditional
   expression decays both of its operands, which is the one spelling that
   also survives `cetta_atom *` being a pointer to an INCOMPLETE type. `(x)+0`
   reads more simply and is what this used first, but it is arithmetic, and
   arithmetic on a pointer to an incomplete type does not compile.

   The conditional applies the usual arithmetic conversions, so a C `bool` and
   a C `char` both arrive as `int`: `true` builds the Number 1 and 'x' builds
   120. C conflates those and this cannot un-conflate them; use cetta_bool()
   and cetta_text() when you mean those. */
#define cetta_atom_of(x) _Generic(1 ? (x) : (x),                             \
    char *:              cetta_sym,       const char *:       cetta_sym,     \
    signed char:         cetta_num_,      unsigned char:      cetta_num_,    \
    short:               cetta_num_,      unsigned short:     cetta_num_,    \
    int:                 cetta_num_,      unsigned:           cetta_num_,    \
    long:                cetta_num_,      unsigned long:      cetta_num_,    \
    long long:           cetta_num_,      unsigned long long: cetta_num_,    \
    float:               cetta_real_,     double:             cetta_real_,   \
    long double:         cetta_real_,                                        \
    cetta_atom *:        cetta_same,      const cetta_atom *: cetta_same_c)(x)

/* An expression, with no count to keep in step and every child coerced:

       cetta_expr("+", 1, 2)                       (+ 1 2)
       cetta_expr("edge", "a", cetta_var("y"))     (edge a $y)

   Children are TAKEN. If any is NULL the whole call fails, drops the ones it
   was given and returns NULL, so a failed inner constructor cannot leak
   through an outer one. Sixteen children is the ceiling; wider uses
   cetta_exprv().
   [tested: tests/test_cetta.c,
   test_the_builder_coerces_each_child_by_its_c_type; commit=56dcac4afc074dce9e401174c65cedc3071075ae] */
#define cetta_expr(...)                                                      \
    cetta_exprv(CETTA_NARG(__VA_ARGS__),                                     \
                (cetta_atom *[]){ CETTA_MAP(__VA_ARGS__) })

/* --- lifetime --- */

/* Take a reference. Returns its argument, so it composes inline. NULL-safe. */
CETTA_API cetta_atom *cetta_keep(const cetta_atom *atom);

/* Drop a reference. NULL-safe. */
CETTA_API void cetta_drop(const cetta_atom *atom);

/* --- reading. Each returns the value the way atoi() and strlen() do, and
       records a failure you can check with cetta_ok(). --- */

CETTA_API cetta_kind cetta_kind_of(const cetta_atom *atom);

/* The name of a SYMBOL, VARIABLE or SPACE, the text of a TEXT, the digits of
   a BIGINT. NULL for every other kind. Borrowed. */
CETTA_API const char *cetta_name(const cetta_atom *atom);
CETTA_API size_t cetta_name_len(const cetta_atom *atom);

/* An exact integer. INT only: a Float is not rounded here, and a BigInt does
   not fit by definition. 0 and a recorded failure otherwise. */
CETTA_API int64_t cetta_int(const cetta_atom *atom);

/* A double. Promotes losslessly (decision 5): a Float is itself, an Int of
   magnitude below 2^53 is exact, a Rational is its quotient. An Int above
   2^53 and a BigInt are REFUSED rather than rounded. 0.0 and a recorded
   failure otherwise. */
CETTA_API double cetta_float(const cetta_atom *atom);

CETTA_API bool cetta_truth(const cetta_atom *atom);
CETTA_API bool cetta_ratio_of(const cetta_atom *atom, int64_t *numerator,
                              int64_t *denominator);

/* Child count of an EXPR, 0 otherwise. */
CETTA_API size_t cetta_len(const cetta_atom *atom);

/* Child `index`, BORROWED and valid while its parent lives. NULL past the end
   or on a non-expression. cetta_keep() it to hold it longer. */
CETTA_API const cetta_atom *cetta_at(const cetta_atom *atom, size_t index);

/* Structural equality. Two variables are equal when their names are. */
CETTA_API bool cetta_eq(const cetta_atom *a, const cetta_atom *b);

/* --- text, through the engine's own reader and writer --- */

/* Read one MeTTa form. The engine's reader is the only reader. */
CETTA_API cetta_atom *cetta_parse(const char *source);

/* Write an atom the way the engine writes it, into a per-thread rotating
   buffer so it drops straight into printf:

       printf("%s -> %s\n", cetta_show(pattern), cetta_show(answer));

   The buffer is reused after CETTA_SHOW_SLOTS further calls on this thread,
   which is the contract strerror() and inet_ntoa() already gave C. Take a
   copy with cetta_show_dup() to keep it, and free that with cetta_free(). */
#define CETTA_SHOW_SLOTS 8
CETTA_API const char *cetta_show(const cetta_atom *atom);
CETTA_API char *cetta_show_dup(const cetta_atom *atom);

/* Free anything this library handed back by pointer that is not an atom. */
CETTA_API void cetta_free(void *pointer);

/* ================================================================== *
 * The runtime
 * ================================================================== */

typedef struct cetta cetta;
typedef struct cetta_space cetta_space;
typedef struct cetta_answers cetta_answers;

typedef struct cetta_config {
  const char *path;      /* engine tree; NULL takes $PETTA_PATH then the
                            tree this library was built beside          */
  size_t stack_limit;    /* bytes; 0 takes the engine's own default      */
  bool verbose;          /* let the engine print each compiled form      */
} cetta_config;

/* Boot the engine. `config` may be NULL for every default. NULL on failure.

   One runtime per process: PL_initialise() sets up the process's single
   Prolog heap, so a second cetta_open() with a matching configuration hands
   back the same runtime and one with a different path fails. */
CETTA_API cetta *cetta_open(const cetta_config *config);

/* Shut the runtime down. Atoms outlive it: they are C memory and stay valid
   until their own references go. */
CETTA_API void cetta_close(cetta *runtime);

/* Whether the engine prints compiled forms. Returns the previous setting. */
CETTA_API bool cetta_verbose(cetta *runtime, bool verbose);

/* A thread other than the one that opened the runtime attaches before it
   touches the engine and detaches before it exits. Building and reading atoms
   needs neither. */
CETTA_API bool cetta_thread_attach(void);
CETTA_API void cetta_thread_detach(void);

/* &self and &petta, borrowed and living as long as the runtime. */
CETTA_API cetta_space *cetta_self(cetta *runtime);
CETTA_API cetta_space *cetta_catalog(cetta *runtime);

/* Create or open a space by name; names begin with '&'. NULL on failure. */
CETTA_API cetta_space *cetta_space_open(cetta *runtime, const char *name);
CETTA_API void cetta_space_close(cetta_space *space);
CETTA_API const char *cetta_space_name(const cetta_space *space);

/* ================================================================== *
 * Asking
 * ================================================================== */

/* Run MeTTa source in &self. Every `!` form contributes a group of answers in
   source order; cetta_group() says which group the current answer is in.

   Eager: the engine's run door computes the whole program before the first
   answer, because that is what running a program means. cetta_eval() is the
   lazy door. NULL on failure. */
CETTA_API cetta_answers *cetta_run(cetta *runtime, const char *source);

/* Load a file through the same door `import!` uses, so a reload replaces the
   first load's definitions rather than doubling them. */
CETTA_API cetta_answers *cetta_load(cetta *runtime, const char *path);

/* The pairs the verbs below dispatch between. Call these directly if you
   would rather not go through _Generic. Each TAKES its atom argument. */
CETTA_API cetta_answers *cetta_self_eval(cetta *runtime, cetta_atom *goal);
CETTA_API cetta_answers *cetta_space_eval(cetta_space *space, cetta_atom *goal);
CETTA_API cetta_answers *cetta_self_match(cetta *runtime, cetta_atom *pattern);
CETTA_API cetta_answers *cetta_space_match(cetta_space *space, cetta_atom *pattern);
CETTA_API cetta_answers *cetta_self_atoms(cetta *runtime);
CETTA_API cetta_answers *cetta_space_atoms(cetta_space *space);
CETTA_API bool cetta_self_add(cetta *runtime, cetta_atom *atom);
CETTA_API bool cetta_space_add(cetta_space *space, cetta_atom *atom);
CETTA_API bool cetta_self_del(cetta *runtime, cetta_atom *atom);
CETTA_API bool cetta_space_del(cetta_space *space, cetta_atom *atom);
CETTA_API size_t cetta_self_count(cetta *runtime);
CETTA_API size_t cetta_space_count(cetta_space *space);
CETTA_API bool cetta_self_wipe(cetta *runtime);
CETTA_API bool cetta_space_wipe(cetta_space *space);

/* Only the selected branch is called; the others are just function names, so
   each one type-checks against its own receiver. This is how tgmath.h works. */
#define CETTA_ON(target, verb) _Generic((target),                            \
    cetta *:        cetta_self_##verb,                                       \
    cetta_space *:  cetta_space_##verb)

/* Evaluate one atom LAZILY: each step computes at most one answer, and
   abandoning the cursor leaves the rest uncomputed. TAKES `goal`. */
#define cetta_eval(target, goal)   CETTA_ON((target), eval)((target), (goal))

/* Stored atoms unifying a pattern, lazily. TAKES `pattern`. */
#define cetta_match(target, pat)   CETTA_ON((target), match)((target), (pat))

/* Every stored atom, lazily. */
#define cetta_atoms(target)        CETTA_ON((target), atoms)((target))

/* Add one atom. TAKES it. */
#define cetta_add(target, atom)    CETTA_ON((target), add)((target), (atom))

/* Remove one exact atom; true when it was there. TAKES it. */
#define cetta_del(target, atom)    CETTA_ON((target), del)((target), (atom))

/* How many atoms are stored. */
#define cetta_count(target)        CETTA_ON((target), count)((target))

/* Empty it. */
#define cetta_wipe(target)         CETTA_ON((target), wipe)((target))

/* --- reading answers --- */

/* The next answer, BORROWED and valid until the following step, or NULL at
   the end. NULL is also what a failure gives, and cetta_ok() tells the two
   apart. This is what cetta_each() calls. */
CETTA_API const cetta_atom *cetta_next(cetta_answers *answers);

/* Which `!` form produced the current answer, counting from 0. Always 0 for
   the lazy doors, which evaluate one goal. */
CETTA_API size_t cetta_group(const cetta_answers *answers);

/* The engine's own rendering of the current answer. Presentation: it can show
   a value cetta_show() refuses, a host-only value or a non-finite float. */
CETTA_API const char *cetta_answer_text(const cetta_answers *answers);

/* Release the cursor and, for a lazy one, the engine behind it. NULL-safe.
   cetta_each() does this for you. */
CETTA_API void cetta_answers_free(cetta_answers *answers);

/* The first answer, OWNED, with the cursor closed and the rest left
   uncomputed. NULL when there is none. CONSUMES `answers`:

       cetta_atom *a = cetta_first(cetta_eval(m, cetta_expr("+", 1, 2)));
       ...
       cetta_drop(a);

   The atom is yours, so it is yours to drop. When all you want is the VALUE,
   the four below do that without an atom ever landing in your hands. */
CETTA_API cetta_atom *cetta_first(cetta_answers *answers);

/* EXACTLY one answer, OWNED, or NULL with a failure recorded when there were
   none or more than one. The Python seat draws the same line between one()
   and first(), and the word means the same thing here: `one` is a claim about
   the cardinality and `first` is not. CONSUMES `answers`
   [tested: tests/test_cetta.c, test_one_and_first_make_different_claims;
   commit=56dcac4afc074dce9e401174c65cedc3071075ae]. */
CETTA_API cetta_atom *cetta_one(cetta_answers *answers);

/* Ask for exactly one answer and read it as a C value: the cursor is closed,
   the atom is released, and the whole question is one expression.

       printf("%lld\n", (long long)cetta_one_int(cetta_eval(m, E("+", 1, 2))));

   Each CONSUMES `answers` and carries cetta_one()'s cardinality claim, so a
   question that answered twice is a recorded failure rather than a silent
   first. Each records a failure, so cetta_ok() tells "no answer" and "wrong
   kind" apart from a real zero. cetta_one_name() borrows the same per-thread
   ring cetta_show() uses. */
CETTA_API int64_t cetta_one_int(cetta_answers *answers);
CETTA_API double cetta_one_float(cetta_answers *answers);
CETTA_API bool cetta_one_truth(cetta_answers *answers);
CETTA_API const char *cetta_one_name(cetta_answers *answers);

/* Every answer in order as one owned array, the eager door for a caller who
   wants them all. CONSUMES `answers`. n_out may be NULL. Release with
   cetta_atoms_free(), which drops each atom and frees the array. */
CETTA_API cetta_atom **cetta_all(cetta_answers *answers, size_t *n_out);
CETTA_API void cetta_atoms_free(cetta_atom **atoms, size_t count);

/* Walk every answer and close the cursor, however the loop is left:

       cetta_each (a, cetta_run(m, "!(superpose (1 2 3))"))
           printf("%s\n", cetta_show(a));

   `break` is safe and closes the cursor. `return` and `goto` out of the body
   are NOT: they leave without running the loop's increment, so free it by
   hand there, or use CETTA_AUTO_ASK below. */
#define cetta_each(var, answers)                                             \
    CETTA_EACH_(var, answers, CETTA_ID(cetta_it_))

/* The same walk with the cursor in hand, for the body that needs to ask it
   something: which `!` form this answer came from, or how the engine itself
   rendered it.

       cetta_each_cursor (a, it, cetta_run(m, src))
           printf("group %zu: %s\n", cetta_group(it), cetta_answer_text(it)); */
#define cetta_each_cursor(var, cursor, answers)                              \
  for (cetta_answers *cursor = (answers); cursor != NULL;                    \
       cetta_answers_free(cursor), cursor = NULL)                            \
    for (const cetta_atom *var; (var = cetta_next(cursor)) != NULL; )

/* The cursor's name is generated ONCE, by the caller above, and passed in as
   a parameter. Generating it at each mention would give three different names
   because __COUNTER__ increments every time it is read. */
#define CETTA_EACH_(var, answers, it)                                        \
  for (cetta_answers *it = (answers); it != NULL;                            \
       cetta_answers_free(it), it = NULL)                                    \
    for (const cetta_atom *var; (var = cetta_next(it)) != NULL; )

/* ================================================================== *
 * Publishing C functions to MeTTa
 * ================================================================== */

/* The five ranked effect classes. Naming one is required, not advisory: the
   engine reasons about caching, reordering and transactions from it, and a
   wrong answer here is a wrong program. */
typedef enum cetta_effect {
  CETTA_PURE,      /* same answer always, reads nothing, writes nothing */
  CETTA_LOOKUP,    /* reads state, writes none                          */
  CETTA_NONDET,    /* reads, and may answer differently                 */
  CETTA_WRITES,    /* changes something                                 */
  CETTA_IO         /* reaches the world                                 */
} cetta_effect;

CETTA_API const char *cetta_effect_str(cetta_effect effect);

typedef struct cetta_call cetta_call;

/* Answer CETTA_OK having called cetta_answer(), CETTA_FAIL to say there is no
   answer for these arguments, or return cetta_fail() to refuse with words. */
typedef cetta_status (*cetta_fn)(cetta_call *call, void *user);

/* One published function. Designated initializers make the call site name
   what it is passing, which is C's answer to keyword arguments:

       cetta_def(m, (cetta_op){ .name = "hypot", .arity = 2,
                                .effect = CETTA_PURE, .fn = op_hypot }); */
typedef struct cetta_op {
  const char  *name;    /* as written; underscores reach MeTTa as hyphens */
  size_t       arity;
  cetta_effect effect;
  cetta_fn     fn;
  void        *user;    /* handed back to fn on every application         */
} cetta_op;

/* Publish, so `(name a b)` in MeTTa calls it. The name reaches MeTTa through
   C's own casing convention, so `car_atom` publishes `car-atom`; a name
   outside C's identifier grammar crosses untouched, which is the escape for
   `prime?` and `%Undefined%`. */
CETTA_API bool cetta_def(cetta *runtime, cetta_op op);

/* Withdraw a published function at every arity, giving the name back. */
CETTA_API bool cetta_undef(cetta *runtime, const char *name);

/* Inside a published function. */
CETTA_API size_t cetta_arity(const cetta_call *call);
CETTA_API const cetta_atom *cetta_arg(const cetta_call *call, size_t index);
CETTA_API cetta *cetta_of(const cetta_call *call);

/* Answer with an atom, which is TAKEN. Answering twice is CETTA_MISUSE; a
   function with many answers answers one expression and lets MeTTa's own
   superpose spread it. Returns CETTA_OK so it can be the return statement. */
CETTA_API cetta_status cetta_answer(cetta_call *call, cetta_atom *atom);

/* Refuse this application with words the engine reports. Returns CETTA_ERROR
   so it too can be the return statement. */
CETTA_API cetta_status cetta_fail(cetta_call *call, const char *message);

/* --- carrying a C value through MeTTa untouched --- */

typedef void (*cetta_free_fn)(void *value);

/* Wrap a C pointer as a grounded atom. MeTTa carries it by reference, never
   serialises it, and hands it back unchanged. */
CETTA_API cetta_atom *cetta_object(void *value, const char *type_name,
                                   cetta_free_fn release);
CETTA_API void *cetta_value(const cetta_atom *atom);
CETTA_API const char *cetta_type(const cetta_atom *atom);

/* A C function as a VALUE rather than a name, so `($f 2)` calls it wherever
   the atom lands. This is what C answers to a Python callable being an atom;
   cetta_def() is the other half, a function reached by its published name. */
CETTA_API cetta_atom *cetta_function(cetta_fn fn, void *user,
                                     cetta_free_fn release);

/* ================================================================== *
 * Bounding and measuring
 * ================================================================== */

/* What an evaluation may spend. Zero on a field means no bound there. A bound
   stops work MID-WAY and writes already made stand, which is the honest
   semantics of every timeout. */
typedef struct cetta_limits {
  double   seconds;      /* wall seconds one call, or one step, may take */
  uint64_t inferences;   /* engine steps it may spend                    */
  size_t   stack_bytes;  /* SWI's stack ceiling, which a runaway recursion
                            hits; NOT MeTTa's reduction depth, which is the
                            max-stack-depth pragma in the program text    */
} cetta_limits;

/* Bounds for every later call. NULL clears them.

   On a lazy cursor the inference bound is a CUMULATIVE budget for the whole
   cursor: the engine's counter is read around every step and the deltas added
   up. Measured 2026-08-27 on an endless generator, budgets of 1,000 / 5,000 /
   20,000 / 100,000 stopped after spending 1,004 / 5,004 / 20,004 / 100,004
   [tested: tests/test_cetta.c, test_a_bound_stops_a_runaway_and_says_so;
   commit=56dcac4afc074dce9e401174c65cedc3071075ae].
   The wall bound applies per step, so time the host spends between steps does
   not count against it. An eager cetta_run() is bounded as one call. */
CETTA_API bool cetta_limit(cetta *runtime, const cetta_limits *limits);
CETTA_API cetta_limits cetta_limits_of(const cetta *runtime);

/* The engine's own counters. Inferences are DETERMINISTIC where wall clock is
   not, which is why this tree gates on them. */
typedef struct cetta_stats {
  uint64_t inferences;
  double   cputime;
  uint64_t gc_count;
  uint64_t gc_freed;
  double   gc_time;
  uint64_t table_bytes;
} cetta_stats;

/* Sample now. Take two and subtract: that is the shape getrusage() gave C and
   it needs no block construct C does not have. */
CETTA_API cetta_stats cetta_stats_now(cetta *runtime);
CETTA_API cetta_stats cetta_stats_since(cetta_stats before, cetta_stats after);

/* ================================================================== *
 * Scope cleanup, where the compiler has it
 * ================================================================== */

#if defined(__GNUC__) || defined(__clang__)
#define CETTA_HAS_AUTO 1
static inline void cetta_drop_p(cetta_atom **p) { cetta_drop(*p); }
static inline void cetta_answers_free_p(cetta_answers **p) { cetta_answers_free(*p); }
/* Released when the block is left, however it is left, including by return
   and goto. This is systemd's `_cleanup_` and the Linux kernel's `__free`; it
   is a GCC and Clang extension rather than ISO C, which is why it sits behind
   CETTA_HAS_AUTO. */
#define CETTA_AUTO      __attribute__((cleanup(cetta_drop_p)))
#define CETTA_AUTO_ASK  __attribute__((cleanup(cetta_answers_free_p)))
/* Hand a resource out of a CETTA_AUTO variable without it being released. */
/* __extension__ is how GCC and Clang are told that the statement expression
   below is a deliberate extension, so -Wpedantic stays on for everything
   else rather than being turned off for the whole build over one line. */
#define CETTA_TAKE(p) \
    __extension__ ({ __typeof__(p) cetta_taken_ = (p); (p) = NULL; cetta_taken_; })
#endif

/* ================================================================== *
 * Shorthand, opt-in
 * ================================================================== */

/* `#define CETTA_SHORTHAND` before including this header for the one-letter
   builders. Off by default because S, V, T, N, R, B and E are short names in
   C's single flat namespace and a program that already uses one should not
   have it taken. The long names always work. */
#ifdef CETTA_SHORTHAND
#define S(name)   cetta_sym(name)
#define V(name)   cetta_var(name)
#define T(text)   cetta_text(text)
#define N(value)  cetta_num(value)
#define R(value)  cetta_real(value)
#define B(value)  cetta_bool(value)
#define E(...)    cetta_expr(__VA_ARGS__)
#endif

/* ================================================================== *
 * Macro machinery
 * ================================================================== */

#define CETTA_CAT_(a, b) a##b
#define CETTA_CAT(a, b) CETTA_CAT_(a, b)

/* cetta_each() needs one name it can both declare and refer to three times.
   __COUNTER__ would give a different name at each mention, so the counter is
   bumped once per loop and CETTA_ID_LAST names that same variable again. */
#ifdef __COUNTER__
#define CETTA_ID(base)  CETTA_CAT(base, __COUNTER__)
#else
/* Without __COUNTER__ two cetta_each() loops on ONE source line would collide.
   Nesting across lines is still fine. */
#define CETTA_ID(base)  CETTA_CAT(base, __LINE__)
#endif

/* Count the arguments, so no call site carries a length that can drift out of
   step with the list beside it. */
#define CETTA_NARG(...) CETTA_NARG_(__VA_ARGS__, 16,15,14,13,12,11,10,9,      \
                                    8,7,6,5,4,3,2,1,0)
#define CETTA_NARG_(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16,  \
                    N,...) N

/* Apply cetta_atom_of to each argument. */
#define CETTA_MAP(...) CETTA_CAT(CETTA_MAP_, CETTA_NARG(__VA_ARGS__))(__VA_ARGS__)
#define CETTA_MAP_1(a)       cetta_atom_of(a)
#define CETTA_MAP_2(a, ...)  cetta_atom_of(a), CETTA_MAP_1(__VA_ARGS__)
#define CETTA_MAP_3(a, ...)  cetta_atom_of(a), CETTA_MAP_2(__VA_ARGS__)
#define CETTA_MAP_4(a, ...)  cetta_atom_of(a), CETTA_MAP_3(__VA_ARGS__)
#define CETTA_MAP_5(a, ...)  cetta_atom_of(a), CETTA_MAP_4(__VA_ARGS__)
#define CETTA_MAP_6(a, ...)  cetta_atom_of(a), CETTA_MAP_5(__VA_ARGS__)
#define CETTA_MAP_7(a, ...)  cetta_atom_of(a), CETTA_MAP_6(__VA_ARGS__)
#define CETTA_MAP_8(a, ...)  cetta_atom_of(a), CETTA_MAP_7(__VA_ARGS__)
#define CETTA_MAP_9(a, ...)  cetta_atom_of(a), CETTA_MAP_8(__VA_ARGS__)
#define CETTA_MAP_10(a, ...) cetta_atom_of(a), CETTA_MAP_9(__VA_ARGS__)
#define CETTA_MAP_11(a, ...) cetta_atom_of(a), CETTA_MAP_10(__VA_ARGS__)
#define CETTA_MAP_12(a, ...) cetta_atom_of(a), CETTA_MAP_11(__VA_ARGS__)
#define CETTA_MAP_13(a, ...) cetta_atom_of(a), CETTA_MAP_12(__VA_ARGS__)
#define CETTA_MAP_14(a, ...) cetta_atom_of(a), CETTA_MAP_13(__VA_ARGS__)
#define CETTA_MAP_15(a, ...) cetta_atom_of(a), CETTA_MAP_14(__VA_ARGS__)
#define CETTA_MAP_16(a, ...) cetta_atom_of(a), CETTA_MAP_15(__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* CETTA_H */
