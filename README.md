<!--
Purpose: show the C API through examples, with cmetta.h as the contract.
-->

# CMeTTa

A C program boots MeTTa in its own process, builds and reads terms, runs programs, pulls answers, and publishes functions the language can call.

```c
#define MT_SHORTHAND
#include <cmetta.h>
#include <stdio.h>

int main(void)
{ metta *m = mt_open(NULL);

  mt_each (a, mt_run(m, "(= (double $x) (* 2 $x))\n!(double 21)"))
      printf("%s\n", mt_show(a));                 /* 42 */

  printf("%lld\n", (long long)mt_one_int(mt_eval(m, E("+", 1, 2))));

  mt_close(m);
}
```

Build with `sh build.sh` and test with `sh test.sh`, using a C11 compiler and SWI-Prolog's development headers located through `swipl --dump-runtime-variables`.

[MesTTo/CMeTTa](https://github.com/MesTTo/CMeTTa) is the C driver beside `extensions/python` and `extensions/node`, separate from the vendored CeTTa substrate.

| Driver | Engine access | Terms |
|---|---|---|
| C | embedded SWI-Prolog | `term_t` through `PL_get_*`, no wire codec |
| Python | janus | tagged arrays from `CODEC.md` |
| Node | WebAssembly | tagged arrays from `CODEC.md` |

## The surface

[cmetta.h](cmetta.h) defines every name and signature; [llms.txt](llms.txt) is the compact usage reference.

| Feature | Doors |
|---|---|
| Runtime | `mt_open`, `mt_close`, `mt_verbose`, `mt_thread_attach`, `mt_thread_detach`, `mt_version` |
| Constructors, marked `MT_MUST_USE` | `mt_sym`, `mt_var`, `mt_text`, `mt_textn`, `mt_num`, `mt_real`, `mt_bool`, `mt_unit`, `mt_bigint`, `mt_rational`, `mt_spaceref`, `mt_exprv`, `mt_object`, `mt_function` |
| C argument conversion | `mt_expr`, `mt_atom_of`; helpers `mt_num_`, `mt_real_`, `mt_same`, `mt_same_c` |
| References | `mt_keep`, `mt_drop` |
| Inspection | `mt_kind_of`, `mt_kind_str`, `mt_name`, `mt_name_len`, `mt_int`, `mt_float`, `mt_truth`, `mt_ratio_of`, `mt_len`, `mt_at`, `mt_eq`, `mt_hash` |
| Unification | `mt_unify`, `mt_unifyv`, `mt_bindings_len`, `mt_binding`, `mt_binding_var`, `mt_binding_value`, `mt_bindings_free`, `mt_substitute` |
| Spaces | `mt_self`, `mt_catalog`, `mt_space_open`, `mt_space_close`, `mt_space_name` |
| Programs and answers | `mt_run`, `mt_load`, `mt_do`, `mt_next`, `mt_row_next`, `mt_bound`, `mt_answers_free`, `mt_each`, `mt_rows` |
| Collected answers | `mt_one`, `mt_first`, `mt_one_int`, `mt_one_float`, `mt_one_truth`, `mt_one_name`, `mt_all`, `mt_list_free` |
| Text | `mt_parse`, `mt_parsen`, `mt_show`, `mt_show_dup`, `mt_write_dup`, `mt_free` |
| Errors | `mt_error`, `mt_errmsg`, `mt_remedy`, `mt_ground`, `mt_ok`, `mt_clear`, `mt_status_str` |
| C callbacks | `mt_def`, `mt_undef`, `mt_arity`, `mt_arg`, `mt_of`, `mt_answer`, `mt_fail`, `mt_effect_str` |
| C objects | `mt_value`, `mt_type`, `mt_object_free` |
| Lowering | `mt_lower`, `mt_lower_raw`, `MT_METTA`, `MT_METTA_RAW` |
| Bounds and counters | `mt_limit`, `mt_limits_of`, `mt_stats_now`, `mt_stats_since` |
| Extension points | `mt_point_declare`, `mt_point_count`, `mt_point_at`, `mt_point_of`, `mt_register`, `mt_unregister`, `mt_seam_count`, `mt_seam_at`, `mt_claim` |
| Libraries and providers | `mt_extension`, `mt_repr`, `mt_provider_open`, `mt_provider_close`, `mt_library` |
| Scope cleanup | `MT_AUTO`, `MT_AUTO_ASK`, `MT_TAKE`; helpers `mt_drop_p`, `mt_answers_free_p` |

Like `tgmath.h`, `_Generic` selects the declared function for either a runtime's `&self` or an explicit space.

| Verb | `metta *` receiver | `mt_space *` receiver |
|---|---|---|
| `mt_add` | `mt_self_add` | `mt_space_add` |
| `mt_add_all` | `mt_self_add_all` | `mt_space_add_all` |
| `mt_del` | `mt_self_del` | `mt_space_del` |
| `mt_eval` | `mt_self_eval` | `mt_space_eval` |
| `mt_match` | `mt_self_match` | `mt_space_match` |
| `mt_atoms` | `mt_self_atoms` | `mt_space_atoms` |
| `mt_count` | `mt_self_count` | `mt_space_count` |
| `mt_wipe` | `mt_self_wipe` | `mt_space_wipe` |

## Ownership

`const mt_atom *` inputs borrow and non-`const` inputs take ownership, so pass `mt_keep` when retaining your own reference.

```c
mt_add(kb, mt_expr("edge", "a", "b"));
```

`mt_drop` releases the retained reference after its last use.

```c
mt_atom *p = mt_expr("edge", "a", mt_var("y"));
while (...) mt_each (row, mt_match(kb, mt_keep(p))) ...
mt_drop(p);
```

## Errors

Calls return their value, NULL, or a documented zero, with `mt_error` and `mt_errmsg` retaining failures across success until `mt_clear`, like `errno`.

```c
mt_clear();
double x = mt_float(mt_arg(c, 0));
double y = mt_float(mt_arg(c, 1));
if ( !mt_ok() ) return mt_fail(c, "wanted two numbers");
```

`mt_remedy` gives the repair and `mt_ground` its authority, rendered with the refusal's fields from the engine's `(refusal ...)` catalog row shared with Python and JavaScript.

```c
mt_clear();
mt_run(m, "!(assertEqual 1 2)");
if ( !mt_ok() ) {
  fprintf(stderr, "%s\n", mt_errmsg());   /* MeTTa assertion failed: ... */
  fprintf(stderr, "%s\n", mt_remedy());   /* correct the claim assert makes, ... */
  fprintf(stderr, "%s\n", mt_ground());   /* metta-law: HostLaws: ... */
}
```

Both are NULL for this library's own contract failures and cleared with the message by `mt_clear`.

## Terms

`mt_expr` counts and converts its children through `_Generic`, dropping them all if any constructor fails.

```c
mt_expr("+", 1, 2)                     /* (+ 1 2)       */
mt_expr("edge", "a", mt_var("y"))      /* (edge a $y)   */
mt_expr("f", mt_expr("g", 1), 2.5)     /* (f (g 1) 2.5) */
```

| C argument | Atom |
|---|---|
| integer or float | Number |
| bare string | Symbol, so `"+"` becomes `+`, not quoted MeTTa text |
| `mt_text("...")` | Text |
| atom | itself |

`#define MT_SHORTHAND` before the include enables `S`, `V`, `T`, `N`, `R`, `B`, and `E` without reserving those short names by default; the long names always work.

| Kind | Value |
|---|---|
| `MT_SYMBOL` | a name that denotes itself |
| `MT_TEXT` | grounded text |
| `MT_INT` | an exact integer fitting `int64_t` |
| `MT_FLOAT` | a float; `2` and `2.0` are different atoms |
| `MT_BIGINT` | an exact integer wider than `int64_t`, read as digits |
| `MT_RATIONAL` | an exact ratio |
| `MT_BOOL` | `True` or `False`, not symbols |
| `MT_VARIABLE` | a variable whose name is its identity within the term |
| `MT_EXPR` | an expression; the empty one is unit |
| `MT_SPACE` | an executable space reference |
| `MT_OBJECT` | a live C value by reference |
| `MT_HANDLE` | a native engine value by reference |

C splits the codec's Number tag into four kinds, and reading promotes only where lossless: `mt_float` accepts an Int within 2^53 and refuses one beyond it, while `mt_int` refuses a Float instead of rounding.

`mt_eq` compares structure and `mt_hash` supplies its fast, non-cryptographic 64-bit hash, with equal hashes for equal atoms including distinct NaN payloads and atoms sharing C-object identity.

The hash uses process-local object addresses and native byte order, so it isn't a stored or transmitted atom ID.

## Unification

`mt_unify` borrows both atoms and returns owned, normalized bindings without an engine, binding variables on either side while `_` stays anonymous and mismatch returns NULL without an error.

```c
mt_atom *pattern = E("job", V("who"), V("rank"));
mt_atom *fact = E("job", "ada", 9);
mt_atom *template = E("hired", V("who"), V("rank"));
mt_bindings *bindings = mt_unify(pattern, fact);
mt_atom *answer = bindings ? mt_substitute(template, bindings) : NULL;

if ( answer ) printf("%s\n", mt_show(answer));  /* (hired ada 9) */

mt_drop(answer);
mt_bindings_free(bindings);
mt_drop(template);
mt_drop(fact);
mt_drop(pattern);
```

`mt_unifyv` makes every operand agree with the first through one substitution, including three or more terms.

Bindings retain their values after the inputs are dropped; `mt_binding(bindings, "who")` borrows one, while `mt_bindings_len`, `mt_binding_var`, and `mt_binding_value` enumerate the mapping.

## Text

Building and inspecting atoms starts no engine, but parsing and rendering use the engine's reader and writers.

| Door | Result |
|---|---|
| `mt_parse`, `mt_parsen` | one form, with a byte count for the latter |
| `mt_show` | presentation text in a per-thread rotating buffer, ready for `printf` |
| `mt_show_dup` | an owned presentation copy |
| `mt_write_dup` | owned, counted `mt_string` source, refusing values whose display spelling wouldn't read back equally |
| `mt_free` | releases copied text, including `mt_string.data` |

Embedded NUL survives `mt_write_dup` followed by `mt_parsen(written.data, written.len)`.

## Answers

`mt_eval` computes at most one answer per step, and `mt_each` closes on exhaustion or `break`, leaving an endless generator's remaining answers uncomputed.

```c
mt_each (a, mt_eval(m, E("from", 0)))
{ printf("%lld\n", (long long)mt_int(a));
  if ( ++taken == 5 ) break;          /* the sixth is never computed */
}
```

Leaving a loop with `return` or `goto` requires explicit cursor cleanup or `MT_AUTO_ASK`.

`mt_rows` exposes the atom, the engine's rendering, the originating `!` group, and the cursor.

```c
typedef struct mt_row {
  const mt_atom *atom;   /* the answer itself                     */
  const char    *text;   /* the engine's own rendering            */
  size_t         group;  /* which `!` form produced it            */
  mt_answers    *of;     /* the cursor, so mt_bound takes the row */
} mt_row;
```

`mt_bound` reads a retained match pattern's named binding at any depth in one term walk without an engine call, corresponding to Python's `row.y` and its `Answers`/`Rows` split.

```c
mt_rows (row, mt_match(kb, E("edge", "a", V("y"))))
    printf("y = %s\n", mt_show(mt_bound(row, "y")));
```

Each collector consumes its cursor, with `mt_one` and `mt_first` making the same cardinality claims as Python's `one()` and `first()`.

| Function | Result |
|---|---|
| `mt_one(r)` | exactly one owned answer; refuses zero or many |
| `mt_first(r)` | first owned answer; no claim about the rest |
| `mt_one_int(r)`, `mt_one_float(r)`, `mt_one_truth(r)`, `mt_one_name(r)` | the value, without an atom to release |
| `mt_all(r)` | every answer in an owned `mt_list` of items and length |

`mt_add_all` takes the list's array and atoms, validates every member, and writes through one engine batch call.

```c
mt_list values = mt_all(mt_run(m, "!(superpose (red green blue))"));
if ( !mt_add_all(kb, values) ) fprintf(stderr, "%s\n", mt_errmsg());
```

`{NULL, 0}` is a valid empty batch; a refused member releases the whole list and leaves the space unchanged.

`mt_run` eagerly executes the program with each row's `group` identifying its `!` form, while `mt_do` runs for effect and discards answers.

```c
mt_do(m, "(= (double $x) (* 2 $x))");
```

`mt_int` returns `int64_t`, printed portably with `<inttypes.h>` or a cast to `long long`.

```c
printf("%" PRId64 "\n", mt_int(a));       /* or cast to long long */
printf("%s\n", mt_show(a));                /* or let the engine write it */
```

## C functions

`mt_def` publishes a callback with designated fields and a required effect class, which the engine uses for caching, reordering, and transactions.

```c
static mt_status op_hypot(mt_call *call, void *user)
{ double a, b;
  mt_clear();
  a = mt_float(mt_arg(call, 0));
  b = mt_float(mt_arg(call, 1));
  if ( !mt_ok() ) return mt_fail(call, "hypot wants two numbers");
  return mt_answer(call, R(hypot(a, b)));
}

mt_def(m, (mt_op){ .name = "hypot", .arity = 2,
                   .effect = MT_PURE, .fn = op_hypot });
```

`(hypot 3.0 4.0)` answers `5.0`.

Names follow C's casing convention, so `word_count` publishes `word-count` as Python's `car_atom` reaches `car-atom`, while names outside C's identifier grammar, such as `prime?` and `%Undefined%`, cross unchanged.

## C values

Wrap a C value once and pass that atom to preserve its engine identity, including for matching and deletion with `mt_keep(handle)`.

```c
mt_atom *handle = mt_object(&account, "account", NULL);
```

| Operation | Identity and lifetime |
|---|---|
| two `mt_object` calls on one pointer | distinct atoms: `==` is `False` and unification fails; Node interns by identity and Python answers `True` |
| ordinary release | SWI blob garbage collection releases the engine reference; `mt_free_fn` runs after the last C reference also goes |
| `mt_object_free(handle)` | consumes that C reference and releases the engine blob immediately; returning an invalidated Prolog alias reports `MT_UNSUPPORTED` |
| other `mt_keep` references | remain valid until dropped |
| MeTTa `get-type` | `%Undefined%`, because this extension declares no `seam:host_object/1` to identify its own values |
| `mt_type` | reads the C type name that MeTTa isn't told |

A C function can also be a value applied wherever it lands.

```c
mt_atom *f = mt_function(fn_triple, NULL, NULL);   /* ($f 5) is 15 */
```

## Lowering

`mt_lower` installs an equation from C tokens, with no quoting or escaped newlines and balanced parentheses checked at compile time.

```c
mt_lower(m, (twice $x), (* 2 $x));
mt_lower(m, (fib $n), (if (< $n 2) $n
                          (+ (fib (- $n 1)) (fib (- $n 2)))));
```

| Form | Source handling |
|---|---|
| `mt_lower`, `MT_METTA` | expand macros before stringifying |
| `mt_lower_raw`, `MT_METTA_RAW` | preserve literal tokens when a MeTTa symbol collides with a C macro |
| Python lowering | reads a function's `__code__` at runtime |
| Node lowering | reads a function's `toString()` at runtime |
| C lowering | uses preprocessor `#` for compile-time access to source |

A lowered equation is an atom the engine reads, type-checks, specialises, and matches, and its calls need no host crossing.

```c
mt_each (a, mt_match(mt_self(m), E("=", E("poly", V("x")), V("body"))))
    puts(mt_show(a));            /* (= (poly $_0) (+ (* 3 $_1) 1)) */
```

The same query finds no equation for an `mt_def` callback, whose opaque body requires its declared effect class.

Parameterising a body by its operators gives one definition callable from both C and MeTTa, as [lower.c](examples/lower.c) demonstrates.

```c
#define POLY(ADD, MUL, x)  ADD(MUL(3, x), 1)
#define C_ADD(a, b) ((a) + (b))
#define C_MUL(a, b) ((a) * (b))
#define M_ADD(a, b) (+ a b)
#define M_MUL(a, b) (* a b)

int64_t poly(int64_t x) { return POLY(C_ADD, C_MUL, x); }
mt_lower(m, (poly $x), POLY(M_ADD, M_MUL, $x));
```

Arbitrary existing C functions can't be lowered: the shared body needs this neutral form, unlike Python's decorator over ordinary Python.

GCC and Clang accept `$x` as an identifier extension; other compilers use the expanded string form, `mt_do(m, "(= (twice $x) (* 2 $x))")`.

## Bounds and counters

`mt_limit` bounds evaluation and reports `MT_LIMIT` separately from faults, preserving writes already made when work stops.

```c
mt_limit(m, (mt_limits){ .seconds = 2.0, .inferences = 1000000 });
if ( !mt_run(m, "!(from 0)") && mt_error() == MT_LIMIT )
    fprintf(stderr, "%s\n", mt_errmsg());   /* you stopped it */
```

| Bound | Scope |
|---|---|
| lazy inference budget | cumulative across the cursor, inside its engine goal because the host can't see that engine's inference count |
| lazy wall bound | per step, excluding host time between steps |

The header records this endless-generator measurement for inference budgets.

| Inferences | Answers before stopping |
|---|---|
| 1,000 | 0 |
| 5,000 | 86 |
| 20,000 | 1,404 |
| 100,000 | 7,118 |

Two samples and a subtraction read the engine's counters in the `getrusage()` style, with deterministic inferences rather than wall-clock timing.

```c
mt_stats before = mt_stats_now(m);
/* ... work ... */
mt_stats spent = mt_stats_since(before, mt_stats_now(m));
printf("%llu inferences\n", (unsigned long long)spent.inferences);
```

## Cleanup

GCC and Clang's `MT_AUTO` releases on block exit, including `return` and `goto`, using the mechanism behind systemd's `_cleanup_` and the kernel's `__free`.

```c
#ifdef MT_HAS_AUTO
  MT_AUTO mt_atom *held = mt_one(mt_eval(m, E("+", 1, 1)));
  MT_AUTO_ASK mt_answers *r = mt_run(m, "!(superpose (1 2 3))");
#endif
```

`MT_TAKE(p)` transfers a value out of an automatic variable without releasing it.

## Threads

| Runtime rule | Contract |
|---|---|
| one runtime per process | `PL_initialise()` sets up one Prolog heap; matching `mt_open` configurations return the same runtime, but a different path fails |
| another thread | call `mt_thread_attach` before engine access and `mt_thread_detach` before exit |
| building and inspecting atoms | no attachment required |
| errors | per-thread state |
| operation table | unguarded; publish before evaluating threads start, as with `sqlite3_create_function()` |

## Extensions

A library includes `cmetta.h`, links against `libcmetta`, and exports `mt_extension_init` for loading by path, following [SQLite's loadable-extension interface](https://www.sqlite.org/loadext.html).

```c
/* solars.c, a library nothing here has heard of */
#include <cmetta.h>

bool mt_extension_init(metta *runtime)
{ mt_provider store = { .user = my_store, .add = store_add,
                        .atom_at = store_atom_at, .clear = store_clear };
  return mt_provider_open(runtime, "&stars", store) &&
         mt_repr(runtime, "star", star_text, NULL) &&
         mt_library(runtime, "solars", "/usr/share/solars/metta");
}
```

`mt_extension` loads and registers the library.

```c
mt_extension(m, "/usr/lib/solars.so");   /* and it is all registered */
```

| Door | What it gives MeTTa |
|---|---|
| `mt_def` | a C function called by name |
| `mt_object` | a live C value by reference |
| `mt_repr` | a C type's printed representation |
| `mt_provider_open` | a space whose atoms the library holds |
| `mt_library` | a directory of MeTTa or Prolog sources |

These registrations use declared extension points, the C counterpart of `engine/ext_points.pl`, with libraries declaring their own points as the driver declares `op`, `repr`, `provider`, and `library`.

| Kind | Reading rule |
|---|---|
| `MT_DECLARATION` | read every row as data |
| `MT_OWNERSHIP` | take the first row whose claim succeeds |
| `MT_EVENT` | run every row |
| `MT_SERVICE` | the driver writes it and a registrant calls it |

`mt_point_declare` declares points, `mt_register` adds rows, `mt_point_at`, `mt_seam_at`, and `mt_seam_count` read them, and `mt_claim` consults ownership rows until one takes the request.

A provider exchanges canonical MeTTa text through the bridge and returns its indexed atom from `atom_at(user, i)` or NULL past the end, letting the engine scan and unify as the Prolog Redis provider does.

C has neither a dataframe notion nor an array interface like Python's Array API/DLPack or JavaScript's `TypedArray`, so numeric libraries carry buffers through `mt_object` without `frame` or `array` extension points.

`tests/shell/test_a_stranger_extends_the_c_seat.sh` builds an external library against `cmetta.h` alone and exercises every extension door.

## Files

| File | Contents |
|---|---|
| `cmetta.h` | public API; the consumer's only include |
| `cmetta.c` | boot, term conversion, cursors, operations |
| `bridge.pl` | Prolog calls to the published engine surface |
| `extension.pl` | declaration read at engine boot |
| `examples/` | [hello.c](examples/hello.c), [ops.c](examples/ops.c), [stream.c](examples/stream.c), [lower.c](examples/lower.c); built and run by the Makefile's test target |
| `tests/` | C suite run by `sh test.sh` and the gate |
| `kit/` | corpus and driver for cross-extension parity |
| `benchmarks/` | C host costs pinned to `baseline.json` |

This seat runs INSIDE the engine's process, so it reads engine terms directly
and has no wire codec. The codec kit that gates the other seats therefore
cannot gate this one, and two things do instead: the C suite in `tests/`, and a
cross-seat parity case that runs `kit/driver` over `kit/corpus.json` and
requires this binding and another seat to answer the same programs identically.
