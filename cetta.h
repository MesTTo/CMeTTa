/* Purpose: drive the PeTTa engine from C. Boot it, build and read MeTTa terms
 *   as C values, run programs, pull answers one at a time, publish C functions
 *   the language calls, bound an evaluation and measure one.
 *
 * Assumes:
 *   - SWI-Prolog 10 with its development headers, threads enabled
 *     [source: /usr/lib/swi-prolog/include/SWI-Prolog.h, PLVERSION 100113]
 *   - C11. _Generic carries the overloads and the argument coercions. Without
 *     it every long-named function still works and the macros do not.
 *   - the engine tree is reachable, either at the path given to mt_open()
 *     or at $PETTA_PATH
 *
 * Guarantees:
 *   - every function that can fail says so, and none of them print, exit or
 *     longjmp; no Prolog exception crosses this header
 *   - an atom is immutable and refcounted, so a term built once may be run
 *     many times and shared between threads without copying
 *   - building and reading atoms starts no engine
 *     [tested: tests/test_cetta.c, test_atoms_need_no_engine; commit=4d20b8d80b2a8eb6fde434e561f30250a35fd3b3]
 *   - mt_eval() computes one answer per step, so a caller that stops
 *     pulling leaves the rest of an infinite stream uncomputed, and
 *     mt_each() closes the cursor on `break` as well as on exhaustion
 *     [tested: tests/test_cetta.c, test_the_walk_closes_its_cursor_on_break;
 *     commit=4d20b8d80b2a8eb6fde434e561f30250a35fd3b3]
 *
 * Owns resources: one Prolog runtime per process, released by mt_close();
 *   one engine per open cursor, released by mt_answers_free(), which
 *   mt_each() calls for you; one malloc'ed block per atom, released when
 *   its last reference goes.
 *
 * Decides, and these six are the whole contract:
 *
 *   1. THE OWNERSHIP LAW, carried by C's own type system. A function taking
 *      `const mt_atom *` BORROWS it and you still own it. A function
 *      taking `mt_atom *` (non-const) TAKES it and you must not drop it
 *      afterwards. Constructors hand you one reference. Accessors hand back
 *      borrowed pointers that live as long as their parent.
 *
 *      Every door you pass a freshly built term to TAKES it, so the common
 *      shape leaks nothing and needs no cleanup line:
 *
 *          mt_add(kb, mt_expr("edge", "a", "b"));
 *
 *      To pass a term you mean to keep, hand over a new reference with
 *      mt_keep(). That is the one thing to remember:
 *
 *          mt_atom *p = mt_expr("edge", "a", mt_var("y"));
 *          while (...) mt_each (row, mt_match(kb, mt_keep(p))) ...
 *          mt_drop(p);
 *
 *   2. ERRORS ARE errno-SHAPED. A function that produces a value returns it,
 *      or NULL, or a documented zero. mt_error() and mt_errmsg() say
 *      what went wrong. Like errno they are SET on failure and NOT cleared on
 *      success, so a run of calls is checked once, where it suits you:
 *
 *          mt_clear();
 *          double x = mt_float(mt_arg(c, 0));
 *          double y = mt_float(mt_arg(c, 1));
 *          if ( !mt_ok() ) return mt_fail(c, "wanted two numbers");
 *
 *      [tested: tests/test_cetta.c, test_the_error_state_is_errno_shaped;
 *      commit=4d20b8d80b2a8eb6fde434e561f30250a35fd3b3]
 *
 *   3. ONE VERB, EITHER RECEIVER. mt_eval, mt_match, mt_atoms,
 *      mt_add, mt_del, mt_count and mt_wipe each take a `metta *`,
 *      meaning its &self, or a `mt_space *`. _Generic picks; the pair it
 *      picks between is declared above each macro for anyone who wants it.
 *      [tested: tests/test_cetta.c, test_one_verb_takes_either_receiver;
 *      commit=4d20b8d80b2a8eb6fde434e561f30250a35fd3b3]
 *
 *   4. A MeTTa Number splits into MT_INT and MT_FLOAT, because C has two
 *      types where the wire codec has one tag and MeTTa tells 2 from 2.0
 *      apart. Values outside int64 and rationals get their own kinds rather
 *      than being rounded into one that fits.
 *
 *   5. READING PROMOTES WHERE IT IS LOSSLESS AND REFUSES WHERE IT IS NOT.
 *      mt_float() of an Int answers that integer, because the conversion
 *      loses nothing below 2^53 and is refused above it. mt_int() of a
 *      Float does NOT round. This is the promotion-lattice reading upstream
 *      Hyperon's own bridging note argues for: "if a promotion path for a
 *      value exists to get to the requested Inner Type, then the accessor
 *      seamlessly works. If a promotion path does not exist then the accessor
 *      will fail."
 *      [tested: tests/test_cetta.c,
 *      test_reading_promotes_only_where_it_is_lossless; commit=4d20b8d80b2a8eb6fde434e561f30250a35fd3b3]
 *
 *   6. A BARE C STRING IN TERM POSITION IS A SYMBOL. mt_expr("+", 1, 2) is
 *      (+ 1 2), not ("+" 1 2). MeTTa source writes a symbol bare and a string
 *      quoted; in C everything is quoted, so the default is the one MeTTa
 *      writes bare. Text is mt_text("..."), which is never ambiguous.
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

#ifndef MT_H
#define MT_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MT_API
#define MT_API extern
#endif

/* C11 or nothing. _Generic carries mt_expr's coercions and the receiver
   dispatch, and without it every macro here expands to a diagnostic about
   something else entirely. Saying so once beats a hundred lines of that. */
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 201112L
#error "cetta.h needs C11: _Generic carries the argument coercions and the \
receiver dispatch. Compile with -std=c11 or later."
#endif

/* Ignoring a returned resource is the leak this library can most easily be
   made to commit, and a compiler that knows will say so.
   The GNU spelling and not C23's [[nodiscard]], even where C23 is available:
   an attribute in [[ ]] form must lead the declaration, where __attribute__
   may follow the storage class, and this header writes `MT_API MT_MUST_USE
   type name(...)`. One spelling that works in every position beats two that
   need the macro to move. */
#if defined(__GNUC__) || defined(__clang__)
#define MT_MUST_USE __attribute__((warn_unused_result))
#else
#define MT_MUST_USE
#endif

/* ================================================================== *
 * Status
 * ================================================================== */

/* MT_ROW and MT_DONE are answers rather than problems: they are how a
   cursor reports progress, the split sqlite3_step() established. */
typedef enum mt_status {
  MT_OK = 0,          /* the call did what it said                      */
  MT_ROW = 1,         /* a cursor produced an answer                    */
  MT_DONE = 2,        /* a cursor is exhausted                          */
  MT_FAIL = 3,        /* the engine had no answer; not an error         */
  MT_ERROR = 4,       /* the engine raised                              */
  MT_NOMEM = 5,       /* allocation failed                              */
  MT_MISUSE = 6,      /* this library's contract was broken             */
  MT_UNSUPPORTED = 7, /* a real value C has no type for, refused by name */
  MT_LIMIT = 8        /* a bound stopped it; you did that, it did not break */
} mt_status;

/* The last failure on THIS thread. Set on failure, NOT cleared on success,
   exactly as errno is, so a run of calls can be checked once at the end. */
MT_API mt_status mt_error(void);

/* Its words, or NULL if nothing has failed since the last mt_clear(). The
   text is owned by the library and overwritten by the next failure here. */
MT_API const char *mt_errmsg(void);

/* Whether nothing has failed on this thread since the last mt_clear(). */
MT_API bool mt_ok(void);

/* Forget the last failure. Call this before a run you intend to check. */
MT_API void mt_clear(void);

/* A stable English name for a status, for your own diagnostics. */
MT_API const char *mt_status_str(mt_status status);

MT_API const char *mt_version(void);

/* ================================================================== *
 * Atoms
 * ================================================================== */

typedef struct mt_atom mt_atom;

/* The nine wire tags of CODEC.md, with the one tag C splits four ways. */
typedef enum mt_kind {
  MT_NONE = -1,/* not an atom; what mt_kind_of(NULL) answers       */
  MT_SYMBOL,   /* `s`: a name that denotes itself                    */
  MT_TEXT,     /* `g`: a grounded value carried as text              */
  MT_INT,      /* `n`: an exact integer that fits int64_t            */
  MT_FLOAT,    /* `n`: a float                                       */
  MT_BIGINT,   /* `n`: an exact integer too wide for int64_t         */
  MT_RATIONAL, /* `n`: an exact ratio                                */
  MT_BOOL,     /* `b`: True or False, which are not symbols          */
  MT_VARIABLE, /* `v`: a variable, its name an identity in its term  */
  MT_EXPR,     /* `e`: an expression; the empty one is unit          */
  MT_SPACE,    /* `p`: an executable space reference                 */
  MT_OBJECT,   /* `o`: a live C value crossing by reference          */
  MT_HANDLE    /* `h`: a native engine value held by reference       */
} mt_kind;

MT_API const char *mt_kind_str(mt_kind kind);

/* --- building. None of these start the engine. --- */

MT_API MT_MUST_USE mt_atom *mt_sym(const char *name);
MT_API MT_MUST_USE mt_atom *mt_var(const char *name);
MT_API MT_MUST_USE mt_atom *mt_text(const char *text);
MT_API MT_MUST_USE mt_atom *mt_textn(const char *text, size_t length);
MT_API MT_MUST_USE mt_atom *mt_num(int64_t value);
MT_API MT_MUST_USE mt_atom *mt_real(double value);
MT_API MT_MUST_USE mt_atom *mt_bool(bool value);
MT_API MT_MUST_USE mt_atom *mt_unit(void);

/* An exact integer wider than int64_t, as decimal digits with an optional
   leading minus. NULL on any other spelling. */
MT_API MT_MUST_USE mt_atom *mt_bigint(const char *decimal);

/* An exact ratio. A zero denominator is refused. Read one back with
   mt_ratio_of(), which answers the pair. */
MT_API MT_MUST_USE mt_atom *mt_rational(int64_t numerator, int64_t denominator);

/* A space reference by its portable engine name, which begins with '&'. */
MT_API MT_MUST_USE mt_atom *mt_spaceref(const char *name);

/* An expression from an array. The children are TAKEN; the array is not. */
MT_API MT_MUST_USE mt_atom *mt_exprv(size_t count, mt_atom **children);

/* The widened forms mt_atom_of dispatches to. Call mt_num or mt_real
   directly rather than these. */
MT_API mt_atom *mt_num_(long long value);
MT_API mt_atom *mt_real_(long double value);
MT_API mt_atom *mt_same(mt_atom *atom);
MT_API mt_atom *mt_same_c(const mt_atom *atom);

/* Turn one C value into an atom: an integer becomes a Number, a float a
   Number, a bare string a SYMBOL (decision 6), and an atom itself.

   The `1 ? (x) : (x)` is what makes a string literal work. _Generic does not
   decay an array, so `char[4]` would match no branch; a conditional
   expression decays both of its operands, which is the one spelling that
   also survives `mt_atom *` being a pointer to an INCOMPLETE type. `(x)+0`
   reads more simply and is what this used first, but it is arithmetic, and
   arithmetic on a pointer to an incomplete type does not compile.

   The conditional applies the usual arithmetic conversions, so a C `bool` and
   a C `char` both arrive as `int`: `true` builds the Number 1 and 'x' builds
   120. C conflates those and this cannot un-conflate them; use mt_bool()
   and mt_text() when you mean those. */
#define mt_atom_of(x) _Generic(1 ? (x) : (x),                             \
    char *:              mt_sym,       const char *:       mt_sym,     \
    signed char:         mt_num_,      unsigned char:      mt_num_,    \
    short:               mt_num_,      unsigned short:     mt_num_,    \
    int:                 mt_num_,      unsigned:           mt_num_,    \
    long:                mt_num_,      unsigned long:      mt_num_,    \
    long long:           mt_num_,      unsigned long long: mt_num_,    \
    float:               mt_real_,     double:             mt_real_,   \
    long double:         mt_real_,                                        \
    mt_atom *:        mt_same,      const mt_atom *: mt_same_c)(x)

/* An expression, with no count to keep in step and every child coerced:

       mt_expr("+", 1, 2)                       (+ 1 2)
       mt_expr("edge", "a", mt_var("y"))     (edge a $y)

   Children are TAKEN. If any is NULL the whole call fails, drops the ones it
   was given and returns NULL, so a failed inner constructor cannot leak
   through an outer one. Sixteen children is the ceiling; wider uses
   mt_exprv().
   [tested: tests/test_cetta.c,
   test_the_builder_coerces_each_child_by_its_c_type; commit=4d20b8d80b2a8eb6fde434e561f30250a35fd3b3] */
#define mt_expr(...)                                                      \
    mt_exprv(MT_NARG(__VA_ARGS__),                                     \
                (mt_atom *[]){ MT_MAP(__VA_ARGS__) })

/* --- lifetime --- */

/* Take a reference. Returns its argument, so it composes inline. NULL-safe. */
MT_API mt_atom *mt_keep(const mt_atom *atom);

/* Drop a reference. NULL-safe. */
MT_API void mt_drop(const mt_atom *atom);

/* --- reading. Each returns the value the way atoi() and strlen() do, and
       records a failure you can check with mt_ok(). --- */

MT_API mt_kind mt_kind_of(const mt_atom *atom);

/* The name of a SYMBOL, VARIABLE or SPACE, the text of a TEXT, the digits of
   a BIGINT. NULL for every other kind. Borrowed. */
MT_API const char *mt_name(const mt_atom *atom);
MT_API size_t mt_name_len(const mt_atom *atom);

/* An exact integer. INT only: a Float is not rounded here, and a BigInt does
   not fit by definition. 0 and a recorded failure otherwise. */
MT_API int64_t mt_int(const mt_atom *atom);

/* A double. Promotes losslessly (decision 5): a Float is itself, an Int of
   magnitude below 2^53 is exact, a Rational is its quotient. An Int above
   2^53 and a BigInt are REFUSED rather than rounded. 0.0 and a recorded
   failure otherwise. */
MT_API double mt_float(const mt_atom *atom);

MT_API bool mt_truth(const mt_atom *atom);
/* A ratio is a pair, so it comes back as one rather than through two
   out-parameters. `den` is 0 when the atom is not a Rational, which is a value
   no ratio has, and the failure is recorded either way. */
typedef struct mt_ratio {
  int64_t num;
  int64_t den;
} mt_ratio;

MT_API mt_ratio mt_ratio_of(const mt_atom *atom);

/* Child count of an EXPR, 0 otherwise. */
MT_API size_t mt_len(const mt_atom *atom);

/* Child `index`, BORROWED and valid while its parent lives. NULL past the end
   or on a non-expression. mt_keep() it to hold it longer. */
MT_API const mt_atom *mt_at(const mt_atom *atom, size_t index);

/* Structural equality. Two variables are equal when their names are. */
MT_API bool mt_eq(const mt_atom *a, const mt_atom *b);

/* --- text, through the engine's own reader and writer --- */

/* Read one MeTTa form. The engine's reader is the only reader. */
MT_API MT_MUST_USE mt_atom *mt_parse(const char *source);

/* Write an atom the way the engine writes it, into a per-thread rotating
   buffer so it drops straight into printf:

       printf("%s -> %s\n", mt_show(pattern), mt_show(answer));

   The buffer is reused after MT_SHOW_SLOTS further calls on this thread,
   which is the contract strerror() and inet_ntoa() already gave C. Take a
   copy with mt_show_dup() to keep it, and free that with mt_free(). */
#define MT_SHOW_SLOTS 8
MT_API const char *mt_show(const mt_atom *atom);
MT_API MT_MUST_USE char *mt_show_dup(const mt_atom *atom);

/* Free anything this library handed back by pointer that is not an atom. */
MT_API void mt_free(void *pointer);

/* ================================================================== *
 * The runtime
 * ================================================================== */

typedef struct metta metta;
typedef struct mt_space mt_space;
typedef struct mt_answers mt_answers;

typedef struct mt_config {
  const char *path;      /* engine tree; NULL takes $PETTA_PATH then the
                            tree this library was built beside          */
  size_t stack_limit;    /* bytes; 0 takes the engine's own default      */
  bool verbose;          /* let the engine print each compiled form      */
} mt_config;

/* Boot the engine. `config` may be NULL for every default. NULL on failure.

   One runtime per process: PL_initialise() sets up the process's single
   Prolog heap, so a second mt_open() with a matching configuration hands
   back the same runtime and one with a different path fails. */
MT_API MT_MUST_USE metta *mt_open(const mt_config *config);

/* Shut the runtime down. Atoms outlive it: they are C memory and stay valid
   until their own references go. */
MT_API void mt_close(metta *runtime);

/* Whether the engine prints compiled forms. Returns the previous setting. */
MT_API bool mt_verbose(metta *runtime, bool verbose);

/* A thread other than the one that opened the runtime attaches before it
   touches the engine and detaches before it exits. Building and reading atoms
   needs neither. */
MT_API bool mt_thread_attach(void);
MT_API void mt_thread_detach(void);

/* &self and &petta, borrowed and living as long as the runtime. */
MT_API mt_space *mt_self(metta *runtime);
MT_API mt_space *mt_catalog(metta *runtime);

/* Create or open a space by name; names begin with '&'. NULL on failure. */
MT_API MT_MUST_USE mt_space *mt_space_open(metta *runtime, const char *name);
MT_API void mt_space_close(mt_space *space);
MT_API const char *mt_space_name(const mt_space *space);

/* ================================================================== *
 * Asking
 * ================================================================== */

/* Run MeTTa source in &self. Every `!` form contributes a group of answers in
   source order, and a row's `group` field says which one it came from.

   Eager: the engine's run door computes the whole program before the first
   answer, because that is what running a program means. mt_eval() is the
   lazy door. NULL on failure. */
MT_API MT_MUST_USE mt_answers *mt_run(metta *runtime, const char *source);

/* Load a file through the same door `import!` uses, so a reload replaces the
   first load's definitions rather than doubling them. */
MT_API MT_MUST_USE mt_answers *mt_load(metta *runtime, const char *path);

/* ------------------------------------------------------------------ *
 * Lowering: C source becoming MeTTa
 * ------------------------------------------------------------------ */

/* The MeTTa text of a token sequence the C compiler saw. Two levels, so the
   argument is macro-expanded before it is stringified: that is what lets a
   body assembled by other macros arrive here already expanded. */
#define MT_METTA(tokens)  MT_METTA_(tokens)
#define MT_METTA_(tokens) #tokens

/* Install an equation written as C TOKENS rather than as a string:

       mt_lower(m, (twice $x), (* 2 $x));
       mt_lower(m, (fib $n), (if (< $n 2) $n
                                 (+ (fib (- $n 1)) (fib (- $n 2)))));

   This is LOWERING, and it is a different thing from mt_def(). A published C
   function is OPAQUE to the engine, which is why it must declare an effect
   class: nothing can be seen of what it does. An equation is MeTTa, so the
   engine reads it, type-checks it, specialises it and reasons about it, and a
   call costs no host crossing at all.

   The preprocessor is what makes this possible. The Python seat lowers by
   reading a function's __code__ and the Node seat by reading its
   toString(); C has neither at run time, but `#` is compile-time access to
   the program's own source, which is the same capability at the only moment C
   offers it. No quoting, no escaped newlines, and the tokens are checked for
   balanced parentheses by the compiler before the engine ever sees them.

   `$x` tokenizes because GCC and Clang admit `$` in an identifier. That is an
   extension rather than ISO C, so a compiler without it needs the string
   form, mt_do(m, "(= (twice $x) (* 2 $x))"), which is what this expands to.

   ONE BODY, BOTH LANGUAGES. Parameterise the body by its operators and it
   expands to C in one mode and to MeTTa in the other, so a function exists
   once and is callable from both:

       #define POLY(ADD, MUL, x)  ADD(MUL(3, x), 1)
       #define C_ADD(a, b)        ((a) + (b))
       #define C_MUL(a, b)        ((a) * (b))
       #define M_ADD(a, b)        (+ a b)
       #define M_MUL(a, b)        (* a b)

       int64_t poly(int64_t x) { return POLY(C_ADD, C_MUL, x); }
       mt_lower(m, (poly $x), POLY(M_ADD, M_MUL, $x));

   which installs `(= (poly $x) (+ (* 3 $x) 1))` and leaves poly() callable
   from C. The engine gets an equation it can see into; C gets a function with
   no crossing. That is what the other seats' twins buy, bought the way C
   buys things. */
#define mt_lower(runtime, head, body)                                     \
    mt_do((runtime), "(= " MT_METTA(head) " " MT_METTA(body) ")")

/* Run source for its EFFECT and discard the answers: definitions, imports,
   pragmas, anything whose point is what it leaves behind rather than what it
   answers. True when it ran.

       mt_do(m, "(= (double $x) (* 2 $x))");

   The alternative is mt_answers_free(mt_run(...)), which says the same thing
   with the reader's attention on the free rather than on the program. */
MT_API bool mt_do(metta *runtime, const char *source);

/* The pairs the verbs below dispatch between. Call these directly if you
   would rather not go through _Generic. Each TAKES its atom argument. */
MT_API MT_MUST_USE mt_answers *mt_self_eval(metta *runtime, mt_atom *goal);
MT_API MT_MUST_USE mt_answers *mt_space_eval(mt_space *space, mt_atom *goal);
MT_API MT_MUST_USE mt_answers *mt_self_match(metta *runtime, mt_atom *pattern);
MT_API MT_MUST_USE mt_answers *mt_space_match(mt_space *space, mt_atom *pattern);
MT_API MT_MUST_USE mt_answers *mt_self_atoms(metta *runtime);
MT_API MT_MUST_USE mt_answers *mt_space_atoms(mt_space *space);
MT_API bool mt_self_add(metta *runtime, mt_atom *atom);
MT_API bool mt_space_add(mt_space *space, mt_atom *atom);
MT_API bool mt_self_del(metta *runtime, mt_atom *atom);
MT_API bool mt_space_del(mt_space *space, mt_atom *atom);
MT_API size_t mt_self_count(metta *runtime);
MT_API size_t mt_space_count(mt_space *space);
MT_API bool mt_self_wipe(metta *runtime);
MT_API bool mt_space_wipe(mt_space *space);

/* Only the selected branch is called; the others are just function names, so
   each one type-checks against its own receiver. This is how tgmath.h works. */
#define MT_ON(target, verb) _Generic((target),                            \
    metta *:        mt_self_##verb,                                       \
    mt_space *:  mt_space_##verb)

/* Evaluate one atom LAZILY: each step computes at most one answer, and
   abandoning the cursor leaves the rest uncomputed. TAKES `goal`. */
#define mt_eval(target, goal)   MT_ON((target), eval)((target), (goal))

/* Stored atoms unifying a pattern, lazily. TAKES `pattern`. */
#define mt_match(target, pat)   MT_ON((target), match)((target), (pat))

/* Every stored atom, lazily. */
#define mt_atoms(target)        MT_ON((target), atoms)((target))

/* Add one atom. TAKES it. */
#define mt_add(target, atom)    MT_ON((target), add)((target), (atom))

/* Remove one exact atom; true when it was there. TAKES it. */
#define mt_del(target, atom)    MT_ON((target), del)((target), (atom))

/* How many atoms are stored. */
#define mt_count(target)        MT_ON((target), count)((target))

/* Empty it. */
#define mt_wipe(target)         MT_ON((target), wipe)((target))

/* --- reading answers --- */

/* One answer, as a record rather than four questions put to the cursor.
   BORROWED: it belongs to the cursor and every field is refreshed by the next
   step, so keep an atom with mt_keep() and text with mt_show_dup(). */
typedef struct mt_row {
  const mt_atom *atom;   /* the answer itself                              */
  const char    *text;   /* the engine's own rendering of it, which can show
                            a value mt_show() refuses: a host-only value or
                            a non-finite float                             */
  size_t         group;  /* which `!` form produced it, counting from 0;
                            always 0 for the lazy doors, which run one goal */
  mt_answers    *of;     /* the cursor it came from, which is what lets
                            mt_bound() take the row and not the cursor     */
} mt_row;

/* The next ANSWER, or NULL at the end. NULL is also what a failure gives, and
   mt_ok() tells the two apart. This is what mt_each() calls, and it is the
   short form because most loops want the answer and nothing else. */
MT_API const mt_atom *mt_next(mt_answers *answers);

/* The next answer as a ROW: the same step, reported in full. A pointer rather
   than a value because the row lives IN the cursor, so returning it by value
   would copy four fields per answer and a cursor walked two million times
   notices. This is what mt_rows() calls.

   The split is the Python seat's. There, `Answers` iterates atoms and `Rows`
   iterates a `Row` whose fields are the query's variable names, because a
   result you match for and a result you evaluate for are different questions.
   Making every walk carry a row would charge the common one for the other. */
MT_API const mt_row *mt_row_next(mt_answers *answers);

/* What the pattern's `$name` is bound to in the CURRENT answer, BORROWED and
   valid until the next step. This is what saves you counting children:

       mt_rows (row, mt_match(kb, E("edge", "a", V("y"))))
           printf("y = %s\n", mt_show(mt_bound(row, "y")));

   rather than mt_at(row->atom, 2) and a comment explaining why 2. The cursor keeps
   the pattern it was opened with and lines it up against each answer, so this
   costs one walk of the term and no engine call. NULL when the cursor has no
   pattern, when no `$name` is in it, or when that position did not bind.

   Only a MATCH cursor has a pattern, so this answers NULL on one from
   mt_eval(): a match answer is an INSTANCE of the pattern and lines up with it
   position for position, where an eval answer is a reduced value that shares
   no shape with the goal. Reading one against the other would find a subterm
   at the same index and call it a binding, which is a wrong answer rather than
   a missing one.

   The Python seat spells the same thing `row.y`, and MeTTa's own answer frames
   carry it as theta, name-to-term pairs against the caller's variables. */
MT_API const mt_atom *mt_bound(const mt_row *row, const char *name);

/* Release the cursor and, for a lazy one, the engine behind it. NULL-safe.
   mt_each() does this for you. */
MT_API void mt_answers_free(mt_answers *answers);

/* The first answer, OWNED, with the cursor closed and the rest left
   uncomputed. NULL when there is none. CONSUMES `answers`:

       mt_atom *a = mt_first(mt_eval(m, mt_expr("+", 1, 2)));
       ...
       mt_drop(a);

   The atom is yours, so it is yours to drop. When all you want is the VALUE,
   the four below do that without an atom ever landing in your hands. */
MT_API MT_MUST_USE mt_atom *mt_first(mt_answers *answers);

/* EXACTLY one answer, OWNED, or NULL with a failure recorded when there were
   none or more than one. The Python seat draws the same line between one()
   and first(), and the word means the same thing here: `one` is a claim about
   the cardinality and `first` is not. CONSUMES `answers`
   [tested: tests/test_cetta.c, test_one_and_first_make_different_claims;
   commit=4d20b8d80b2a8eb6fde434e561f30250a35fd3b3]. */
MT_API MT_MUST_USE mt_atom *mt_one(mt_answers *answers);

/* Ask for exactly one answer and read it as a C value: the cursor is closed,
   the atom is released, and the whole question is one expression.

       printf("%lld\n", (long long)mt_one_int(mt_eval(m, E("+", 1, 2))));

   Each CONSUMES `answers` and carries mt_one()'s cardinality claim, so a
   question that answered twice is a recorded failure rather than a silent
   first. Each records a failure, so mt_ok() tells "no answer" and "wrong
   kind" apart from a real zero. mt_one_name() borrows the same per-thread
   ring mt_show() uses. */
MT_API int64_t mt_one_int(mt_answers *answers);
MT_API double mt_one_float(mt_answers *answers);
MT_API bool mt_one_truth(mt_answers *answers);
MT_API const char *mt_one_name(mt_answers *answers);

/* An owned array and its length, which are one thing and so travel as one.
   The alternative is an out-parameter for the count and a loop bound the
   caller has to keep in step with it by hand. */
typedef struct mt_list {
  mt_atom **items;
  size_t    len;
} mt_list;

/* Every answer in order, the eager door for a caller who wants them all.
   CONSUMES `answers`. An empty or failed call answers {NULL, 0}, which loops
   zero times, so a caller need not test it before walking:

       mt_list all = mt_all(mt_eval(m, goal));
       for (size_t i = 0; i < all.len; i++) puts(mt_show(all.items[i]));
       mt_list_free(all);                                                  */
MT_API MT_MUST_USE mt_list mt_all(mt_answers *answers);

/* Drops every atom and frees the array. Safe on {NULL, 0}. */
MT_API void mt_list_free(mt_list list);

/* Walk every answer and close the cursor, however the loop is left. The row
   carries everything there is to know about one answer, so there is one walk
   and not two:

       mt_each (a, mt_run(m, "!(superpose (1 2 3))"))
           printf("%s\n", mt_show(a));

   `break` is safe and closes the cursor. `return` and `goto` out of the body
   are NOT: they leave without running the loop's increment, so free it by
   hand there, or use MT_AUTO_ASK below. */
#define mt_each(atom, answers)                                            \
    MT_WALK_(atom, answers, MT_ID(mt_it_), const mt_atom *, mt_next)

/* The same walk reported in full, for a query rather than an evaluation:

       mt_rows (row, mt_match(kb, E("edge", "a", V("y"))))
           printf("group %zu: y = %s\n",
                  row->group, mt_show(mt_bound(row, "y")));

   Named for what it binds, the way mt_each is. The Python seat draws the same
   line between iterating `Answers` and iterating `Rows`. */
#define mt_rows(row, answers)                                             \
    MT_WALK_(row, answers, MT_ID(mt_it_), const mt_row *, mt_row_next)

/* The cursor's name is generated ONCE, by the caller above, and passed in as
   a parameter. Generating it at each mention would give three different names
   because __COUNTER__ increments every time it is read. */
#define MT_WALK_(var, answers, it, type, step)                            \
  for (mt_answers *it = (answers); it != NULL;                            \
       mt_answers_free(it), it = NULL)                                    \
    for (type var; (var = step(it)) != NULL; )

/* ================================================================== *
 * Publishing C functions to MeTTa
 * ================================================================== */

/* The five ranked effect classes. Naming one is required, not advisory: the
   engine reasons about caching, reordering and transactions from it, and a
   wrong answer here is a wrong program. */
typedef enum mt_effect {
  MT_PURE,      /* same answer always, reads nothing, writes nothing */
  MT_LOOKUP,    /* reads state, writes none                          */
  MT_NONDET,    /* reads, and may answer differently                 */
  MT_WRITES,    /* changes something                                 */
  MT_IO         /* reaches the world                                 */
} mt_effect;

MT_API const char *mt_effect_str(mt_effect effect);

typedef struct mt_call mt_call;

/* Answer MT_OK having called mt_answer(), MT_FAIL to say there is no
   answer for these arguments, or return mt_fail() to refuse with words. */
typedef mt_status (*mt_fn)(mt_call *call, void *user);

/* One published function. Designated initializers make the call site name
   what it is passing, which is C's answer to keyword arguments:

       mt_def(m, (mt_op){ .name = "hypot", .arity = 2,
                                .effect = MT_PURE, .fn = op_hypot }); */
typedef struct mt_op {
  const char  *name;    /* as written; underscores reach MeTTa as hyphens */
  size_t       arity;
  mt_effect effect;
  mt_fn     fn;
  void        *user;    /* handed back to fn on every application         */
} mt_op;

/* Publish, so `(name a b)` in MeTTa calls it. The name reaches MeTTa through
   C's own casing convention, so `car_atom` publishes `car-atom`; a name
   outside C's identifier grammar crosses untouched, which is the escape for
   `prime?` and `%Undefined%`. */
MT_API bool mt_def(metta *runtime, mt_op op);

/* Withdraw a published function at every arity, giving the name back. */
MT_API bool mt_undef(metta *runtime, const char *name);

/* Inside a published function. */
MT_API size_t mt_arity(const mt_call *call);
MT_API const mt_atom *mt_arg(const mt_call *call, size_t index);
MT_API metta *mt_of(const mt_call *call);

/* Answer with an atom, which is TAKEN. Answering twice is MT_MISUSE; a
   function with many answers answers one expression and lets MeTTa's own
   superpose spread it. Returns MT_OK so it can be the return statement. */
MT_API mt_status mt_answer(mt_call *call, mt_atom *atom);

/* Refuse this application with words the engine reports. Returns MT_ERROR
   so it too can be the return statement. */
MT_API mt_status mt_fail(mt_call *call, const char *message);

/* --- carrying a C value through MeTTa untouched --- */

typedef void (*mt_free_fn)(void *value);

/* Wrap a C pointer as a grounded atom. MeTTa carries it by reference, never
   serialises it, and hands it back unchanged. */
MT_API MT_MUST_USE mt_atom *mt_object(void *value, const char *type_name,
                                   mt_free_fn release);
MT_API void *mt_value(const mt_atom *atom);
MT_API const char *mt_type(const mt_atom *atom);

/* A C function as a VALUE rather than a name, so `($f 2)` calls it wherever
   the atom lands. This is what C answers to a Python callable being an atom;
   mt_def() is the other half, a function reached by its published name. */
MT_API MT_MUST_USE mt_atom *mt_function(mt_fn fn, void *user,
                                     mt_free_fn release);

/* ================================================================== *
 * Bounding and measuring
 * ================================================================== */

/* What an evaluation may spend. Zero on a field means no bound there. A bound
   stops work MID-WAY and writes already made stand, which is the honest
   semantics of every timeout. */
typedef struct mt_limits {
  double   seconds;      /* wall seconds one call, or one step, may take */
  uint64_t inferences;   /* engine steps it may spend                    */
  size_t   stack_bytes;  /* SWI's stack ceiling, which a runaway recursion
                            hits; NOT MeTTa's reduction depth, which is the
                            max-stack-depth pragma in the program text    */
} mt_limits;

/* Bounds for every later call. By value and not by pointer, because a
   compound literal says it at the call site and the zero struct already means
   what a NULL would have:

       mt_limit(m, (mt_limits){ .seconds = 2.0, .inferences = 1000000 });
       mt_limit(m, (mt_limits){0});          -- and that clears them

   On a lazy cursor the inference bound is a CUMULATIVE budget for the whole
   cursor, built INTO the goal the engine runs. It cannot be metered from out
   here: an SWI engine counts its own inferences and this process cannot see
   them, so a bound placed around each step would measure the pull loop.
   Measured 2026-08-28 on the endless generator (= (from $n) (superpose ($n
   (from (+ $n 1))))), budgets of 1,000 / 5,000 / 20,000 / 100,000 stop after
   0 / 86 / 1,404 / 7,118 answers. Answers scaling with the budget is the
   property that matters, and the one a per-step meter cannot produce
   [tested: tests/test_cetta.c, test_a_bound_stops_a_runaway_and_says_so;
   commit=WORKTREE].
   The wall bound applies per step, so time the host spends between steps does
   not count against it. An eager mt_run() is bounded as one call. */
MT_API bool mt_limit(metta *runtime, mt_limits limits);
MT_API mt_limits mt_limits_of(const metta *runtime);

/* The engine's own counters. Inferences are DETERMINISTIC where wall clock is
   not, which is why this tree gates on them. */
typedef struct mt_stats {
  uint64_t inferences;
  double   cputime;
  uint64_t gc_count;
  uint64_t gc_freed;
  double   gc_time;
  uint64_t table_bytes;
} mt_stats;

/* Sample now. Take two and subtract: that is the shape getrusage() gave C and
   it needs no block construct C does not have. */
MT_API mt_stats mt_stats_now(metta *runtime);
MT_API mt_stats mt_stats_since(mt_stats before, mt_stats after);

/* ================================================================== *
 * Scope cleanup, where the compiler has it
 * ================================================================== */

#if defined(__GNUC__) || defined(__clang__)
#define MT_HAS_AUTO 1
static inline void mt_drop_p(mt_atom **p) { mt_drop(*p); }
static inline void mt_answers_free_p(mt_answers **p) { mt_answers_free(*p); }
/* Released when the block is left, however it is left, including by return
   and goto. This is systemd's `_cleanup_` and the Linux kernel's `__free`; it
   is a GCC and Clang extension rather than ISO C, which is why it sits behind
   MT_HAS_AUTO. */
#define MT_AUTO      __attribute__((cleanup(mt_drop_p)))
#define MT_AUTO_ASK  __attribute__((cleanup(mt_answers_free_p)))
/* Hand a resource out of a MT_AUTO variable without it being released. */
/* __extension__ is how GCC and Clang are told that the statement expression
   below is a deliberate extension, so -Wpedantic stays on for everything
   else rather than being turned off for the whole build over one line. */
#define MT_TAKE(p) \
    __extension__ ({ __typeof__(p) mt_taken_ = (p); (p) = NULL; mt_taken_; })
#endif

/* ================================================================== *
 * Shorthand, opt-in
 * ================================================================== */

/* `#define MT_SHORTHAND` before including this header for the one-letter
   builders. Off by default because S, V, T, N, R, B and E are short names in
   C's single flat namespace and a program that already uses one should not
   have it taken. The long names always work. */
#ifdef MT_SHORTHAND
#define S(name)   mt_sym(name)
#define V(name)   mt_var(name)
#define T(text)   mt_text(text)
#define N(value)  mt_num(value)
#define R(value)  mt_real(value)
#define B(value)  mt_bool(value)
#define E(...)    mt_expr(__VA_ARGS__)
#endif

/* ================================================================== *
 * Macro machinery
 * ================================================================== */

#define MT_CAT_(a, b) a##b
#define MT_CAT(a, b) MT_CAT_(a, b)

/* mt_each() needs one name it can both declare and refer to three times.
   __COUNTER__ would give a different name at each mention, so the counter is
   bumped once per loop and MT_ID_LAST names that same variable again. */
#ifdef __COUNTER__
#define MT_ID(base)  MT_CAT(base, __COUNTER__)
#else
/* Without __COUNTER__ two mt_each() loops on ONE source line would collide.
   Nesting across lines is still fine. */
#define MT_ID(base)  MT_CAT(base, __LINE__)
#endif

/* Count the arguments, so no call site carries a length that can drift out of
   step with the list beside it. */
#define MT_NARG(...) MT_NARG_(__VA_ARGS__, 16,15,14,13,12,11,10,9,      \
                                    8,7,6,5,4,3,2,1,0)
#define MT_NARG_(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16,  \
                    N,...) N

/* Apply mt_atom_of to each argument. */
#define MT_MAP(...) MT_CAT(MT_MAP_, MT_NARG(__VA_ARGS__))(__VA_ARGS__)
#define MT_MAP_1(a)       mt_atom_of(a)
#define MT_MAP_2(a, ...)  mt_atom_of(a), MT_MAP_1(__VA_ARGS__)
#define MT_MAP_3(a, ...)  mt_atom_of(a), MT_MAP_2(__VA_ARGS__)
#define MT_MAP_4(a, ...)  mt_atom_of(a), MT_MAP_3(__VA_ARGS__)
#define MT_MAP_5(a, ...)  mt_atom_of(a), MT_MAP_4(__VA_ARGS__)
#define MT_MAP_6(a, ...)  mt_atom_of(a), MT_MAP_5(__VA_ARGS__)
#define MT_MAP_7(a, ...)  mt_atom_of(a), MT_MAP_6(__VA_ARGS__)
#define MT_MAP_8(a, ...)  mt_atom_of(a), MT_MAP_7(__VA_ARGS__)
#define MT_MAP_9(a, ...)  mt_atom_of(a), MT_MAP_8(__VA_ARGS__)
#define MT_MAP_10(a, ...) mt_atom_of(a), MT_MAP_9(__VA_ARGS__)
#define MT_MAP_11(a, ...) mt_atom_of(a), MT_MAP_10(__VA_ARGS__)
#define MT_MAP_12(a, ...) mt_atom_of(a), MT_MAP_11(__VA_ARGS__)
#define MT_MAP_13(a, ...) mt_atom_of(a), MT_MAP_12(__VA_ARGS__)
#define MT_MAP_14(a, ...) mt_atom_of(a), MT_MAP_13(__VA_ARGS__)
#define MT_MAP_15(a, ...) mt_atom_of(a), MT_MAP_14(__VA_ARGS__)
#define MT_MAP_16(a, ...) mt_atom_of(a), MT_MAP_15(__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* MT_H */
