# The C binding

MeTTa from C. A C program boots the PeTTa engine in its own process, builds
and reads terms as C values, runs programs, pulls answers one at a time, and
publishes C functions the language can call.

```c
#include <cetta.h>
#include <stdio.h>

int main(void)
{ cetta_t *m;
  cetta_answers_t *answers;

  cetta_open(NULL, &m);
  cetta_run(m, "(= (double $x) (* 2 $x))\n!(double 21)\n", &answers);
  while ( cetta_answers_step(answers) == CETTA_ROW )
    printf("%s\n", cetta_answers_text(answers));       /* 42 */
  cetta_answers_free(answers);
  cetta_close(m);
}
```

Build with `make`, then `make test`. It needs a C compiler and SWI-Prolog's
development headers; `swipl --dump-runtime-variables` is how the Makefile finds
them.

## Where this seat sits

`bindings/` holds one folder per driver of the engine, and this is the C one,
beside `python` and `node`. It is not the vendored CeTTa C substrate, which is
a different track: `bindings/` is who DRIVES the engine, `backends/` is what
the engine CONSULTS.

What makes this seat different from the other two is that it is IN the engine's
process. Python reaches the engine through janus and Node through a WebAssembly
build, so both have a language boundary to cross and both encode every term
into the tagged arrays `CODEC.md` describes. C has no boundary: it reads
`term_t` directly with `PL_get_*`. There is no wire codec here, and that is the
reason the seat exists.

## The ownership law

C carries it in the type system, and there is nothing else to remember:

- a function taking `const cetta_atom_t *` **borrows**; you still own it
- a function taking `cetta_atom_t *` **steals**; do not release it afterwards
- every constructor returns a reference you own
- every accessor returns a borrowed pointer, valid while its parent lives

So this leaks nothing, because `cetta_expr` steals its children:

```c
cetta_atom_t *goal = cetta_expr(3, cetta_sym("+"), cetta_int(1), cetta_int(2));
```

and this leaks, because `cetta_add` borrows:

```c
cetta_add(space, cetta_expr(2, cetta_sym("f"), cetta_int(1)));   /* wrong */
```

If an inner constructor fails, the outer one releases the siblings that
succeeded and returns `NULL`, so a failure part-way through a nested build
cannot leave you holding half a term.

## Atoms

Nine wire tags become twelve C kinds, because C has types where the codec has
one tag and refuses to round what does not fit:

| kind | what it is |
|---|---|
| `CETTA_SYMBOL` | a name that denotes itself |
| `CETTA_STRING` | grounded text |
| `CETTA_INT` | an exact integer that fits `int64_t` |
| `CETTA_FLOAT` | a float; `2` and `2.0` are different atoms |
| `CETTA_BIGINT` | an exact integer too wide for `int64_t`, read as digits |
| `CETTA_RATIONAL` | an exact ratio, read as numerator and denominator |
| `CETTA_BOOL` | `True` or `False`, which are not symbols |
| `CETTA_VARIABLE` | a variable; the name is an identity within its term |
| `CETTA_EXPR` | an expression; the empty one is unit |
| `CETTA_SPACE` | an executable space reference |
| `CETTA_OBJECT` | a live C value crossing by reference |
| `CETTA_HANDLE` | a native engine value held by reference |

Building and reading them starts no engine. `cetta_parse()` and `cetta_show()`
do, because text goes through the engine's own reader and writer rather than a
second one grown here.

## Answers are stepped, not drained

`cetta_eval()` computes one answer per `cetta_answers_step()`, so an endless
generator is ordinary:

```c
cetta_run(m, "(= (from $n) (superpose ($n (from (+ $n 1)))))\n", &defined);
cetta_answers_free(defined);

cetta_atom_t *goal = cetta_expr(2, cetta_sym("from"), cetta_int(0));
cetta_eval(cetta_self(m), goal, &answers);
for (int i = 0; i < 5 && cetta_answers_step(answers) == CETTA_ROW; i++)
  puts(cetta_answers_text(answers));
cetta_answers_free(answers);      /* the sixth answer is never computed */
```

The current answer belongs to the cursor and dies on the next step, which is
the contract `sqlite3_column_*` already gave C. `cetta_retain()` it to keep it.

`cetta_run()` is the eager door, because running a program means running it;
its answers carry `cetta_answers_group()` saying which `!` form produced each.

## Publishing C functions

```c
static cetta_status_t op_hypot(cetta_call_t *call, void *user)
{ double a, b;
  if ( cetta_float_value(cetta_call_arg(call, 0), &a) != CETTA_OK ||
       cetta_float_value(cetta_call_arg(call, 1), &b) != CETTA_OK )
  { cetta_call_error(call, "hypot wants two floats");
    return CETTA_ERROR;
  }
  return cetta_call_return(call, cetta_float(hypot(a, b)));
}

cetta_op(m, "hypot", 2, CETTA_PURE_STRUCTURAL, op_hypot, NULL);
```

`(hypot 3.0 4.0)` now answers `5.0`. Naming the effect class is required, not
advisory: the engine reasons about caching, reordering and transactions from
it.

The name reaches MeTTa through C's own casing convention, so `word_count`
publishes `word-count`, exactly as Python's `car_atom` reaches `car-atom`. A
name outside C's identifier grammar crosses untouched, which is the escape for
`prime?` and `%Undefined%`.

A C value can also cross MeTTa untouched and come back the same object:

```c
cetta_atom_t *handle = cetta_object(&account, "account", NULL);
```

and a C function can be a value rather than a name, applied wherever it lands:

```c
cetta_atom_t *f = cetta_function(fn_triple, NULL, NULL);   /* ($f 5) is 15 */
```

## Bounding and measuring

An embedded engine that cannot be stopped is a hazard, so bounds are part of
the surface:

```c
cetta_limits_t limits = { .seconds = 2.0, .inferences = 1000000 };
cetta_set_limits(m, &limits);

if ( cetta_run(m, "!(from 0)\n", &answers) == CETTA_LIMIT )
  fprintf(stderr, "%s\n", cetta_errmsg());   /* you stopped it; it did not break */
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
cetta_stats_t before, after, spent;
cetta_stats(m, &before);
/* ... work ... */
cetta_stats(m, &after);
cetta_stats_delta(&before, &after, &spent);
printf("%llu inferences\n", (unsigned long long)spent.inferences);
```

Two samples and a subtraction, because C has no `with` block and this is the
shape `getrusage()` already gave it.

## Errors

Nothing prints, exits or longjmps. A fallible call answers a
`cetta_status_t`, and `cetta_errmsg()` has the words on the calling thread,
which is the shape `dlerror()` and `strerror()` already established.

```c
if ( cetta_run(m, "!(assertEqual 1 2)\n", &answers) == CETTA_ERROR )
  fprintf(stderr, "%s\n", cetta_errmsg());
```

MeTTa keeps most failures as VALUES rather than raising: `(car-atom 5)` answers
unit and `(+ 1 foo)` answers itself unreduced. Those arrive as ordinary
answers, not errors, which is the language's design and not this binding's.

## Threads

One runtime per process, because `PL_initialise()` sets up the process's single
Prolog heap. A second `cetta_open()` with a matching configuration hands back
the same runtime; one with a different path answers `CETTA_MISUSE`.

A thread other than the one that opened the runtime calls
`cetta_thread_attach()` before it touches the engine and
`cetta_thread_detach()` before it exits. Building and reading atoms needs
neither.

## Layout

| file | what it is |
|---|---|
| `cetta.h` | the public API, and the only file a consumer includes |
| `cetta.c` | the C half: boot, term conversion, cursors, ops |
| `bridge.pl` | the Prolog half, calling published engine surface only |
| `decider.pl` | the seat declaration the engine globs at boot |
| `examples/` | `hello`, `ops`, `stream` |
| `tests/` | the C suite, run by `make test` and by `check.sh` |
| `kit/` | the corpus and driver the cross-seat parity test uses |

`bindings/python/tests/ch21_another_language_at_the_seam/test_c_binding.py`
runs both this seat and the Python host over `kit/corpus.json` and requires the
same answers.

Constraints and issues found while building this are recorded in
`ai-cetta-c-constraints.md` at the repository root.
