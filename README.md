# The C binding

MeTTa from C. A C program boots the PeTTa engine in its own process, builds
and reads terms as C values, runs programs, pulls answers one at a time, and
publishes C functions the language can call.

```c
#define CETTA_SHORTHAND
#include <cetta.h>
#include <stdio.h>

int main(void)
{ cetta *m = cetta_open(NULL);

  cetta_each (a, cetta_run(m, "(= (double $x) (* 2 $x))\n!(double 21)"))
      printf("%s\n", cetta_show(a));                 /* 42 */

  printf("%lld\n", (long long)cetta_one_int(cetta_eval(m, E("+", 1, 2))));

  cetta_close(m);
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

Everything below is one of these five. They are in `cetta.h` too, at the top.

**1. `const` borrows, non-`const` takes.** Every door you hand a freshly built
term to TAKES it, so the common shape leaks nothing and needs no cleanup line:

```c
cetta_add(kb, cetta_expr("edge", "a", "b"));
```

To pass a term you mean to keep, hand over a new reference with `cetta_keep()`.
That is the one thing to remember:

```c
cetta_atom *p = cetta_expr("edge", "a", cetta_var("y"));
while (...) cetta_each (row, cetta_match(kb, cetta_keep(p))) ...
cetta_drop(p);
```

**2. Errors are `errno`-shaped.** A function that produces a value returns it,
or NULL. `cetta_error()` and `cetta_errmsg()` say what went wrong, and like
`errno` they are set on failure and not cleared on success, so a run of calls
is checked once rather than one `if` per call:

```c
cetta_clear();
double x = cetta_float(cetta_arg(c, 0));
double y = cetta_float(cetta_arg(c, 1));
if ( !cetta_ok() ) return cetta_fail(c, "wanted two numbers");
```

**3. One verb, either receiver.** `cetta_eval`, `cetta_match`, `cetta_atoms`,
`cetta_add`, `cetta_del`, `cetta_count` and `cetta_wipe` each take a `cetta *`,
meaning its `&self`, or a `cetta_space *`. `_Generic` picks, the way `tgmath.h`
does; the pair it picks between is declared beside each one.

**4. A Number splits four ways, and reading promotes only where it is
lossless.** `CETTA_INT`, `CETTA_FLOAT`, `CETTA_BIGINT` and `CETTA_RATIONAL`,
because C has types where the wire codec has one tag. `cetta_float()` of an Int
answers that integer; `cetta_int()` of a Float does not round; an Int past 2^53
is refused by `cetta_float()` rather than silently rounded.

**5. A bare C string in term position is a symbol.** `cetta_expr("+", 1, 2)` is
`(+ 1 2)`, not `("+" 1 2)`. MeTTa writes a symbol bare and a string quoted; in
C everything is quoted, so the default is the one MeTTa writes bare. Text is
`cetta_text("...")`.

## Building terms

No count to keep in step, no constructor per child:

```c
cetta_expr("+", 1, 2)                        /* (+ 1 2)        */
cetta_expr("edge", "a", cetta_var("y"))      /* (edge a $y)    */
cetta_expr("f", cetta_expr("g", 1), 2.5)     /* (f (g 1) 2.5)  */
```

`_Generic` reads each argument's C type: an integer becomes a Number, a float a
Number, a bare string a Symbol, and an atom itself. If any child fails the
whole call fails and drops the ones it was given, so a failure part-way through
a nested build cannot leave you holding half a term.

`#define CETTA_SHORTHAND` before the include for the one-letter builders,
`S() V() T() N() R() B() E()`. They are opt-in because those are short names in
C's single flat namespace. The long names always work.

| kind | what it is |
|---|---|
| `CETTA_SYMBOL` | a name that denotes itself |
| `CETTA_TEXT` | grounded text |
| `CETTA_INT` | an exact integer that fits `int64_t` |
| `CETTA_FLOAT` | a float; `2` and `2.0` are different atoms |
| `CETTA_BIGINT` | an exact integer too wide for `int64_t`, read as digits |
| `CETTA_RATIONAL` | an exact ratio |
| `CETTA_BOOL` | `True` or `False`, which are not symbols |
| `CETTA_VARIABLE` | a variable; the name is an identity within its term |
| `CETTA_EXPR` | an expression; the empty one is unit |
| `CETTA_SPACE` | an executable space reference |
| `CETTA_OBJECT` | a live C value crossing by reference |
| `CETTA_HANDLE` | a native engine value held by reference |

Building and reading them starts no engine. `cetta_parse()` and `cetta_show()`
do, because text goes through the engine's own reader and writer rather than a
second one grown here. `cetta_show()` writes into a per-thread rotating buffer
so it drops straight into `printf`, which is the contract `strerror()` already
gave C; `cetta_show_dup()` gives you a copy to keep.

## Answers are stepped, not drained

`cetta_eval()` computes one answer per step, so an endless generator is
ordinary. `cetta_each` closes the cursor however the loop is left, `break`
included:

```c
cetta_each (a, cetta_eval(m, E("from", 0)))
{ printf("%lld\n", (long long)cetta_int(a));
  if ( ++taken == 5 ) break;          /* the sixth is never computed */
}
```

Use `cetta_each_cursor (a, it, ...)` when the body needs the cursor itself, for
`cetta_group(it)` or `cetta_answer_text(it)`.

When you want one value rather than a walk:

| door | what it claims |
|---|---|
| `cetta_one(r)` | EXACTLY one answer, owned; refuses zero or many |
| `cetta_first(r)` | the first, owned; claims nothing about the rest |
| `cetta_one_int(r)`, `_float`, `_truth`, `_name` | the value, no atom in your hands |
| `cetta_all(r, &n)` | every answer as one owned array |

Each consumes the cursor. `one` and `first` draw the same line the Python seat
draws between `one()` and `first()`.

`cetta_run()` is the eager door, because running a program means running it;
its answers carry `cetta_group()` saying which `!` form produced each.

## Publishing C functions

```c
static cetta_status op_hypot(cetta_call *call, void *user)
{ double a, b;
  cetta_clear();
  a = cetta_float(cetta_arg(call, 0));
  b = cetta_float(cetta_arg(call, 1));
  if ( !cetta_ok() ) return cetta_fail(call, "hypot wants two numbers");
  return cetta_answer(call, R(hypot(a, b)));
}

cetta_def(m, (cetta_op){ .name = "hypot", .arity = 2,
                         .effect = CETTA_PURE, .fn = op_hypot });
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
cetta_atom *handle = cetta_object(&account, "account", NULL);
```

and a C function can be a value rather than a name, applied wherever it lands:

```c
cetta_atom *f = cetta_function(fn_triple, NULL, NULL);   /* ($f 5) is 15 */
```

## Bounding and measuring

An embedded engine that cannot be stopped is a hazard, so bounds are part of
the surface:

```c
cetta_limit(m, &(cetta_limits){ .seconds = 2.0, .inferences = 1000000 });
if ( !cetta_run(m, "!(from 0)") && cetta_error() == CETTA_LIMIT )
    fprintf(stderr, "%s\n", cetta_errmsg());   /* you stopped it */
```

`CETTA_LIMIT` is its own status precisely because a bound is not a fault. On a
lazy cursor the inference bound is a cumulative budget for the whole cursor,
metered step by step, so a big budget really does buy more steps than a small
one. The wall bound applies per step, so time the host spends between steps
does not count against it. A bound stops work MID-WAY and writes already made
stand, which is the honest semantics of every timeout.

Measuring uses the engine's own counters, and inferences are deterministic
where wall clock is not:

```c
cetta_stats before = cetta_stats_now(m);
/* ... work ... */
cetta_stats spent = cetta_stats_since(before, cetta_stats_now(m));
printf("%llu inferences\n", (unsigned long long)spent.inferences);
```

Two samples and a subtraction, because C has no `with` block and this is the
shape `getrusage()` already gave it.

## Scope cleanup

Where GCC and Clang have it, `CETTA_AUTO` releases a variable however the block
is left, `return` and `goto` included. This is systemd's `_cleanup_` and the
kernel's `__free`:

```c
#ifdef CETTA_HAS_AUTO
  CETTA_AUTO cetta_atom *held = cetta_one(cetta_eval(m, E("+", 1, 1)));
  CETTA_AUTO_ASK cetta_answers *r = cetta_run(m, "!(superpose (1 2 3))");
#endif
```

`CETTA_TAKE(p)` hands a value out of such a variable without it being released.

## Threads

One runtime per process, because `PL_initialise()` sets up the process's single
Prolog heap. A second `cetta_open()` with a matching configuration hands back
the same runtime; one with a different path fails.

A thread other than the one that opened the runtime calls
`cetta_thread_attach()` before it touches the engine and
`cetta_thread_detach()` before it exits. Building and reading atoms needs
neither, and the error state is per-thread.

The operation table is not guarded: publish every operation before the threads
that evaluate start, the same restriction `sqlite3_create_function()` carries.

## Layout

| file | what it is |
|---|---|
| `cetta.h` | the public API, and the only file a consumer includes |
| `cetta.c` | the C half: boot, term conversion, cursors, ops |
| `bridge.pl` | the Prolog half, calling published engine surface only |
| `extension.pl` | the seat declaration the engine reads at boot |
| `examples/` | `hello`, `ops`, `stream` |
| `tests/` | the C suite, run by `sh test.sh` and by the gate |
| `kit/` | the corpus and driver the cross-seat parity test uses |
| `benchmarks/` | what a C host pays, pinned to `baseline.json` |

The Python seat's `test_c_binding.py` runs both this seat and the Python host
over `kit/corpus.json` and requires the same answers.

Constraints and issues found while building this are recorded in
`ai-cetta-c-constraints.md` at the repository root.
