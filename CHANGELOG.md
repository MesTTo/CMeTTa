<!-- Purpose: record changes to the C surface and their regression witnesses.
Open Obligations: None. -->

# Changelog

## Unreleased

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
