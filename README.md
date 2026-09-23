<!--
Purpose: show the C API through examples, with cmetta.h as the contract.
Open Obligations: None.
-->

# CMeTTa

A C program boots MeTTa in its own process, builds and reads terms, runs programs, pulls answers, and publishes functions the language can call.

<!-- shared:what-is-metta -->
## What MeTTa is

MeTTa is a language for rewriting metagraphs. A program and its data are the
same thing: atoms in a space, where an atom is a symbol, a number, a variable
or an expression built from other atoms, and a space is the metagraph they
form together.

You write equations rather than statements, and the engine matches a pattern
against the whole space at once. Every match is an answer, so a rule that fits
three ways yields three results, and whether you take one of them, the first,
or all is the caller's choice rather than the language's. Search is something
you write down instead of something you implement.

One space holds symbolic rules and grounded values side by side: a number, a
matrix, a handle to a trained model. A rule can match on what a model produced
and a model can be called from inside a rule, so the neurosymbolic case is
ordinary here rather than an integration between two systems. Both halves are
atoms in the same metagraph, read by the same matcher.
<!-- /shared:what-is-metta -->

## Why C

C is what embedded and systems code is written in, and it is the interface
every other language already knows how to call. A C program opens the engine
in its own process, builds terms, and publishes its own functions for the
language to call back into.

So anything with a C FFI reaches MeTTa through this without a server, a socket
or a runtime to host: a game engine, a database extension, a device that has a
compiler and no interpreter.

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

Install shared and archive libraries into a consumer prefix:

```sh
make install PREFIX="$PWD/build/prefix"
export PKG_CONFIG_PATH="$PWD/build/prefix/lib/pkgconfig"
cc consumer.c $(pkg-config --cflags --libs cmetta) -o consumer
cc consumer.c $(pkg-config --cflags cmetta) \
  "$PWD/build/prefix/lib/libcmetta.a" \
  $(pkg-config --static --libs cmetta) -o archive-consumer
```

`make install-check` runs both consumers with `METTA_PATH` unset and checks
that the archive consumer has no dependency on `libcmetta.so`. SWI-Prolog
remains a shared dependency.

[MesTTo/CMeTTa](https://github.com/MesTTo/CMeTTa) is the C driver beside `extensions/python` and `extensions/node`, separate from the vendored CeTTa substrate.

| Driver | Engine access | Terms |
|---|---|---|
| C | embedded SWI-Prolog | `term_t` through `PL_get_*`, no wire codec |
| Python | janus | tagged arrays from `CODEC.md` |
| Node | WebAssembly | tagged arrays from `CODEC.md` |

## The surface

[cmetta.h](https://github.com/MesTTo/CMeTTa/blob/main/cmetta.h) defines every name and signature; [llms.txt](llms.txt) is the compact usage reference.

| Feature | Doors |
|---|---|
| Runtime | `mt_open`, `mt_close`, `mt_verbose`, `mt_thread_attach`, `mt_thread_detach`, `mt_version` |
| Constructors, marked `MT_MUST_USE` | `mt_sym`, `mt_var`, `mt_text`, `mt_textn`, `mt_num`, `mt_real`, `mt_bool`, `mt_unit`, `mt_bigint`, `mt_rational`, `mt_spaceref`, `mt_exprv`, `mt_object`, `mt_function` |
| C argument conversion | `mt_expr`, `mt_atom_of`; helpers `mt_num_`, `mt_real_`, `mt_same`, `mt_same_c` |
| References | `mt_keep`, `mt_drop` |
| Inspection | `mt_kind_of`, `mt_kind_str`, `mt_name`, `mt_name_len`, `mt_int`, `mt_float`, `mt_truth`, `mt_ratio_of`, `mt_len`, `mt_at`, `mt_eq`, `mt_alpha_eq`, `mt_compare`, `mt_order`, `mt_hash` |
| Unification | `mt_unify`, `mt_unifyv`, `mt_bindings_len`, `mt_binding`, `mt_binding_var`, `mt_binding_value`, `mt_bindings_free`, `mt_substitute` |
| Spaces | `mt_self`, `mt_catalog`, `mt_space_open`, `mt_space_close`, `mt_space_drop`, `mt_space_name` |
| Closed scopes | `mt_transaction`, `mt_speculate` |
| Standing queries | `mt_subscribe`, `mt_unsubscribe` |
| Native producers | `mt_iterator`, `mt_answers_from`, `mt_answer_iter`, `mt_stream`, `mt_stream_of`, `mt_step`, `mt_answers_status` |
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
| `mt_run` | `mt_self_run` | `mt_space_run` |
| `mt_load` | `mt_self_load` | `mt_space_load` |
| `mt_do` | `mt_self_do` | `mt_space_do` |
| `mt_query` | `mt_self_query` | `mt_space_query` |
| `mt_eval_under` | `mt_self_eval_under` | `mt_space_eval_under` |

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

Version 1 installs as `libcmetta.so.1`. Rebuild consumers and extensions together.
`mt_alloc`, `mt_calloc` and `mt_resize` allocate transferable list storage;
`mt_free` releases it through the allocator retained with each block. An
allocator selected by `mt_allocator_set` must outlive its outstanding blocks.
Atom teardown uses no allocation and no recursive calls.

`mt_text_ref` borrows immutable counted text; `mt_expr_ref` borrows an immutable
child vector and retains its children. `mt_children` reads that vector directly.
Their optional owner callback runs at the last reference. Failed borrowing
leaves the owner with the caller; `mt_object` and `mt_function` instead take
their resource on every path, including failure.

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
| `MT_HANDLE` | an engine value with no MeTTa structure, such as a partial application, held by reference: it prints as the engine prints it and goes back as the identical value |

C splits the codec's Number tag into four kinds, and reading promotes only where lossless: `mt_float` accepts an Int within 2^53 and refuses one beyond it, while `mt_int` refuses a Float instead of rounding.

`mt_eq` compares structure and `mt_hash` supplies its fast, non-cryptographic 64-bit hash, with equal hashes for equal atoms including distinct NaN payloads and atoms sharing C-object identity.

`mt_alpha_eq` compares up to a consistent renaming of variables, MeTTa's
`=alpha`: `(f $x $y)` matches `(f $a $b)` but not `(f $a $a)`, and each `_` is
a variable of its own. It is how a program asks whether an answer is the atom
it expected when the answer's variables carry engine names.

`mt_compare` is the engine's standard order of terms, the order `msort`
answers in, exact across integers, ratios, big integers and floats; `mt_order`
is the same order shaped for `qsort`:

```c
mt_list all = mt_all(mt_atoms(kb));
qsort(all.items, all.len, sizeof *all.items, mt_order);
```

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
| `mt_forms` | all source forms without executing directives; no prefix on syntax failure |
| `mt_show` | presentation text in a per-thread rotating buffer, ready for `printf` |
| `mt_show_dup` | an owned presentation copy |
| `mt_write_dup` | owned, counted `mt_string` source, refusing values whose display spelling wouldn't read back equally |
| `mt_free` | releases copied text, including `mt_string.data` |

Logical text and names use UTF-8, refusing malformed byte sequences. Unsigned
arguments and `mt_unum` retain all 64 bits; `mt_bigint` canonicalizes digits.

Embedded NUL survives `mt_write_dup` followed by `mt_parsen(written.data, written.len)`.

## Answers

Outside a closed transaction, `mt_eval` computes at most one answer per step, and `mt_each` closes on exhaustion or `break`, leaving an endless generator's remaining answers uncomputed.

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

An answer keeps the engine's variable identity. Every occurrence of one engine
variable is one name: the source name where the answer carries one, otherwise
a fresh `_N` from a counter shared by the whole process, so `(fact $u $u $w)`
reads back as `(fact $_3 $_3 $_4)` and two answers never share a variable.
An equation read back through C therefore still computes where it is copied.

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

A native producer supplies `mt_iterator {state, next, close}`. `next` yields an
owned atom with `MT_ROW`, ends with `MT_DONE`, or returns an error set by
`mt_error_set`. `mt_answers_from` takes the iterator; `close` runs once on
exhaustion, error or abandonment. `mt_answer_iter` uses this protocol inside a
host callback and retains the callback arguments until close. A callable value
returns a consumptive stream, read through `(c-iter value)` or `mt_stream_of`.
`mt_step` and `mt_answers_status` distinguish exhaustion from failure.

`mt_space_close` releases the C handle; `mt_space_drop` retires the engine space
and its definitions. `mt_del` removes one unifying occurrence, refusing a bare
variable; `(remove-atom ...)` is the language's operation for draining a pattern.

## Queries, joins and guards

`mt_query(kb, pattern, guard)` means `(match space pattern (if guard pattern Empty))`.
The engine joins shared variables and evaluates the guard; NULL means `True`.
Both atom arguments transfer ownership to the call. The cursor returns instances
of the pattern, so `mt_bound(row, "x")` reads the engine's binding.

```c
mt_atom *pattern = mt_expr(",",
    mt_expr("Parent", mt_var("x"), mt_var("y")),
    mt_expr("Parent", mt_var("y"), mt_var("z")));
mt_rows (row, mt_query(kb, mt_keep(pattern),
                      mt_expr("==", mt_var("x"), "Tom")))
    printf("%s\n", mt_show(mt_bound(row, "z")));
mt_drop(pattern);
```

With `(Parent Tom Bob)` and `(Parent Bob Ann)`, this prints `Ann`.
`mt_match` remains the primitive stored-pattern lookup; `mt_query` reaches the
language matcher, including conjunctions.

## Prepared queries and temporary facts

A prepared query is an immutable pattern you retain with `mt_keep`. Each call
to `mt_query` opens a fresh engine query against current facts. There is no
separate prepared-object cache to invalidate. For a temporary assumption, add
facts and collect the query inside `mt_speculate`; all engine writes made in
that callback are discarded. Collect inside the callback when the answers
must survive its end, as [language.c](https://github.com/MesTTo/CMeTTa/blob/main/examples/language.c) demonstrates.

## Transactions and speculation

`mt_transaction(m, callback, user)` invokes `mt_scope_fn` once. `MT_OK` commits;
`MT_FAIL` rolls back; an error rolls back and preserves its reason.
`mt_speculate` uses the same callback and always discards changes. Nested scopes
are savepoints. The engine decides the transaction, including provider
participation and buffered events; C retains its callback registrations until
that verdict is known.

```c
static mt_status store_pair(metta *m, void *user)
{
    mt_space *kb = user;
    (void)m;
    if (!mt_add(kb, mt_expr("edge", 1, 2))) return mt_error();
    if (!mt_add(kb, mt_expr("edge", 2, 3))) return mt_error();
    return MT_OK;
}
/* mt_transaction(m, store_pair, kb) publishes both facts together. */
```

The callback and `user` are borrowed for the synchronous call. Queries opened
inside the closed scope collect through `metta_host_hold`: commit retains their
rows and rollback discards them. Set an inference bound before asking an endless
query inside such a scope. Arbitrary C memory and I/O need the caller's recovery
protocol. A transaction started only inside a MeTTa expression cannot own a C
registration snapshot; enter every enclosing scope through `mt_transaction`
before changing C registrations.

## Materialized answers

`mt_all(cursor)` closes the cursor and returns an owned `mt_list`. Its atoms
remain readable after later writes; `mt_list_free` releases the snapshot.
A failure on a later pull discards the entire collection and preserves the
error. This is a snapshot. Python's automatically maintained answer view needs
a shared engine service before C can expose it without implementing that
maintenance algorithm again.

## Mutable cells

Cells are engine atoms. The same reference is passed to `new-state`,
`get-state` and `change-state!` through ordinary term construction.

```c
mt_atom *cell = mt_one(mt_eval(m, mt_expr("new-state", 0)));
bool changed = mt_one_truth(mt_eval(m,
    mt_expr("change-state!", mt_keep(cell), 7)));
int64_t value = mt_one_int(mt_eval(m, mt_expr("get-state", mt_keep(cell))));
/* changed == true; value == 7 */
mt_drop(cell);
```

Dropping the atom releases C's reference. The engine owns the cell's storage.
The current engine stores cell values as transactional facts: writes inside
`mt_speculate` are discarded, while `mt_transaction` commits them on success.

## Typed atoms, arrays and shape types

Language types are atoms such as `(: shape-left (Matrix 2 3))`.
`mt_type` names a C representation; `(get-type value)` asks the language.
Shared variables in an arrow type relate dimensions across arguments and result:

```c
mt_do(kb, "(: shape-left (Matrix 2 3)) (: shape-right (Matrix 3 4)) "
          "(: shape-product (-> (Matrix $m $k) (Matrix $k $n) (Matrix $m $n)))");
mt_atom *shape = mt_one(mt_eval(kb,
    mt_expr("get-type", mt_expr("shape-product", "shape-left", "shape-right"))));
/* shape is (Matrix 2 4). */
mt_drop(shape);
```

For native array memory, `mt_object` retains the pointer through a release
callback; `mt_expr_ref` exposes an immutable child vector. An external adapter
supplies dimensions, strides and operations. The core does not infer them from
an address or select an array library.

## Algebras and semirings

`mt_eval_under(target, algebra, goal)` scopes the engine's algebra selection and
returns ordinary `(value coefficient)` pairs. It takes both atom arguments.
The cursor owns each pair until the next step; `mt_keep` or `mt_one` retains it.

```c
mt_atom *answer = mt_one(mt_eval_under(m, mt_sym("tropical"), mt_expr("+", 2, 3)));
/* answer is (5 0): tropical's multiplicative identity is 0. */
mt_drop(answer);
```

Algebras are `(algebra ...)` rows in `mt_catalog(m)`, naming operations, identities,
laws, carrier and scope. An unknown algebra raises an error with the remedy to
declare it. Closing, exhausting or failing a cursor restores the prior selection.
The coefficients and their validation come from `metta_with_under` and
`metta_annotation`. Python's tagged-proof traversal, reinterpretation and its
special aggregate result for `counting` are host algorithms; this door does not
claim those services exist in the shared engine.

## Reified worlds and compensation

The engine publishes effect coverage, compensation lookup and transaction
services. Complete world reification, diff, commit, and saga receipt recovery
currently live in Python. They require shared engine services before C can
offer them under the same language semantics. There are no success-shaped
substitutes: a service C cannot yet offer refuses by name rather than
returning something an unwary caller would read as an answer.

## C functions

Inspect a term before executing it:

```c
mt_atom *plan = mt_effect_plan(m, mt_parse("(add-atom &self (item 7))"));
/* (EffectPlan writesState ((add-atom writesState))) */
mt_drop(plan);
```

The shared planner reads source masks and compilation effects. It does not
execute the requested write; dynamic calls receive a conservative class.

`mt_matcher` supplies custom matching for a grounded value. Its callback
borrows the other operand and returns candidate atoms. The engine unifies each
candidate with that operand, so a candidate `(value 7)` binds `$x` in
`(unify matcher (value $x) $x Empty)` to `7`. `MT_FAIL` declines a match;
`mt_fail` reports an error. `mt_answer_iter` streams candidates and closes on
exhaustion or abandonment. The release callback owns the same lifetime as
`mt_object`; a free variable binds the matcher whole without calling it.

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

Names cross exactly: `word_count` and `word-count` remain different names. The `mt_` convention names this API; it never rewrites a published language name.

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
| MeTTa `get-type` | reads the exact type symbol through `seam:host_object/1` and `seam:grounded_class_type/2`; NULL contributes no type candidate |
| `mt_type` | borrows that same C type name |

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
    puts(mt_show(a));            /* (= (poly $_0) (+ (* 3 $_0) 1)) */
```

The same query finds no equation for an `mt_def` callback, whose opaque body requires its declared effect class.

Parameterising a body by its operators gives one definition callable from both C and MeTTa, as [lower.c](https://github.com/MesTTo/CMeTTa/blob/main/examples/lower.c) demonstrates.

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
                        .match = store_match, .clear = store_clear };
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

`tests/extension_fixture.c` builds separately against the public header; `tests/test_extensions.c` checks successful calls and failed initialization rollback.

## Foreign spaces, row cursors and live objects

A provider receives borrowed atoms directly. `match(user, pattern, limit, out)`
opens a candidate `mt_iterator`; the engine unifies every candidate, including
repeated variables. A variable pattern requests enumeration. The bound is
advisory and must not truncate an over-approximation before unification.
Duplicate candidates remain duplicate answers. `remove` reports one occurrence
through its boolean output. A NULL callback declines that capability.

Supply `begin`, `commit` and `rollback` together when the store can participate
in transactions. The engine captures the selected provider before beginning;
withdrawal or reuse of its name cannot redirect completion. Query cursors also
retain the provider through close. `mt_provider_open` takes its user resource
on success and failure.

This is also C's table interface: a backend opens a row cursor and converts each
row to an owned atom. The core names no database or dataframe library. A provider
may read fields from a live C struct whenever a query opens; opaque fields pass
through `mt_object`, preserving pointer identity. The borrowed query pattern
stays alive until the iterator closes, and returned atoms follow `mt_keep`/`mt_drop`.

## Composed spaces

Composition is an engine query over space values. For example, the source
`(match (superpose (&front &back)) (item $x) $x)` reads both stores and retains
their multiplicity. Reusing that term reads later writes from either source.
An overlay uses that read expression and sends writes explicitly to `&front`.
A C provider can expose that cursor behind a named, read-only space through
the same foreign-space interface. Python's mapped-view policy and object-field
discovery are host conveniences; C providers state their field shape explicitly.

## Events and standing queries

`mt_subscribe(m, name, subscription)` registers a pattern and C callback over
committed additions and removals. The callback receives a borrowed atom and a
boolean edge; it may retain the atom or cancel itself. `mt_unsubscribe` closes
the registration. Callbacks run on the writer's thread, and the caller owns any
queue or scheduler. Rollback delivers nothing. A callback error is reported
after commit and does not undo the write. Close a subscription before reusing
its watched space's name. See `tests/test_subscriptions.c`.

Both registration arguments transfer their owned pattern and callback resource
on every path. `mt_seam_at(m, "subscription", index)` inspects the active machinery
as data. [language.c](https://github.com/MesTTo/CMeTTa/blob/main/examples/language.c) checks that speculation emits no event
and one committed addition emits one event.

## Integrations, async and network entry points

Native producers and providers expose incremental rows without requiring a
scheduler. A C application owns its worker threads, queues and cancellation;
workers attach through `mt_thread_attach`, then close cursors before detaching.
HTTP, GraphQL, authentication and third-party array/database adapters belong to
external libraries loaded through `mt_extension`. There is no hidden server,
event loop or Python runtime in this binding.

`tests/extension_fixture.c` compiles separately against the public header.
Its initializer either publishes a callable, equation and source library or
refuses after publication. `tests/test_extensions.c` checks rollback and the
successful calls. Initialization cannot roll back arbitrary external I/O.

## Files

| File | Contents |
|---|---|
| `cmetta.h` | public API; the consumer's only include |
| `cmetta.c` | boot, term conversion, cursors, operations |
| `bridge.pl` | Prolog calls to the published engine surface |
| `extension.pl` | declaration read at engine boot |
| `examples/` | [hello.c](https://github.com/MesTTo/CMeTTa/blob/main/examples/hello.c), [ops.c](https://github.com/MesTTo/CMeTTa/blob/main/examples/ops.c), [stream.c](https://github.com/MesTTo/CMeTTa/blob/main/examples/stream.c), [lower.c](https://github.com/MesTTo/CMeTTa/blob/main/examples/lower.c), [language.c](https://github.com/MesTTo/CMeTTa/blob/main/examples/language.c); built and run by the Makefile's test target |
| `tests/` | C suite run by `sh test.sh` and the gate |
| `kit/` | corpus and driver for cross-extension parity |
| `benchmarks/` | C host costs pinned to `baseline.json` |

This seat runs INSIDE the engine's process, so it reads engine terms directly
and has no wire codec. The codec kit that gates the other seats therefore
cannot gate this one, and two things do instead: the C suite in `tests/`, and a
cross-seat parity case that runs `kit/driver` over `kit/corpus.json` and
requires this binding and another seat to answer the same programs identically.
