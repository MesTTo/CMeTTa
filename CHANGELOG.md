<!-- Purpose: record changes to the C surface and their regression witnesses.
Open Obligations: None. -->

# Changelog

## Unreleased

- Decode engine values in the wire grammar the Python and Node seats read,
  and hold only native blobs by reference. A partial application such as
  `partial(+,[1])` used to be refused, failing every answer of the cursor or
  run that held it; it now arrives as the expression `(partial + (1))`, equal
  to the one a C program builds and to the one the other seats answer, and as
  there it is data when passed back rather than a function that applies.
  Every other non-list compound the engine hands out, a refusal's payload
  among them, is `(F args...)` with its functor a symbol, a zero-arity one
  `(F)`; an improper list is `(cons Head Tail)` along its spine; variables
  keep their identity; and an answer that is a cyclic term is refused by name
  instead of walked forever, at the answer, group and C-argument sites where
  the Python seat refuses it. An `MT_HANDLE` is now only a native blob: it
  holds a record, prints as the engine prints it, goes back as the identical
  blob, and two handles are equal exactly when they hold one blob of one
  runtime. Releasing one erases its record under a handshake with `mt_close`,
  so no erase reaches the heap `PL_cleanup` frees, and a handle passed back
  after `mt_close` is refused by name. A compiler without C11 atomics is
  refused at build time, since `cmetta.h` promises atomic reference counts.
  Decoding now classifies each term once with `PL_term_type`, where it asked
  up to six questions, each a checked foreign call, so the term-out bench
  reads 8.3% fewer instructions and cursor-step 1.1% fewer, net of the
  acyclicity guard; both rows are re-pinned with their steps placed beside
  them.

- Add mt_solve, relational let answered as bindings, the C counterpart of the
  Python seat's solve(). mt_solve(target, pattern, subject) evaluates
  (let pattern subject template) with the template derived rather than
  written: the named variables of the pattern, then those the subject adds,
  each at its first occurrence, a lone one standing for itself. Every answer
  is an instance of the template, so the cursor keeps it and mt_bound() reads
  each variable by name, as it does for mt_match and mt_query. Solving 25 for
  (* $x $y) answers the six factor pairs. A solve naming no variable, `_`
  included, is refused with MT_MISUSE.

- Stop a decode from aborting the process on terms with many variables, and
  bound every term walk's SWI references by its depth. Naming a variable
  walked the engine's name list making two term references per pair and kept
  them, so a parse of 3,000 distinct variables under a 16 MB stack limit, or
  16,000 under the default, filled the stacks and passed the 0 that followed
  to PL_get_arg, which aborts. Those references now live in a frame per call,
  decode keeps one reference per depth rather than one per sub-expression,
  reading each element into the next depth's so descending copies nothing,
  and a compound over a 400,000-element list decodes under a 16 MB limit.

- Add `mt_compare`, the engine's standard order of terms, and `mt_order`, the
  same order for `qsort`. Python atoms sort in the engine's term order and C
  atoms could not be sorted at all, so a C program had to ask the engine to
  `msort` a list it already held. Numbers compare by exact value across
  integers, ratios, big integers and floats, through each float's integer
  ratio, because the engine's order is exact: `9007199254740995` sorts before
  `9007199254740996.0`, which comparing as floats would call a tie. 920
  adjacent pairs of random mixed atoms from the engine's `msort` agree.

- Add `mt_alpha_eq`, MeTTa's `=alpha` over C atoms: equality up to a
  one-to-one renaming of variables, with each `_` a variable of its own. The
  Python seat's atoms answer `alpha_eq` and C had only `mt_eq`, which compares
  variable names, so a program could not ask whether an answer carrying engine
  variable names was the atom it expected. It needs no engine, and 400
  generated pairs agree with the engine's `=alpha`.

- Keep variable identity in every answer. An engine variable with no source
  name decoded as SWI writes one, `_`, the anonymous name, so `mt_atoms`,
  `mt_match`, `mt_eval` and `mt_bound` answered `(fact $u $u $w)` as
  `(fact $_ $_ $_)` and a lowered equation read back as three unrelated
  variables; copying one into another space stored an equation whose body no
  longer mentioned its argument. Each answer now names its variables from a
  process-wide counter, one name per variable and never one name across two
  answers, stepping over source names, as the Python seat's wire encoder does.
  `test_an_answer_keeps_variable_identity` fails ten ways against the previous
  library.

- Decide every benchmark row by instructions:u and Cachegrind's estimated
  cycles; CPU time is recorded per operation as advice. task-clock could only
  decide below one runnable process per core, and the box the gate runs on is
  never quiet, so its comparisons were declined in nearly every run. The driver
  marks its window with Cachegrind's client requests beside perf's control
  descriptors, the case sizes drop tenfold so a simulated run takes seconds,
  and `bench.sh` now needs valgrind, whose version the counter stamp records.

- Relink the library and every program built on it when the compiler, its
  flags, the SWI host or the engine path change. A `.toolchain-stamp` holds the
  command line each output bakes in and is rewritten only when it differs, the
  way git's Makefile tracks its CFLAGS. A library linked against the stock SWI
  before the engine began refusing unpatched hosts had kept booting it, and
  every program linking it died in PL_initialise; a copied checkout also kept
  booting the original's engine.

- Expose `mt_effect_plan` through the shared source planner. The effect-rank
  twin found that general `explain` metadata did not describe C registrations;
  planning now returns the operation roster and joined class without execution.
  Publish C callback effects to the catalog and compose overloaded ranks;
  previously the planner conservatively classified even pure callbacks as I/O.

- Release captured provider ownership at transaction completion. The SQLite
  corpus exposed connections retained until blob collection after provider and
  cursor close; a regression now requires immediate release of the last owner.

- Add `mt_matcher` after the custom-matching corpus exposed a missing C door
  to the engine's grounded-value matching hooks. Candidate iterators retain
  callback data, propagate errors and close when abandoned.

- Let `build.sh` and `test.sh` run in an isolated component checkout. The
  enclosing gate supplies any deadline; these scripts no longer require a
  superproject-relative helper.

- Build and install `libcmetta.a`, and publish its SWI dependency through
  `pkg-config --static`. Exercise an archive consumer alongside the shared one.

- Fix the callbacks example calling `word-count` after publishing `word_count`.
  Check all callback results and the expected refusal so incorrect output fails
  the existing example gate.

## 1.0.0

- Add closed transactions and speculation, committed event subscriptions,
  native iterators, and typed foreign row providers with transaction callbacks.
- Add guarded joins and reusable query patterns, scoped algebra evaluation,
  space-specific source execution, engine space release, and native object
  type names through the engine's grounded type seam.
- Preserve exact published names, UTF-8, integer identity, callback lifetimes,
  and complete-collection errors. Add allocator provenance, borrowed storage,
  and allocation-free atom release to make ownership testable.
- Document temporary facts, cells, shape types, composed spaces, integration
  boundaries, and the engine services still needed for worlds and maintained views.
- Change the installed ABI to 1, so consumers and extensions rebuild against it.
