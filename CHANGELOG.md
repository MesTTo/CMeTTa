# Changelog

## Unreleased

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
- Change the installed ABI to 1; see [MIGRATION.md](MIGRATION.md) before rebuilding consumers.
