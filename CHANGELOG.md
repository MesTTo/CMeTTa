# Changelog

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
