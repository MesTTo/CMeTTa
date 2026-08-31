# The C binding

MeTTa from C. A C program boots the MeTTa engine in its own process, builds
and reads terms as C values, runs programs, pulls answers one at a time, and
publishes C functions the language can call.

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

Build with `sh build.sh`, test with `sh test.sh`. It needs a C compiler and
SWI-Prolog's development headers; `swipl --dump-runtime-variables` is how the
Makefile finds them.

## Where this seat sits

`extensions/` holds one folder per driver of the engine, and this is the C
one, beside `python` and `node`. It is not the vendored CeTTa C substrate,
which is a different track: an extension under `extensions/` DRIVES the
engine.

What makes this seat different from the other two is that it is IN the
engine's process. Python reaches the engine through janus and Node through a
WebAssembly build, so both have a language boundary to cross and both encode
every term into the tagged arrays `CODEC.md` describes. C has no boundary: it
reads `term_t` directly with `PL_get_*`. There is no wire codec here, and that
is the reason the seat exists.

## Five rules, and then you know the library

Everything below is one of these five. They are in `cmetta.h` too, at the top.

**1. `const` borrows, non-`const` takes.** Every door you hand a freshly built
term to TAKES it, so the common shape leaks nothing and needs no cleanup line:

```c
mt_add(kb, mt_expr("edge", "a", "b"));
```

To pass a term you mean to keep, hand over a new reference with `mt_keep()`.
That is the one thing to remember:

```c
mt_atom *p = mt_expr("edge", "a", mt_var("y"));
while (...) mt_each (row, mt_match(kb, mt_keep(p))) ...
mt_drop(p);
```

**2. Errors are `errno`-shaped.** A function that produces a value returns it,
or NULL. `mt_error()` and `mt_errmsg()` say what went wrong, and like
`errno` they are set on failure and not cleared on success, so a run of calls
is checked once rather than one `if` per call:

```c
mt_clear();
double x = mt_float(mt_arg(c, 0));
double y = mt_float(mt_arg(c, 1));
if ( !mt_ok() ) return mt_fail(c, "wanted two numbers");
```

**3. One verb, either receiver.** `mt_eval`, `mt_match`, `mt_atoms`,
`mt_add`, `mt_del`, `mt_count` and `mt_wipe` each take a `metta *`,
meaning its `&self`, or a `mt_space *`. `_Generic` picks, the way `tgmath.h`
does; the pair it picks between is declared beside each one.

**4. A Number splits four ways, and reading promotes only where it is
lossless.** `MT_INT`, `MT_FLOAT`, `MT_BIGINT` and `MT_RATIONAL`,
because C has types where the wire codec has one tag. `mt_float()` of an Int
answers that integer; `mt_int()` of a Float does not round; an Int past 2^53
is refused by `mt_float()` rather than silently rounded.

**5. A bare C string in term position is a symbol.** `mt_expr("+", 1, 2)` is
`(+ 1 2)`, not `("+" 1 2)`. MeTTa writes a symbol bare and a string quoted; in
C everything is quoted, so the default is the one MeTTa writes bare. Text is
`mt_text("...")`.

## Building terms

No count to keep in step, no constructor per child:

```c
mt_expr("+", 1, 2)                     /* (+ 1 2)       */
mt_expr("edge", "a", mt_var("y"))      /* (edge a $y)   */
mt_expr("f", mt_expr("g", 1), 2.5)     /* (f (g 1) 2.5) */
```

`_Generic` reads each argument's C type: an integer becomes a Number, a float a
Number, a bare string a Symbol, and an atom itself. If any child fails the
whole call fails and drops the ones it was given, so a failure part-way through
a nested build cannot leave you holding half a term.

`#define MT_SHORTHAND` before the include for the one-letter builders,
`S() V() T() N() R() B() E()`. They are opt-in because those are short names in
C's single flat namespace. The long names always work.

| kind | what it is |
|---|---|
| `MT_SYMBOL` | a name that denotes itself |
| `MT_TEXT` | grounded text |
| `MT_INT` | an exact integer that fits `int64_t` |
| `MT_FLOAT` | a float; `2` and `2.0` are different atoms |
| `MT_BIGINT` | an exact integer too wide for `int64_t`, read as digits |
| `MT_RATIONAL` | an exact ratio |
| `MT_BOOL` | `True` or `False`, which are not symbols |
| `MT_VARIABLE` | a variable; the name is an identity within its term |
| `MT_EXPR` | an expression; the empty one is unit |
| `MT_SPACE` | an executable space reference |
| `MT_OBJECT` | a live C value crossing by reference |
| `MT_HANDLE` | a native engine value held by reference |

Building and reading them starts no engine. `mt_parse()` and `mt_show()`
do, because text goes through the engine's own reader and writer rather than a
second one grown here. `mt_show()` writes into a per-thread rotating buffer
so it drops straight into `printf`, which is the contract `strerror()` already
gave C; `mt_show_dup()` gives you a copy to keep.

## Answers are stepped, not drained

`mt_eval()` computes one answer per step, so an endless generator is
ordinary. `mt_each` closes the cursor however the loop is left, `break`
included:

```c
mt_each (a, mt_eval(m, E("from", 0)))
{ printf("%lld\n", (long long)mt_int(a));
  if ( ++taken == 5 ) break;          /* the sixth is never computed */
}
```

Use `mt_rows` when you want the whole answer rather than the atom alone. It
binds an `mt_row`, which carries the atom, the engine's own rendering of it,
the `!` group it came from, and the cursor:

```c
typedef struct mt_row {
  const mt_atom *atom;   /* the answer itself                     */
  const char    *text;   /* the engine's own rendering            */
  size_t         group;  /* which `!` form produced it            */
  mt_answers    *of;     /* the cursor, so mt_bound takes the row */
} mt_row;
```

`mt_bound` is what saves you counting children. The cursor keeps the pattern it
was opened with, so a binding comes back under the name you wrote:

```c
mt_rows (row, mt_match(kb, E("edge", "a", V("y"))))
    printf("y = %s\n", mt_show(mt_bound(row, "y")));
```

rather than `mt_at(row, 2)` and a comment explaining why 2. It works at any
depth in the pattern and costs one walk of the term, no engine call. The Python
seat spells the same thing `row.y`, and draws the same line this does between
iterating `Answers` and iterating `Rows`.

When you want one value rather than a walk:

| door | what it claims |
|---|---|
| `mt_one(r)` | EXACTLY one answer, owned; refuses zero or many |
| `mt_first(r)` | the first, owned; claims nothing about the rest |
| `mt_one_int(r)`, `_float`, `_truth`, `_name` | the value, no atom in your hands |
| `mt_all(r)` | every answer, as an `mt_list` of items and length |

Each consumes the cursor. `one` and `first` draw the same line the Python seat
draws between `one()` and `first()`.

`mt_run()` is the eager door, because running a program means running it, and
each row's `group` says which `!` form the answer came from. When the point is
the effect rather than the answers, `mt_do(m, src)` runs and discards:

```c
mt_do(m, "(= (double $x) (* 2 $x))");
```

## Printing a number

`mt_int` answers an `int64_t`, and printing one portably wants `<inttypes.h>`:

```c
printf("%" PRId64 "\n", mt_int(a));       /* or cast to long long */
printf("%s\n", mt_show(a));                /* or let the engine write it */
```

## Publishing C functions

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

`(hypot 3.0 4.0)` now answers `5.0`. Designated initializers are what C has
instead of keyword arguments, and they are why the effect class is readable at
the call site rather than being the third of five positional arguments. Naming
it is required, not advisory: the engine reasons about caching, reordering and
transactions from it.

The name reaches MeTTa through C's own casing convention, so `word_count`
publishes `word-count`, exactly as Python's `car_atom` reaches `car-atom`. A
name outside C's identifier grammar crosses untouched, which is the escape for
`prime?` and `%Undefined%`.

A C value can cross MeTTa untouched and come back the same object:

```c
mt_atom *handle = mt_object(&account, "account", NULL);
```

Each call makes ONE value, and this seat does not intern: two `mt_object` calls
on the same pointer are two atoms that answer `False` to `==` and fail to
`unify`, where the Node seat interns by identity and Python answers `True`. Wrap
once and pass the atom. The blob is released by SWI's garbage collector through
the `mt_free_fn` you hand it, so not interning costs no memory; what it costs is
that comparison. `get-type` answers `%Undefined%` for one, because this seat
declares no `seam:host_object/1`, the seam by which a host tells the engine a
value is its own; `mt_type()` is how C reads the name back, and MeTTa is not
told it.

and a C function can be a value rather than a name, applied wherever it lands:

```c
mt_atom *f = mt_function(fn_triple, NULL, NULL);   /* ($f 5) is 15 */
```

## Lowering: C source becoming MeTTa

`mt_def` publishes a C function the engine CALLS. `mt_lower` installs an
EQUATION, which is a different thing:

```c
mt_lower(m, (twice $x), (* 2 $x));
mt_lower(m, (fib $n), (if (< $n 2) $n
                          (+ (fib (- $n 1)) (fib (- $n 2)))));
```

The body is C tokens the compiler saw, so there is no quoting, no escaped
newlines, and unbalanced parentheses are a compile error rather than a runtime
one. The preprocessor is what makes this possible: Python lowers by reading a
function's `__code__` and Node by reading its `toString()`, and C has neither
at run time but has `#`, which is access to the program's own source at the
one moment C offers it.

The difference from `mt_def` is what the engine can see. A published function
is opaque, which is why it must declare an effect class. An equation is MeTTa,
so the engine reads it, type-checks it, specialises it, and it is an atom in
the space like any other:

```c
mt_each (a, mt_match(mt_self(m), E("=", E("poly", V("x")), V("body"))))
    puts(mt_show(a));            /* (= (poly $_0) (+ (* 3 $_1) 1)) */
```

The same query against an `mt_def` name finds nothing. A lowered call also
crosses into no host at all.

**One body, both languages.** Parameterise the body by its operators and it
expands to C in one mode and MeTTa in the other, so the function exists once
and is callable from both:

```c
#define POLY(ADD, MUL, x)  ADD(MUL(3, x), 1)
#define C_ADD(a, b) ((a) + (b))
#define C_MUL(a, b) ((a) * (b))
#define M_ADD(a, b) (+ a b)
#define M_MUL(a, b) (* a b)

int64_t poly(int64_t x) { return POLY(C_ADD, C_MUL, x); }
mt_lower(m, (poly $x), POLY(M_ADD, M_MUL, $x));
```

That is what the other seats' twins buy, bought the way C buys things.
`examples/lower.c` runs all of it.

What is out of reach: an ARBITRARY existing C function cannot be lowered. The
body has to be written in the neutral form, where Python's decorator lowers a
function written in ordinary Python.

`$x` tokenizes because GCC and Clang admit `$` in an identifier. Without that
extension, use the string form, which is what this expands to:
`mt_do(m, "(= (twice $x) (* 2 $x))")`.

## Bounding and measuring

An embedded engine that cannot be stopped is a hazard, so bounds are part of
the surface:

```c
mt_limit(m, (mt_limits){ .seconds = 2.0, .inferences = 1000000 });
if ( !mt_run(m, "!(from 0)") && mt_error() == MT_LIMIT )
    fprintf(stderr, "%s\n", mt_errmsg());   /* you stopped it */
```

`MT_LIMIT` is its own status precisely because a bound is not a fault. On a
lazy cursor the inference bound is a cumulative budget for the whole cursor,
built into the goal the engine runs, so a big budget really does buy more steps
than a small one: over an endless generator, budgets of 1,000 / 5,000 / 20,000
/ 100,000 stop after 0 / 86 / 1,404 / 7,118 answers. It cannot be metered from
out here, because an engine counts its own inferences and this process cannot
see them. The wall bound applies per step, so time the host spends between steps
does not count against it. A bound stops work MID-WAY and writes already made
stand, which is the honest semantics of every timeout.

Measuring uses the engine's own counters, and inferences are deterministic
where wall clock is not:

```c
mt_stats before = mt_stats_now(m);
/* ... work ... */
mt_stats spent = mt_stats_since(before, mt_stats_now(m));
printf("%llu inferences\n", (unsigned long long)spent.inferences);
```

Two samples and a subtraction, because C has no `with` block and this is the
shape `getrusage()` already gave it.

## Scope cleanup

Where GCC and Clang have it, `MT_AUTO` releases a variable however the block
is left, `return` and `goto` included. This is systemd's `_cleanup_` and the
kernel's `__free`:

```c
#ifdef MT_HAS_AUTO
  MT_AUTO mt_atom *held = mt_one(mt_eval(m, E("+", 1, 1)));
  MT_AUTO_ASK mt_answers *r = mt_run(m, "!(superpose (1 2 3))");
#endif
```

`MT_TAKE(p)` hands a value out of such a variable without it being released.

## Threads

One runtime per process, because `PL_initialise()` sets up the process's single
Prolog heap. A second `mt_open()` with a matching configuration hands back
the same runtime; one with a different path fails.

A thread other than the one that opened the runtime calls
`mt_thread_attach()` before it touches the engine and
`mt_thread_detach()` before it exits. Building and reading atoms needs
neither, and the error state is per-thread.

The operation table is not guarded: publish every operation before the threads
that evaluate start, the same restriction `sqlite3_create_function()` carries.

## Layout

| file | what it is |
|---|---|
| `cmetta.h` | the public API, and the only file a consumer includes |
| `cmetta.c` | the C half: boot, term conversion, cursors, ops |
| `bridge.pl` | the Prolog half, calling published engine surface only |
| `extension.pl` | the seat declaration the engine reads at boot |
| `examples/` | `hello`, `ops`, `stream`, `lower` |
| `tests/` | the C suite, run by `sh test.sh` and by the gate |
| `kit/` | the corpus and driver the cross-seat parity test uses |
| `benchmarks/` | what a C host pays, pinned to `baseline.json` |

The Python seat's `test_c_binding.py` runs both this seat and the Python host
over `kit/corpus.json` and requires the same answers.

Constraints and issues found while building this are recorded in
`ai-cmetta-c-constraints.md` at the repository root.
