/* Purpose: drive the MeTTa engine from C. Boot it, build and read MeTTa terms
 *   as C values, run programs, pull answers one at a time, publish C functions
 *   the language calls, bound an evaluation and measure one.
 *
 * Assumes:
 *   - SWI-Prolog 10 with its development headers, threads enabled
 *     [source: /usr/lib/swi-prolog/include/SWI-Prolog.h, PLVERSION 100114;
 *     commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c]
 *   - C11. _Generic carries receiver dispatch and argument coercions.
 *   - the engine tree is reachable, either at the path given to mt_open()
 *     or at $METTA_PATH
 *
 * Guarantees:
 *   - every function that can fail says so, and none of them print, exit or
 *     longjmp; no Prolog exception crosses this header
 *   - a door called before mt_open(), or after mt_close(), REFUSES with
 *     MT_MISUSE naming mt_open(). It is the first mistake a new caller makes
 *     and SWI's foreign-frame API answers it with a signal, which is worse
 *     than all three of the above
 *     [tested: tests/test_cmetta.c, test_a_door_before_the_runtime_refuses;
 *     commit=c530ccb8fb7d0a5b2aa53df6e9f981ada9f81be8]
 *   - NULL is what a failed mt_open(), mt_space_open() or constructor hands
 *     back, so every door refuses one rather than reading through it
 *     [tested: tests/test_cmetta.c, test_a_door_that_takes_an_atom_refuses_null;
 *     commit=c530ccb8fb7d0a5b2aa53df6e9f981ada9f81be8]
 *   - term walks are iterative. Building, reading, comparing and writing
 *     use explicit stacks; release links dead nodes with O(1) auxiliary space
 *     [tested: tests/test_cmetta.c, test_a_deep_term_does_not_overrun_the_stack,
 *     tests/test_ownership.c; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c]
 *   - an atom is immutable and refcounted, so a term built once may be run
 *     many times and shared between threads without copying
 *   - an answer keeps the engine's variable identity: every occurrence of one
 *     engine variable decodes to one name, its source name where the answer's
 *     name state carries one and otherwise a fresh `_N` from a process-wide
 *     counter, so two answers never share a variable and an equation read
 *     back through C still computes where it is copied [tested:
 *     tests/test_cmetta.c, test_an_answer_keeps_variable_identity;
 *     commit=cce10b38ae45bb4c7b9f61aad51f4570aa217547]
 *   - building and reading atoms starts no engine
 *     [tested: tests/test_cmetta.c, test_atoms_need_no_engine; commit=4d20b8d80b2a8eb6fde434e561f30250a35fd3b3]
 *   - outside a closed transaction, mt_eval() computes one answer per step, so a caller that stops
 *     pulling leaves the rest of an infinite stream uncomputed, and
 *     mt_each() closes the cursor on `break` as well as on exhaustion.
 *     Inside a transaction, the engine collects into its held-cursor service:
 *     commit retains answers and rollback discards them. SWI cannot yield
 *     across that closed goal [tested: tests/test_transactions.c;
 *     commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c].
 *
 * Owns resources: one Prolog runtime per process, shut down through mt_close();
 *   one engine, held result, or native iterator per cursor, released by
 *   mt_answers_free(); C atoms and buffers carry their allocating callback
 *   until released. mt_each() closes on break and exhaustion.
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
 *      [tested: tests/test_cmetta.c, test_the_error_state_is_errno_shaped;
 *      commit=4d20b8d80b2a8eb6fde434e561f30250a35fd3b3]
 *
 *      A FAILURE IS RECORDED; AN ANSWER IS NOT, and the line between them is
 *      whether the value could also be a real answer. mt_int(), mt_float(),
 *      mt_truth() and mt_ratio_of() record, because 0, 0.0, false and a
 *      denominator are values a real atom could carry. mt_kind_of(),
 *      mt_name(), mt_len() and mt_at() do not, because they are how you
 *      CLASSIFY an atom before reading it and a walk over a leaf must not arm
 *      an error the caller then reads back. Every constructor and every door
 *      records, including the ones that answer NULL
 *      [tested: tests/test_cmetta.c, test_a_failed_constructor_says_so;
 *      commit=c530ccb8fb7d0a5b2aa53df6e9f981ada9f81be8].
 *
 *   3. ONE VERB, EITHER RECEIVER. mt_eval, mt_match, mt_atoms,
 *      mt_run, mt_load, mt_do, mt_add, mt_del, mt_count and mt_wipe each take a `metta *`,
 *      meaning its &self, or a `mt_space *`. _Generic picks; the pair it
 *      picks between is declared above each macro for anyone who wants it.
 *      [tested: tests/test_cmetta.c, test_one_verb_takes_either_receiver;
 *      commit=4d20b8d80b2a8eb6fde434e561f30250a35fd3b3]
 *
 *   4. A MeTTa Number splits into MT_INT and MT_FLOAT, because C has two
 *      types where the wire codec has one tag and MeTTa tells 2 from 2.0
 *      apart. Values outside int64 and rationals get their own kinds rather
 *      than being rounded into one that fits: an integer too wide for int64
 *      is MT_BIGINT and a ratio with a half too wide is MT_BIGRATIONAL, each
 *      carried as canonical decimal text, the form SWI's own janus binding
 *      crosses a rational in and GMP's mpq_set_str() reads.
 *
 *   5. READING PROMOTES WHERE IT IS LOSSLESS AND REFUSES WHERE IT IS NOT.
 *      mt_float() of an Int answers that integer, because the conversion
 *      loses nothing below 2^53 and is refused above it, and mt_ratio_of() of
 *      an Int answers it over 1 for the same reason. mt_int() of a
 *      Float does NOT round. This is the promotion-lattice reading upstream
 *      Hyperon's own bridging note argues for: "if a promotion path for a
 *      value exists to get to the requested Inner Type, then the accessor
 *      seamlessly works. If a promotion path does not exist then the accessor
 *      will fail."
 *      [tested: tests/test_cmetta.c,
 *      test_reading_promotes_only_where_it_is_lossless; commit=4d20b8d80b2a8eb6fde434e561f30250a35fd3b3]
 *
 *   6. A BARE C STRING IN TERM POSITION IS A SYMBOL. mt_expr("+", 1, 2) is
 *      (+ 1 2), not ("+" 1 2). MeTTa source writes a symbol bare and a string
 *      quoted; in C everything is quoted, so the default is the one MeTTa
 *      writes bare. Text is mt_text("..."), which is never ambiguous.
 *
 * Fails when: the caller needs separate context state in one process. This
 *   API exposes one context; named spaces isolate source and storage but share
 *   registrations. It also refuses to expose frame-scoped SWI term handles.
 *   Nesting is bounded by memory rather than by the C stack, so a term deep
 *   enough to exhaust the heap, or SWI's own stack_limit on the way in and
 *   out, is refused by name rather than crossing.
 *
 * Guarded by: nothing, deliberately. An atom is immutable and its refcount is
 *   atomic, so building, sharing and dropping atoms is safe from any thread,
 *   and the error state is thread-local. Registration tables are not guarded:
 *   register and withdraw while evaluation workers are quiescent. A custom
 *   allocator must support the threads that allocate and release its blocks.
 *
 * Open Obligations:
 *   To Do: None
 *   Hacks: None
 *   Future Enhancements: None
 */

#ifndef MT_H
#define MT_H

#define MT_VERSION "1.0.0"

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
#error "cmetta.h needs C11: _Generic carries the argument coercions and the \
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

/* What to do about the last failure, or NULL where the engine declared no
   repair for it (which every failure of this library's own contract is).

   One line, with the refusal's own parts already in it: a tripped bound reads
   "raise the bound past 0.05 seconds, or narrow the query". It comes from the
   engine's `(refusal ...)` catalog row for the kind the ball was, rendered by
   the engine, which is where the Python and JavaScript seats read the same
   sentence from; nothing here composes prose of its own. Owned by the library
   and overwritten by the next failure on this thread. */
MT_API const char *mt_remedy(void);

/* The authority the last refusal stands on, or NULL where none was declared.

   "metta-law: <the law and where this engine states it>", or "arbiter: <the
   captured upstream answer that settles it>". This is for a developer reading
   why the engine refuses at all, where mt_remedy() is for the program's next
   move. Same lifetime as mt_errmsg(). */
MT_API const char *mt_ground(void);

/* Whether nothing has failed on this thread since the last mt_clear(). */
MT_API bool mt_ok(void);

/* Forget the last failure. Call this before a run you intend to check. */
MT_API void mt_clear(void);
/* Record a callback failure on this thread. Only error statuses are accepted. */
MT_API mt_status mt_error_set(mt_status status, const char *message);

/* A stable English name for a status, for your own diagnostics. */
MT_API const char *mt_status_str(mt_status status);

MT_API const char *mt_version(void);

/* Allocation belongs to the caller. The callback follows realloc: NULL
   allocates, new_size zero frees, and a failed resize preserves the old block.
   Sizes include the private ownership header. Storage must have max_align_t
   alignment. The callback and user must outlive their blocks and support any
   thread that releases them. SWI's own heap remains SWI's responsibility.
   [source: https://github.com/lua/lua/blob/6e22fedb74cf0c9b6656e9fce8b7331db847c605/lmem.c]
   Setting a NULL callback restores libc. Returns this thread's previous
   allocator; existing blocks retain theirs. Callbacks must not call mt_alloc. */
typedef void *(*mt_realloc_fn)(void *user, void *pointer,
                             size_t old_size, size_t new_size);
typedef struct mt_allocator {
  mt_realloc_fn resize;
  void *user;
} mt_allocator;
MT_API mt_allocator mt_allocator_set(mt_allocator allocator);
MT_API MT_MUST_USE void *mt_alloc(size_t size);
MT_API MT_MUST_USE void *mt_calloc(size_t count, size_t size);
MT_API MT_MUST_USE void *mt_resize(void *pointer, size_t size);

/* ================================================================== *
 * Atoms
 * ================================================================== */

typedef struct mt_atom mt_atom;

/* The nine wire tags of CODEC.md, with the one tag C splits five ways. A
   kind added after 1.0.0 goes at the end, so every earlier kind keeps the
   value a program compiled against 1.0.0 switches on. */
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
  MT_HANDLE,   /* `h`: a native engine value held by reference       */
  MT_BIGRATIONAL /* `n`: an exact ratio with a half too wide for int64_t */
} mt_kind;

/* An engine value reaches C in the wire grammar every seat reads, so an
   answer is the same atom here as in the Python and Node seats and compares
   equal to the expression a program builds [source: docs/journal/
   2026-09-05-node-runtime-gaps.md and extensions/python/metta/_binding/
   wire.pl, metta_py_encode/4; commit=b88bfb4ce75e4f37ccda3d99456acb40afddf761].
   A compound the engine hands out, such as a refusal's payload, is the
   expression (F args...), its functor a symbol and a zero-arity one (F); an
   improper list is (cons Head Tail) along its spine; a partial application
   is (partial F Args); a variable keeps its identity; and a cyclic answer is
   refused by name [tested: tests/test_internal_contracts.c,
   test_compounds_decode_in_the_shared_wire_grammar and
   test_a_cyclic_answer_is_refused_by_name; commit=65b02ca599b0db696faf221f4f39e94210013fc1].
   A value read this way goes back as the atom C holds, never as the engine's
   term, so, as in those seats, a partial application passed back is data
   rather than a function that applies, and (id p) on a refusal's payload
   acts on the expression [measured 2026-09-24: the Python seat answers
   ((partial + (1)) 2) for the same hand-back]. Answers and rows (mt_next,
   mt_step, mt_first, mt_one, mt_all, mt_bound), a published function's
   arguments (mt_arg), a matcher's operand, a subscription's notification, a
   parse and an effect plan all read this way. The one door that does not is
   a provider's add, remove and match: a store has to hand the engine back the
   term the engine gave it, so what this grammar would change arrives there
   carried, as described at mt_provider.

   An MT_HANDLE holds an engine value by reference: a native blob, such as a
   host language's object, from any door, and a term a provider carried.
   mt_show() prints it as the engine does and passing it back to any door
   puts the identical value back; mt_write_dup() refuses it, having no source
   spelling. Two blob handles are mt_eq, hash alike and compare equal exactly
   when they hold one blob of one runtime, never merely because they print
   alike [tested: tests/test_internal_contracts.c,
   test_native_handle_decode_and_encode_contract;
   commit=65b02ca599b0db696faf221f4f39e94210013fc1]. Two carried handles are
   mt_eq exactly when their terms are variants whose variables have the same
   names in this crossing, since those names are what a carried term shares
   with the atom around it, and mt_alpha_eq renames them with the rest of the
   atom [tested: tests/test_internal_contracts.c,
   test_a_provider_carries_what_it_stores; commit=256a3a6aa4248e89c7b007edacc6832d62eb4594]. A handle cannot
   outlive the runtime that answered it: after mt_close() passing it back is
   refused by name [tested: tests/test_reopen.c,
   test_a_handle_does_not_outlive_its_runtime;
   commit=65b02ca599b0db696faf221f4f39e94210013fc1]. */

MT_API const char *mt_kind_str(mt_kind kind);

/* Logical text, names and source use UTF-8. Constructors retain bytes without
   starting SWI; transport to the engine refuses malformed encoding. Filenames
   use SWI's platform representation. Lengths always count bytes.
   [tested: tests/test_native_parity.c; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c] */

/* --- building. None of these start the engine. --- */

MT_API MT_MUST_USE mt_atom *mt_sym(const char *name);
MT_API MT_MUST_USE mt_atom *mt_var(const char *name);
MT_API MT_MUST_USE mt_atom *mt_text(const char *text);
MT_API MT_MUST_USE mt_atom *mt_textn(const char *text, size_t length);
/* Borrow immutable, NUL-terminated storage without copying its length bytes.
   On success the atom owns one release(owner) call; on failure ownership stays
   with the caller. NULL release means static or otherwise externally held
   storage. The terminator is required even when the text contains NUL bytes. */
MT_API MT_MUST_USE mt_atom *mt_text_ref(const char *text, size_t length,
                                      void *owner, void (*release)(void *));
MT_API MT_MUST_USE mt_atom *mt_num(int64_t value);
MT_API MT_MUST_USE mt_atom *mt_unum(uint64_t value);
MT_API MT_MUST_USE mt_atom *mt_real(double value);
MT_API MT_MUST_USE mt_atom *mt_bool(bool value);
MT_API MT_MUST_USE mt_atom *mt_unit(void);

/* An exact integer as decimal digits with an optional leading minus.
   Canonicalizes leading zeroes and returns MT_INT when the value fits int64_t,
   otherwise MT_BIGINT. NULL on any other spelling. */
MT_API MT_MUST_USE mt_atom *mt_bigint(const char *decimal);

/* An exact ratio, stored in CANONICAL form: lowest terms, sign on the
   numerator, so mt_rational(1, -2) reads back as -1/2 and mt_rational(2, 4)
   as 1/2. That is what Python's fractions.Fraction does, and more to the
   point it is what the engine does, so a ratio built here is one the engine
   can read: SWI writes -1r2 and its reader refuses 1r-2. A zero denominator
   is refused, and so is the one pair whose canonical form does not fit, a
   denominator of INT64_MIN sharing no factor with its numerator. Read one
   back with mt_ratio_of(), which answers the pair.

   A canonical denominator of 1 answers an INT, because that is the same
   canonicalisation and the engine performs it either way: SWI evaluates
   `3 rdiv 1` to 3, so a whole-number ratio stored in a space came back as an
   Int and mt_eq() then answered false against the atom that had been stored
   [tested: tests/test_cmetta.c, test_a_ratio_is_stored_in_canonical_form,
   test_a_ratio_is_canonical_in_both_halves; commit=c530ccb8fb7d0a5b2aa53df6e9f981ada9f81be8]. */
MT_API MT_MUST_USE mt_atom *mt_rational(int64_t numerator, int64_t denominator);

/* An exact ratio of any width as decimal text, "N/D" with an optional leading
   minus on N, the form mt_name() answers for a BigRational and GMP's
   mpq_get_str() writes. It is canonicalized as mt_rational() canonicalizes, so
   the kind follows the value: a whole ratio is an Int or BigInt, one whose
   halves both fit int64_t is a Rational, and only the rest is a BigRational,
   so mt_bigrational("2/4") is mt_rational(1, 2) and mt_eq() says so. A zero
   denominator and any other spelling are refused; NULL then
   [tested: tests/test_native_parity.c, test_wide_ratios_agree_with_the_engine;
   commit=WORKTREE].
   Time: Theta(D^2) limb operations for D digits, the decimal conversion and
   Stein's gcd both quadratic in the width. */
MT_API MT_MUST_USE mt_atom *mt_bigrational(const char *ratio);

/* A space reference by its portable engine name, which begins with '&'. */
MT_API MT_MUST_USE mt_atom *mt_spaceref(const char *name);

/* An expression from an array. The children are TAKEN; the array is not. */
MT_API MT_MUST_USE mt_atom *mt_exprv(size_t count, mt_atom **children);

/* Borrow an immutable child vector and retain each child. Only the descriptor
   allocates. The vector must live until release(owner), called exactly once on
   successful construction after the last parent reference is dropped. */
MT_API MT_MUST_USE mt_atom *mt_expr_ref(size_t count,
                                      const mt_atom *const *children,
                                      void *owner, void (*release)(void *));

/* The widened forms mt_atom_of dispatches to. Call mt_num or mt_real
   directly rather than these. */
MT_API mt_atom *mt_num_(long long value);
MT_API mt_atom *mt_unum_(unsigned long long value);
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
    int:                 mt_num_,      unsigned:           mt_unum_,   \
    long:                mt_num_,      unsigned long:      mt_unum_,   \
    long long:           mt_num_,      unsigned long long: mt_unum_,   \
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
   [tested: tests/test_cmetta.c,
   test_the_builder_coerces_each_child_by_its_c_type; commit=4d20b8d80b2a8eb6fde434e561f30250a35fd3b3] */
#define mt_expr(...)                                                      \
    mt_exprv(MT_NARG(__VA_ARGS__),                                     \
                (mt_atom *[]){ MT_MAP(__VA_ARGS__) })

/* --- lifetime --- */

/* Take a reference. Returns its argument, so it composes inline. NULL-safe. */
MT_API mt_atom *mt_keep(const mt_atom *atom);

/* Drop a reference. NULL-safe. Teardown allocates nothing and uses no recursive
   calls, including for shared and deeply nested expressions. Release callbacks
   run synchronously when their final owner goes away.
   [tested: tests/test_ownership.c; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c] */
MT_API void mt_drop(const mt_atom *atom);

/* --- reading. Each returns the value the way atoi() and strlen() do, and
       records a failure you can check with mt_ok(). --- */

MT_API mt_kind mt_kind_of(const mt_atom *atom);

/* The name of a SYMBOL, VARIABLE or SPACE, the text of a TEXT, the digits of
   a BIGINT, the canonical N/D of a BIGRATIONAL, the engine's written form of
   a HANDLE. NULL for every other kind.
   Borrowed. A handle's written form presents it and does not identify it;
   see MT_HANDLE. */
MT_API const char *mt_name(const mt_atom *atom);
MT_API size_t mt_name_len(const mt_atom *atom);

/* An exact integer. INT only: a Float is not rounded here, and a BigInt does
   not fit by definition. 0 and a recorded failure otherwise. */
MT_API int64_t mt_int(const mt_atom *atom);

/* A double. Promotes losslessly (decision 5): a Float is itself, an Int of
   magnitude below 2^53 is exact, a Rational is its quotient. An Int above
   2^53, a BigInt and a BigRational are REFUSED rather than rounded; read
   those with mt_name. 0.0 and a recorded failure otherwise. */
MT_API double mt_float(const mt_atom *atom);

MT_API bool mt_truth(const mt_atom *atom);
/* A ratio is a pair, so it comes back as one rather than through two
   out-parameters. An INT reads as itself over 1, which is rule 5's promotion
   and exact; it has to, because mt_rational() answers an Int for a canonical
   denominator of 1 and its own accessor cannot refuse what it built. `den` is
   0 for anything else, a BigRational included since its halves do not fit,
   which is a value no ratio has, and the failure is recorded. */
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
/* Borrow the contiguous child vector. Its length is mt_len(atom). */
MT_API const mt_atom *const *mt_children(const mt_atom *atom);

/* Structural equality, matching the engine's term identity. Two variables are
   equal when their names are; signed float zeros differ and every NaN agrees,
   because SWI canonicalises NaN payloads as they enter a term. Answers false
   and records MT_NOMEM in the one case where it cannot decide, a machine that
   cannot hold the walk's own stack. */
MT_API bool mt_eq(const mt_atom *a, const mt_atom *b);

/* Equality up to a consistent renaming of variables: MeTTa's =alpha, the
   engine's variant check, and the Python seat's Atom.alpha_eq. The renaming is
   a bijection, so (f $x $y) is alpha-equal to (f $a $b) but not to (f $a $a),
   and each anonymous `_` is a variable of its own. Every other leaf compares
   as mt_eq compares it. Needs no engine; false for NULL. This is how to ask
   whether an answer is the atom you expected when the answer's variables
   carry engine names [tested: tests/test_cmetta.c,
   test_alpha_equivalence_is_a_renaming; commit=52d89c668f800fe58692a0b3a3a733591e39e94e]. */
MT_API bool mt_alpha_eq(const mt_atom *a, const mt_atom *b);

/* The standard order of terms, the order the engine's msort answers in:
   negative, zero or positive as a sorts before, with or after b. Variables
   come first, then numbers by exact value (a float before an exact number of
   equal value, NaN first, -0.0 before 0.0), then strings, host values, the
   empty expression, symbols (True and False as the engine's atoms true and
   false), and other expressions child by child with a prefix first. The
   engine orders variables by age, which C cannot see; here they order by
   name. Needs no engine; NULL records MT_MISUSE.

   mt_order is the same order shaped for qsort and bsearch over an array of
   atom pointers, such as an mt_list's items:

       mt_list all = mt_all(mt_atoms(kb));
       qsort(all.items, all.len, sizeof *all.items, mt_order);

   [tested: tests/test_cmetta.c, test_the_standard_order_is_the_engines;
   commit=d1e3a98101670ae0c56e1f7b23a690916cf5e06e] */
MT_API int mt_compare(const mt_atom *a, const mt_atom *b);
MT_API int mt_order(const void *a, const void *b);

/* Structural FNV-1a hash matching mt_eq(): equal atoms always hash alike,
   signed zeros remain distinct, all NaN payloads agree, counted text includes
   embedded NUL, and C objects hash by identity. This is a non-cryptographic,
   in-process hash for caller-owned tables, not a persistent or portable wire
   identifier. NULL records MT_MISUSE and returns 0; an exhausted deep-walk
   stack records MT_NOMEM and returns 0
   [tested: tests/test_hash.c;
   commit=d37f1a5192999fdaa1a617e86191de4fe3570f91]. */
MT_API uint64_t mt_hash(const mt_atom *atom);

/* A normalized substitution produced by mt_unify() or mt_unifyv(). The
   object owns its variable and value atoms; the accessors BORROW them until
   mt_bindings_free(). Entries retain the deterministic binding order of the
   same last-child-first work-list the Python seat uses.

   Unification is symmetric, iterative and has no occurs check, matching the
   Python seat: variables in either operand bind, `_` is anonymous, and
   variadic unification makes every operand agree with the first through one
   shared substitution. A structural mismatch returns NULL without recording
   an error; call mt_clear() first when NULL must be distinguished from an
   allocation or contract failure. Ground equality succeeds with a non-NULL
   substitution of length zero. Binding values are transitively normalized,
   while no-occurs-check cycles remain finite.

   mt_substitute() BORROWS both arguments and returns an OWNED atom. It applies
   one normalized substitution without walking a replacement again, so a
   cyclic binding such as `$x = (f $x)` remains a finite `(f $x)`.
   [tested: tests/test_unify.c;
   commit=e927fffde3a19d9927892bf64a7fc6202b866ae0]

   Both treat an MT_HANDLE as a leaf, which unifies with a variable or a
   handle equal to it and which substitution leaves as it is: the variables
   inside a carried term are the engine's to bind, when the handle goes back. */
typedef struct mt_bindings mt_bindings;

MT_API MT_MUST_USE mt_bindings *mt_unify(const mt_atom *left,
                                         const mt_atom *right);
MT_API MT_MUST_USE mt_bindings *mt_unifyv(
    size_t count, const mt_atom *const *atoms);
MT_API size_t mt_bindings_len(const mt_bindings *bindings);
MT_API const mt_atom *mt_binding_var(const mt_bindings *bindings,
                                     size_t index);
MT_API const mt_atom *mt_binding_value(const mt_bindings *bindings,
                                       size_t index);
/* A value by variable NAME, BORROWED; NULL when the name is unbound. */
MT_API const mt_atom *mt_binding(const mt_bindings *bindings,
                                 const char *name);
MT_API MT_MUST_USE mt_atom *mt_substitute(const mt_atom *atom,
                                          const mt_bindings *bindings);
MT_API void mt_bindings_free(mt_bindings *bindings);

/* --- text, through the engine's own reader and writer --- */

/* Read one MeTTa form. The engine's reader is the only reader. */
MT_API MT_MUST_USE mt_atom *mt_parse(const char *source);
/* The counted twin, for source containing NUL. */
MT_API MT_MUST_USE mt_atom *mt_parsen(const char *source, size_t length);

typedef struct mt_string {
  char  *data;
  size_t len;
} mt_string;

/* Present an atom the way the engine displays it, into a per-thread rotating
   buffer so it drops straight into printf:

       printf("%s -> %s\n", mt_show(pattern), mt_show(answer));

   Presentation is deliberately lossy for values with no MeTTa source form. A
   counted string containing NUL is truncated by this C-string API. The buffer
   is reused after MT_SHOW_SLOTS further calls on this thread, which is the
   contract strerror() and inet_ntoa() already gave C. Take a copy with
   mt_show_dup() to keep it, and free that with mt_free(). */
#define MT_SHOW_SLOTS 8
MT_API const char *mt_show(const mt_atom *atom);
MT_API MT_MUST_USE char *mt_show_dup(const mt_atom *atom);

/* Serialize an atom to MeTTa source that mt_parsen() reads back as an equal
   atom. `data` is OWNED and counted by `len`; free it with mt_free().
   {NULL, 0} with the engine's reason when the value has no round-trip source
   spelling, such as a live C object, a non-finite float or a symbol containing
   whitespace. The count is what lets a text atom carry NUL without truncation
   [tested: test_presentation_and_round_trip_text_are_distinct;
   commit=2e13376bb6e1662655525533a1ab02800940aec5]. */
MT_API MT_MUST_USE mt_string mt_write_dup(const mt_atom *atom);

/* Release raw storage obtained from mt_alloc, mt_calloc, mt_resize or the
   library's owned string/list results. Handles have their own release doors.
   Never pass a libc allocation. NULL-safe. */
MT_API void mt_free(void *pointer);

/* ================================================================== *
 * The runtime
 * ================================================================== */

typedef struct metta metta;
/* A closed callback scope: MT_OK commits, MT_FAIL rolls back without an
   exception, and an error status rolls back with its thread-local reason.
   Nested C transactions are savepoints. Engine state and this library's
   registrations roll back; arbitrary host memory and I/O belong to the caller.
   Transactions must be entered and completed on one attached thread. Enter
   every enclosing transaction through these doors when changing C registrations;
   raw nested engine scopes cannot own the corresponding C rollback snapshot. */
typedef mt_status (*mt_scope_fn)(metta *runtime, void *user);
MT_API mt_status mt_transaction(metta *runtime, mt_scope_fn body, void *user);
/* Run the same callback and always discard engine and registration changes. */
MT_API mt_status mt_speculate(metta *runtime, mt_scope_fn body, void *user);
typedef struct mt_space mt_space;
typedef struct mt_answers mt_answers;

/* An owned array allocated with mt_alloc/mt_calloc/mt_resize and its length.
   Doors taking an mt_list take both the atoms and the array. */
typedef struct mt_list {
  mt_atom **items;
  size_t    len;
} mt_list;

/* Read every top-level form, including directives, without executing it.
   Empty source returns {NULL, 0}; a syntax failure returns the same empty list
   with mt_error set. Variable names remain as written in each form. */
MT_API MT_MUST_USE mt_list mt_forms(const char *source);

/* Pull one owned atom with MT_ROW, end with MT_DONE and NULL, or return an
   error after mt_error_set. close(state) runs once on exhaustion, refusal or
   abandonment. The state may contain retained arguments or any C resource. */
typedef struct mt_iterator {
  void *state;
  mt_status (*next)(void *state, mt_atom **answer);
  void (*close)(void *state);
} mt_iterator;

/* Takes the iterator, including on failure. Works without an engine. Row text
   is NULL for native iterators; use the atom accessors or mt_show after boot. */
MT_API MT_MUST_USE mt_answers *mt_answers_from(mt_iterator iterator);
/* A consumptive iterator as a grounded value. Takes the iterator on every
   path. mt_stream_of borrows its cursor while the atom lives. In MeTTa,
   (c-iter value) consumes its remaining answers. Applying an mt_function
   that uses mt_answer_iter returns such a value, because the engine's
   grounded_apply protocol returns one value. */
MT_API MT_MUST_USE mt_atom *mt_stream(mt_iterator iterator);
MT_API mt_answers *mt_stream_of(const mt_atom *atom);
/* The last step's status, initially MT_OK. This does not advance the cursor. */
MT_API mt_status mt_answers_status(const mt_answers *answers);
/* Advance with an explicit status; *answer is borrowed or NULL. */
MT_API mt_status mt_step(mt_answers *answers, const mt_atom **answer);

typedef struct mt_config {
  const char *path;      /* engine tree; NULL takes $METTA_PATH then the
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
   until their own references go, and so do the answers an eager mt_run()
   already collected. Every door that needs the engine refuses afterwards,
   naming mt_open(); the four release doors -- this one, mt_answers_free(),
   mt_space_close() and mt_thread_detach() -- stay no-ops instead, so a host
   is not punished for the order it tidies up in. A SWI halt hook may cancel
   cleanup; then the runtime remains open and mt_error() is MT_ERROR. If SWI
   completes shutdown but cannot reclaim its memory, the handle closes with
   MT_ERROR and this process refuses a later unsafe restart. */
MT_API void mt_close(metta *runtime);

/* Whether the engine prints compiled forms. Returns the previous setting. */
MT_API bool mt_verbose(metta *runtime, bool verbose);

/* A thread other than the one that opened the runtime attaches before it
   touches the engine and detaches before it exits. Building and reading atoms
   needs neither. */
MT_API bool mt_thread_attach(void);
MT_API void mt_thread_detach(void);

/* &self and &petta, borrowed and living as long as the runtime. NULL, with
   MT_MISUSE recorded, when `runtime` is the NULL a failed mt_open() answered
   or the engine is closed. */
MT_API mt_space *mt_self(metta *runtime);
MT_API mt_space *mt_catalog(metta *runtime);

/* Create or open a space by name; names begin with '&'. NULL on failure. */
MT_API MT_MUST_USE mt_space *mt_space_open(metta *runtime, const char *name);
MT_API void mt_space_close(mt_space *space);
/* Release the engine space and its compiled definitions. The C handle remains
   owned and must be closed. The engine decides which spaces are releasable. */
MT_API bool mt_space_drop(mt_space *space);
/* The name is C memory, so this answers whether or not the engine is running,
   and NULL for a NULL space rather than refusing: it is a classifier. */
MT_API const char *mt_space_name(const mt_space *space);

/* ================================================================== *
 * Asking
 * ================================================================== */

/* Run MeTTa source in the receiver's space. Every `!` form contributes a group of answers in
   source order, and a row's `group` field says which one it came from.

   Eager: the engine's run door computes the whole program before the first
   answer, because that is what running a program means. mt_eval() is the
   lazy door. NULL on failure. */
MT_API MT_MUST_USE mt_answers *mt_self_run(metta *runtime, const char *source);
MT_API MT_MUST_USE mt_answers *mt_space_run(mt_space *space, const char *source);

/* Load a file through the same door `import!` uses, so a reload replaces the
   first load's definitions rather than doubling them. */
MT_API MT_MUST_USE mt_answers *mt_self_load(metta *runtime, const char *path);
MT_API MT_MUST_USE mt_answers *mt_space_load(mt_space *space, const char *path);

/* ------------------------------------------------------------------ *
 * Lowering: C source becoming MeTTa
 * ------------------------------------------------------------------ */

/* The MeTTa text of a token sequence the C compiler saw. Two levels, so the
   argument is macro-expanded before it is stringified: that is what lets a
   body assembled by other macros arrive here already expanded. */
#define MT_METTA(tokens)  MT_METTA_(tokens)
#define MT_METTA_(tokens) #tokens

/* Stringify exactly the tokens written at the call site. Use this when a MeTTa
   symbol collides with a C macro and expansion would silently change it. */
#define MT_METTA_RAW(tokens) #tokens

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

/* The non-expanding twin. Both arguments are stringified in this macro itself,
   because forwarding either through another macro would expand it first
   [tested: test_raw_lowering_preserves_tokens_that_are_c_macros;
   commit=2e13376bb6e1662655525533a1ab02800940aec5]. */
#define mt_lower_raw(runtime, head, body)                                 \
    mt_do((runtime), "(= " #head " " #body ")")

/* Run source for its EFFECT and discard the answers: definitions, imports,
   pragmas, anything whose point is what it leaves behind rather than what it
   answers. True when it ran.

       mt_do(m, "(= (double $x) (* 2 $x))");

   The alternative is mt_answers_free(mt_run(...)), which says the same thing
   with the reader's attention on the free rather than on the program. */
MT_API bool mt_self_do(metta *runtime, const char *source);
MT_API bool mt_space_do(mt_space *space, const char *source);

/* The pairs the verbs below dispatch between. Call these directly if you
   would rather not go through _Generic. Each TAKES its atom argument. */
MT_API MT_MUST_USE mt_answers *mt_self_eval(metta *runtime, mt_atom *goal);
MT_API MT_MUST_USE mt_answers *mt_space_eval(mt_space *space, mt_atom *goal);
/* Inspect source effects without evaluating goal. TAKES goal and returns an
   owned (EffectPlan <joined-class> ((<operation> <class>) ...)) atom. The
   shared source planner includes compilation effects and conservatively
   classifies dynamic calls. NULL carries the error through mt_error().
   [tested: tests/test_native_parity.c; commit=91eef0753a3d55913cee42a2d385bbbf008f0be5] */
MT_API MT_MUST_USE mt_atom *mt_self_effect_plan(metta *runtime, mt_atom *goal);
MT_API MT_MUST_USE mt_atom *mt_space_effect_plan(mt_space *space, mt_atom *goal);
MT_API MT_MUST_USE mt_answers *mt_self_match(metta *runtime, mt_atom *pattern);
MT_API MT_MUST_USE mt_answers *mt_space_match(mt_space *space, mt_atom *pattern);
/* Engine match followed by a True-valued guard. A conjunction is the ordinary
   expression ( , pattern ... ). NULL guard means True. TAKES both atoms;
   mt_bound reads named variables from each instantiated pattern. Retain the
   pattern with mt_keep to prepare it once and read current facts repeatedly.
   Longhand: (match space pattern (if guard pattern Empty)). */
MT_API MT_MUST_USE mt_answers *mt_self_query(metta *runtime, mt_atom *pattern,
                                            mt_atom *guard);
MT_API MT_MUST_USE mt_answers *mt_space_query(mt_space *space, mt_atom *pattern,
                                             mt_atom *guard);
/* Relational let answered as bindings, the Python seat's solve(). The known
   value goes on let's pattern side and the relation runs backwards:

       mt_rows (row, mt_solve(m, mt_num(10), mt_expr("double", mt_var("x"))))
           printf("x = %s\n", mt_show(mt_bound(row, "x")));      prints x = 5

   The answer template is derived rather than written: the named variables of
   the pattern, then those the subject adds, each at its first occurrence, a
   lone one standing for itself. Every answer is an instance of it, so
   mt_bound() reads each variable by name. TAKES both atoms. NULL with
   MT_MISUSE when neither holds a named variable, `_` being no name.
   Longhand: (let pattern subject ($x $y ...)) [tested: tests/test_cmetta.c,
   test_solve_runs_let_backwards_and_reads_bindings_by_name;
   commit=65b02ca599b0db696faf221f4f39e94210013fc1]. */
MT_API MT_MUST_USE mt_answers *mt_self_solve(metta *runtime, mt_atom *pattern,
                                            mt_atom *subject);
MT_API MT_MUST_USE mt_answers *mt_space_solve(mt_space *space, mt_atom *pattern,
                                             mt_atom *subject);
/* Evaluate under the engine's per-ask algebra. Each answer is (value coefficient),
   both ordinary atoms; the cursor owns it until the next step. TAKES algebra
   and goal. The declaration remains unchanged after close, failure or exhaustion.
   [tested: test_algebras_are_scoped_engine_data; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c] */
MT_API MT_MUST_USE mt_answers *mt_self_eval_under(metta *runtime, mt_atom *algebra,
                                                 mt_atom *goal);
MT_API MT_MUST_USE mt_answers *mt_space_eval_under(mt_space *space, mt_atom *algebra,
                                                  mt_atom *goal);
MT_API MT_MUST_USE mt_answers *mt_self_atoms(metta *runtime);
MT_API MT_MUST_USE mt_answers *mt_space_atoms(mt_space *space);
MT_API bool mt_self_add(metta *runtime, mt_atom *atom);
MT_API bool mt_space_add(mt_space *space, mt_atom *atom);
MT_API bool mt_self_add_all(metta *runtime, mt_list atoms);
MT_API bool mt_space_add_all(mt_space *space, mt_list atoms);
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
#define mt_effect_plan(target, goal) MT_ON((target), effect_plan)((target), (goal))
#define mt_run(target, source)  MT_ON((target), run)((target), (source))
#define mt_load(target, path)   MT_ON((target), load)((target), (path))
#define mt_do(target, source)   MT_ON((target), do)((target), (source))

/* Stored atoms unifying a pattern, lazily. TAKES `pattern`. */
#define mt_match(target, pat)   MT_ON((target), match)((target), (pat))
#define mt_query(target, pat, guard) MT_ON((target), query)((target), (pat), (guard))
#define mt_solve(target, pattern, subject) \
    MT_ON((target), solve)((target), (pattern), (subject))
#define mt_eval_under(target, algebra, goal) \
    MT_ON((target), eval_under)((target), (algebra), (goal))

/* Every stored atom, lazily. */
#define mt_atoms(target)        MT_ON((target), atoms)((target))

/* Add one atom. TAKES it. */
#define mt_add(target, atom)    MT_ON((target), add)((target), (atom))

/* Add one batch through one engine call. TAKES every atom and the array;
   {NULL, 0} is a valid empty batch. A refused member writes none
   [tested: tests/test_batch_add.c;
   commit=c591b4e77d4ca20fcedcf4c87a942d5afc2bf625]. At 2,000 atoms the batch
   costs 12,045 engine inferences against 46,028 sequentially
   [measured 2026-09-01: 46028 sequential and 12045 batch inferences;
   command=for run in 1 2 3; do extensions/cmetta/tests/test_batch_add; done;
   fixture=2000 distinct integer atoms per fresh named space;
   commit=c591b4e77d4ca20fcedcf4c87a942d5afc2bf625]. */
#define mt_add_all(target, atoms) MT_ON((target), add_all)((target), (atoms))

/* Remove one unifying occurrence; true when it was there. A bare variable is
   refused, as it names no single occurrence. TAKES the atom. */
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
  const char    *text;   /* presentation text, like mt_show(); it can show a
                            host-only value mt_write_dup() refuses, and its C
                            string spelling truncates an embedded NUL       */
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
   [tested: tests/test_cmetta.c, test_one_and_first_make_different_claims;
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
  const char  *name;    /* exact engine name; no identifier conversion */
  size_t       arity;
  mt_effect effect;
  mt_fn     fn;
  void        *user;    /* handed back to fn on every application         */
} mt_op;

/* Publish, so `(name a b)` in MeTTa calls it. The name reaches MeTTa EXACTLY
   as written, which is what the field above says and what ABI 1 changed:
   `car_atom` publishes `car_atom`, and a hyphenated head is registered by
   writing `car-atom` here. This paragraph described the automatic conversion
   the break removed [source: extensions/cmetta/CHANGELOG.md, "Preserve exact
   published names"]. */
MT_API bool mt_def(metta *runtime, mt_op op);

/* Withdraw a published function at every arity, giving the name back. */
MT_API bool mt_undef(metta *runtime, const char *name);

/* Inside a published function. */
MT_API size_t mt_arity(const mt_call *call);
MT_API const mt_atom *mt_arg(const mt_call *call, size_t index);
MT_API metta *mt_of(const mt_call *call);

/* Answer with an atom, which is TAKEN. Answering twice is MT_MISUSE. */
MT_API mt_status mt_answer(mt_call *call, mt_atom *atom);
/* Answer incrementally. Takes the iterator on every path. Its close callback
   and this call's argument release run on exhaustion, error, cut or cursor
   abandonment. Arguments remain valid until close returns. */
MT_API mt_status mt_answer_iter(mt_call *call, mt_iterator iterator);

/* Refuse this application with words the engine reports. Returns MT_ERROR
   so it too can be the return statement. */
MT_API mt_status mt_fail(mt_call *call, const char *message);

/* --- carrying a C value through MeTTa untouched --- */

typedef void (*mt_free_fn)(void *value);

/* Wrap a C pointer as a grounded atom. MeTTa carries it by reference, never
   serialises it, and hands it back unchanged. The release callback runs when
   the last C and engine owner lets go; engine blob garbage collection decides
   the ordinary timing after a value has crossed. Takes value on every path,
   including constructor failure, which calls release immediately. A non-NULL
   type_name is its exact engine type symbol as well as the borrowed mt_type
   result; NULL contributes no type candidate. Invalid UTF-8 refuses when
   the type crosses into the engine. */
MT_API MT_MUST_USE mt_atom *mt_object(void *value, const char *type_name,
                                   mt_free_fn release);
MT_API void *mt_value(const mt_atom *atom);
MT_API const char *mt_type(const mt_atom *atom);

/* Deterministically release an object's engine blob and CONSUME this C atom.
   Other C references keep the box alive until they too are dropped. Existing
   Prolog aliases become invalid and a later attempt to return one is refused
   as a released object instead of being dereferenced. Before mt_open(), or
   after mt_close() has already released every blob, this is mt_drop() with the
   same take semantics. False only for NULL, a non-object, or an FLI failure. */
MT_API bool mt_object_free(mt_atom *atom);

/* A C function as a VALUE rather than a name, so `($f 2)` calls it wherever
   the atom lands. This is what C answers to a Python callable being an atom;
   mt_def() is the other half, a function reached by its published name.
   Takes user on every path, including a NULL function or allocation failure. */
MT_API MT_MUST_USE mt_atom *mt_function(mt_fn fn, void *user,
                                     mt_free_fn release);

/* A grounded value with custom structural matching inside engine `unify`.
   fn borrows one argument, the other operand, and answers candidate atoms
   with mt_answer or mt_answer_iter. Each candidate unifies with that operand;
   MT_FAIL means no match. Errors abort matching. A free variable binds the
   value whole without invoking fn. This value is not callable.
   Takes user on every path; release runs after the last C, blob and active
   matcher cursor owner releases it. Native mt_unify remains structural and
   does not invoke engine hooks.
   [tested: tests/test_matchers.c; commit=91eef0753a3d55913cee42a2d385bbbf008f0be5] */
MT_API MT_MUST_USE mt_atom *mt_matcher(mt_fn fn, void *user,
                                    mt_free_fn release);

/* ================================================================== *
 * Extending this seat
 * ================================================================== */

/* This seat's own extension seam, the twin of engine/ext_points.pl one level
   out and of metta.seam on the Python seat. A POINT is declared with a KIND
   and the fields a row carries; a REGISTRANT is a row against a declared
   point; and both read back as data, so "what can I extend here" is a query
   rather than a source reading.

   Four kinds where the engine declares five: host_service splits service by an
   audience internal to the engine (host bindings against extensions) and a
   seat has one audience. The kind decides how a point is READ, and reading it
   another way is refused:

     MT_DECLARATION  every row is read, as data
     MT_OWNERSHIP    the FIRST row whose claims() answers takes the request
     MT_EVENT        every row runs and its answer is discarded
     MT_SERVICE      the SEAT writes it and a registrant CALLS it

   mt_def, mt_repr, mt_provider_open, mt_library and mt_subscribe write
   registrations against their respective points. mt_object owns a box;
   constructing one does not create a registry row.

   The seam table follows the operation table's rule stated at the top of this
   header: it is NOT guarded, so register everything before the threads that
   evaluate start. Reading it back is a read and takes no lock. */
typedef enum mt_seam_kind {
  MT_DECLARATION = 0,
  MT_OWNERSHIP   = 1,
  MT_EVENT       = 2,
  MT_SERVICE     = 3
} mt_seam_kind;

/* One declared extension point. `fields` is the names a row carries, space
   separated, and `doc` is what the point decides, both for a program reading
   the seam back rather than for a compiler. */
typedef struct mt_point {
  const char  *name;
  mt_seam_kind kind;
  const char  *fields;
  const char  *doc;
} mt_point;

/* Declare a point. A second declaration of the same name is refused naming the
   first, because a point with two kinds has two read rules and neither is
   true. A library declares one of its own exactly as this seat declares its
   shipped ones, which is seam:kind/2 being multifile one level out. */
MT_API bool mt_point_declare(metta *runtime, mt_point point);

/* The declared points, by index and by name. mt_point_at() answers NULL past
   the end, so a walk is `for (i = 0; (p = mt_point_at(m, i)); i++)`, the same
   shape mt_arg() takes inside a published function. */
MT_API size_t mt_point_count(metta *runtime);
MT_API const mt_point *mt_point_at(metta *runtime, size_t index);
MT_API const mt_point *mt_point_of(metta *runtime, const char *name);

/* One registration against a declared point.

   `mt_seam_row` and not `mt_row`, because `mt_row` is already an ANSWER row in
   this header (mt_row_next, mt_bound) and one header has one meaning per name.
   Its readers are mt_seam_count and mt_seam_at for the same reason.

   `value` is whatever that point's contract says and is not interpreted here;
   `release` runs when the row is withdrawn or the runtime closes. `claims` is
   for an MT_OWNERSHIP point only: it answers non-NULL to take the request and
   NULL to decline, which is pluggy's firstresult and the engine's own
   ownership rule. */
typedef struct mt_seam_row {
  const char *point;
  const char *name;
  void       *value;
  void       *(*claims)(void *value, void *subject);
  mt_free_fn  release;
} mt_seam_row;

/* Add a row. Refuses an undeclared point naming every declared one, and a row
   with no claims() against an ownership point. Registering an existing name
   REPLACES that row in place, which keeps ownership order stable. */
MT_API bool mt_register(metta *runtime, mt_seam_row row);
MT_API bool mt_unregister(metta *runtime, const char *point, const char *name);

/* The rows against one point, in registration order. NULL past the end. */
MT_API size_t mt_seam_count(metta *runtime, const char *point);
MT_API const mt_seam_row *mt_seam_at(metta *runtime, const char *point,
                                    size_t index);

/* Consult an MT_OWNERSHIP point: the first row that claims `subject`, with its
   answer written through `answer` when that is not NULL. NULL when no row
   claims, which is an answer rather than a failure. */
MT_API const mt_seam_row *mt_claim(metta *runtime, const char *point,
                                   void *subject, void **answer);

/* --- what a library may register --- */

/* How a C object of one type PRINTS in MeTTa. Without one, an object renders
   as its type name, which is honest and useless for reading an answer; this is
   the door that makes it readable without pretending the value has a MeTTa
   form it does not have. The text your function answers is read before the
   call returns, so a static buffer is enough and a caller may reuse it. */
typedef const char *(*mt_text_fn)(void *value, void *user);
MT_API bool mt_repr(metta *runtime, const char *type_name, mt_text_fn text,
                    void *user);

/* Atoms held by a C backend. Callback arguments BORROW immutable atoms; retain
   with mt_keep when storing one. match opens a candidate iterator whose close
   runs on exhaustion, failure or abandonment. The pattern stays alive until
   close, and the engine unifies every candidate. limit is advisory, zero when
   absent: apply it only when candidates are exact answers. A variable pattern
   asks for all atoms, so enumeration needs no second callback.

   A store answers the engine with the atoms the engine gave it, so add,
   remove and match read every argument such that it goes back as the term it
   was: a proper list, a symbol, a number, text and a variable read as the
   wire grammar reads them, and a non-list compound, a dict and an improper or
   partial list, which that grammar would give back changed, arrive CARRIED,
   each an MT_HANDLE holding the engine's term. Answering a stored atom puts
   the identical term back, a variable it shares with the rest of its atom
   still shared, so a partial application stored here still applies when
   matched back, exactly as from the native space. Removing and matching find
   it by value: the carried handle in the engine's argument is mt_eq to the
   stored one, while the list that spells its expression is another atom
   [tested: tests/test_providers.c, test_a_stored_compound_comes_back_whole;
   commit=256a3a6aa4248e89c7b007edacc6832d62eb4594]. A store that writes its atoms out as source cannot
   write a carried one: mt_write_dup() refuses it by name.

   Callbacks return MT_OK or an error set with mt_error_set. remove additionally
   writes whether one occurrence was removed. NULL declines a capability.
   begin/commit/rollback must be supplied together or all absent. begin owns
   recovery on failure; a successful begin owes one commit or rollback, including
   nested speculation. A failing completion still releases its local resources.
   The selected registration is retained through completion even if its name is
   closed and reused. External concurrency is the provider's responsibility.
   [tested: tests/test_providers.c; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c] */
typedef struct mt_provider {
  void       *user;
  mt_status   (*add)(void *user, const mt_atom *atom);
  mt_status   (*remove)(void *user, const mt_atom *atom, bool *removed);
  mt_status   (*match)(void *user, const mt_atom *pattern, size_t limit,
                       mt_iterator *answers);
  mt_status   (*clear)(void *user);
  mt_status   (*begin)(void *user);
  mt_status   (*commit)(void *user);
  mt_status   (*rollback)(void *user);
  mt_free_fn  release;
} mt_provider;

/* Back a named space with a provider, and stop backing it. The name is a
   space name, `&stars`: one that is not is refused at the door, and so is one
   another provider already owns, through the engine's own claim on the name.
   Open TAKES user through release on every path. Closing releases the name;
   suspended queries and transaction completion retain the old provider until
   their last owner releases it. Completed transaction captures release eagerly,
   so closing the final cursor after close runs release without waiting for GC
   [tested: tests/test_providers.c; commit=91eef0753a3d55913cee42a2d385bbbf008f0be5]. */
MT_API bool mt_provider_open(metta *runtime, const char *space,
                             mt_provider provider);
MT_API bool mt_provider_close(metta *runtime, const char *space);

/* A standing query over committed additions and removals. notify borrows the
   matching atom and runs synchronously on the writer's attached thread;
   true means added and false removed. Return MT_OK or an error status.
   A notification failure reports an error after commit, without undoing the
   committed write. The caller owns any queue, scheduler and external locking.

   Names identify registrations exactly, without identifier conversion. A
   subscription watches its named space until explicitly unsubscribed or the
   runtime closes; close it before reusing that space's name. Register and
   withdraw under the same serialization rule as operations. Self-cancellation
   is supported and keeps callback data alive until notify returns. */
typedef struct mt_subscription {
  const char *space;
  mt_atom *pattern;
  mt_status (*notify)(void *user, bool added, const mt_atom *atom);
  void *user;
  mt_free_fn release;
} mt_subscription;

/* Subscribe TAKES pattern and user on every path; space and name are copied.
   Refuses a duplicate name or a space without the engine's events capability.
   Registration and cancellation participate in mt_transaction/mt_speculate.
   A missing registration makes unsubscribe false without setting an error. */
MT_API bool mt_subscribe(metta *runtime, const char *name,
                         mt_subscription subscription);
MT_API bool mt_unsubscribe(metta *runtime, const char *name);

/* A directory of MeTTa or Prolog sources this library ships, under an alias,
   so `(library <alias> <file>)` resolves from MeTTa and from C. This is SWI's
   own file_search_path/2, so an alias registered here is one every SWI tool
   already understands. */
MT_API bool mt_library(metta *runtime, const char *alias, const char *directory);

/* Load a shared object and let it register.

   The library must export `mt_extension_init`, which this calls with the
   runtime; everything it registers is ordinary and nothing here knows its
   name. This is sqlite3's loadable-extension shape, entry point and all
   [source: https://www.sqlite.org/loadext.html], and it is what makes a
   library a SATELLITE of this seat rather than a fork of it. The handle stays
   open until process exit, because an escaped value may hold a pointer into it.

   Initialization runs in a closed transaction. A refusal rolls back engine
   state and C registrations; external host effects remain the initializer's
   responsibility. Even a refused initializer's handle stays loaded until
   process exit because it may have handed C function values to its caller.
   Refuses when the file cannot be opened, when it exports no
   mt_extension_init, or when that function answers false, each naming which.
   [tested: tests/test_extensions.c; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c] */
typedef bool (*mt_extension_fn)(metta *runtime);
MT_API bool mt_extension(metta *runtime, const char *path);

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
   [tested: tests/test_cmetta.c, test_a_bound_stops_a_runaway_and_says_so;
   commit=a8ea956cecbe8af67a7dd340f00c74dd94dbfb7c].
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
