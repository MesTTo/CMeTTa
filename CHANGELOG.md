<!-- Purpose: record changes to the C surface and their regression witnesses.
Open Obligations: None. -->

# Changelog

## Unreleased

- Document a known issue in the handle contract. A handle holding a compound
  other than a partial application, such as a caught refusal's payload,
  answers nothing and raises nothing where the engine evaluates it, because
  the engine's translator reads only blobs and partial applications as
  values there; it goes back whole wherever the engine reads data. The
  engine fix is outside this library.

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
  and a handle's key walk keeps two per level, a list's tail
  taking its cell's level, so a handle over a 400,000-element list decodes
  under a 16 MB limit where it ran out of memory up to 32 MB. A compound
  handle's text is again the engine's written form, so mt_name() answers
  partial(+,[1]) rather than the key it is compared by; the key now sits
  beside the record and identifies blobs within their runtime too.

- Make a compound handle's identity injective and a handle's release safe
  against a concurrent close. A compound was identified by its quoted text,
  which prints two blobs, or two variables, alike; it now carries a key that
  length-prefixes every name, spells floats exactly, names a blob by its
  atom and a variable by its first occurrence, so two handles are equal
  exactly when they hold variants. Releasing a handle erases its engine
  record, and mt_close() now announces itself and waits for any erase in
  flight while every release checks for the announcement first, so no record
  is erased into the heap PL_cleanup() frees. A compiler without C11 atomics
  is refused at build time instead of producing reference counts that race.

- Identify a native-blob handle by its blob atom and a compound handle by its
  quoted text. Handles compared by their printed text, so two different
  blobs whose writer prints the same text were `mt_eq`, hashed alike and
  sorted as equal; the blob atom each handle now holds settles which value it
  is.

- Hold every engine value with no MeTTa structure as an `MT_HANDLE` that
  goes back whole. A partial application such as `partial(+,[1])` used to be
  refused, and the refusal failed every answer of the cursor or run holding
  it, while the engine prints it as `(partial + (1))` and the Python seat
  reads it. The handle records the engine term, so `mt_show` prints what the
  engine prints and passing the handle back puts the identical value back:
  the partial still applies, `(<handle> 2)` answering 3. Native blobs gain
  the same round trip, where they used to be refused on resubmission. A
  handle does not outlive its runtime, and passing one back after
  `mt_close` is refused by name.

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
