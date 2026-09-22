# CMeTTa language surface and verification

CMeTTa now exposes the shared engine's closed scopes, queries, type metadata,
algebra selection, providers and committed observations with C ownership.
Full Python equivalence still requires extracting world, saga, maintained-answer
and tagged-proof orchestration into the engine. No C implementation of those
language mechanisms was added.

## Implemented surface

Every result uses the existing `mt_keep`/`mt_drop`, cursor or list ownership
contract. Inputs marked as transferred are consumed even on failure; callback
arguments and accessor results are borrowed. `cmetta.h` states the exceptions
for borrowed-storage constructors, which acquire their owner only on success.

| Capability | C notation and engine owner | Executed witness |
|---|---|---|
| Transactions and speculation | `mt_transaction` / `mt_speculate` call the engine coordinator. Retained C registrations follow its outcome; nested scopes are savepoints. | `test_transactions`, `test_engine_scopes_cannot_abandon_c_registrations` |
| Prepared queries, joins and guards | Reuse an immutable pattern with `mt_keep`. `mt_query(s,p,g)` is `(match s p (if g p Empty))`; conjunction and shared variables remain engine terms. | `test_prepared_queries_join_and_guard_current_facts` |
| Temporary facts | Add inside `mt_speculate`, collect through the engine's held-cursor service, and return owned atoms after rollback. | `test_temporary_facts_leave_owned_answers_after_speculation` |
| Mutable cells | `new-state`, `get-state`, `change-state!` use ordinary term construction. Current engine transactions also roll cell writes back. | `test_mutable_cells_follow_engine_transactions` |
| Typed atoms and shapes | Type declarations relate shape variables. A native object's exact type name is published through `host_object/1` and `grounded_class_type/2`; the engine infers and dispatches. | `test_typed_atoms_relate_array_shapes`, `test_native_object_types_reach_engine_dispatch` |
| Algebras | `mt_eval_under` delegates scoped selection, identity and coefficients to `metta_with_under` and `metta_annotation`, returning owned `(value coefficient)` terms. | `test_algebras_are_scoped_engine_data` |
| Foreign spaces and live C data | Typed `mt_provider` callbacks produce candidate row cursors. The engine unifies candidates, preserves multiplicity and captures transaction participants. | `test_providers` |
| Composed spaces | A `match` over an engine `superpose` reads current facts from each source, preserving multiplicity. | `test_composed_spaces_read_live_sources` |
| Events and standing patterns | `mt_subscribe` publishes an engine subscription. Add/remove callbacks run after commit; rollback produces no notification. Self-cancellation retains the active callback's data. | `test_subscriptions` |
| Materialized snapshots | `mt_all` consumes a cursor and owns its collected atoms; a late error discards the entire prefix. | `test_native_iterators_close_on_every_exit`, `test_engine_iterators_keep_arguments_until_close` |
| Native producers | `mt_iterator` is a state/next/close cursor. Named functions stream answers; callable values return a stream consumed by the engine's `c-iter` adapter. | `test_native_iterators_close_on_every_exit`, `test_engine_iterators_keep_arguments_until_close` |
| Source and space lifetime | Run/load/do accept a runtime or space. `mt_forms` parses without executing. `mt_space_drop` retires engine state; `mt_space_close` releases the C handle. | `test_transactions`, `test_native_atoms_match_engine_terms` |
| Extension lifetime | Shared-object initialization is a closed transaction. Failed initialization restores registrations; loaded code remains mapped until process exit for escaped callbacks. | `test_extensions` |

Names retain their exact spelling. `word_count` and `word-count` are different
symbols; there is no automatic renaming. ABI 1 also adds allocator provenance,
borrowed text and child spans, exact unsigned values, UTF-8 validation, terminal
cursor statuses and allocation-free destruction. [MIGRATION.md](MIGRATION.md)
states the consumer changes and rollback procedure.

The callback, iterator, provider and allocator foundation reuses local C
implementation commit `551a2c998a1aa26b51b99f46fd771e955ea97961`. Its parent had
the same C/Prolog/header state as this checkout's baseline. The current change
preserves the newer README, removes the old name conversion, connects native
types, adds guarded queries and algebra selection, and verifies the whole
surface against the current engine. Earlier verification was not reused as a
substitute for running the gates here.

## Engine services still needed

| Language capability | Existing implementation and exact boundary |
|---|---|
| Reified worlds | [Python world orchestration](../python/metta/_history/world.py) and [world bridge](../python/metta/_binding/worlds.pl) own images, effect admission, evaluation, rebasing, diff and commit. The engine publishes effect and transaction primitives, but no complete shared world lifecycle. |
| Compensation and saga recovery | [Python Saga](../python/metta/_history/saga.py) owns enlistment, receipts, compensation ordering and recovery. Engine compensation lookup and covered effects alone do not implement that protocol. |
| Automatically maintained answers | [Python Live](../python/metta/live.py) selects incremental maintenance or invalidation/reanswer and maintains the result bag. C exposes engine events and owned snapshots; copying Live's maintenance algorithm would create another language implementation. |
| Tagged proof interpretation | [Python query bridge](../python/metta/_binding/query.pl) owns `metta_py_tagged_prove`, counting and tagged sources. C scopes the engine's existing annotation service, but does not claim Python's proof traversal, reinterpretation or aggregate counting result. |

These mechanisms must become published engine services before C can expose
their full semantics. Calling private Python predicates or rebuilding their
algorithms in C would violate the single-engine boundary. No engine files were
changed, and no success-shaped stubs were added. Existing engine-native opaque
`MT_HANDLE` results also remain display-only: resubmission is refused until the
engine supplies a portable retention/identity contract for them.

## Host ecosystem choices

Pandas, SQLAlchemy, Python array protocols, an asyncio scheduler, HTTP servers,
GraphQL schemas and remote authorization policy were not copied into the core.
C consumers receive typed row cursors, opaque owned values, function pointers,
thread attachment and loadable extensions. A database adapter can implement
`mt_provider`; a native array adapter supplies dimensions, strides and
operations explicitly. The core names no such third-party library. Scheduling,
networking and authorization remain the embedding application's responsibilities.
Term composition provides reads across spaces; an overlay's write policy is
an explicit provider policy, not a second union implementation in CMeTTa.

## Verification on 2026-09-22

Builds and runtime tests used `ai-battery-1`, a detached worktree inside this
repository. An owned engine/library copy under its `ai-tmp/engine` prevented
QLF and test-fixture writes from touching the concurrent superproject work.
`METTA_PATH` and `ENGINE_PATH` selected that copy. The explicitly requested
superproject evidence lane was run from its root.

| Check | Before | After |
|---|---:|---:|
| `make all test` counted C checks | 486 passed, 0 failed | 543 passed, 0 failed |
| Additional behavior suites | Existing suites passed | All existing suites plus 7 new suites passed |
| Built and executed examples | 4 | 5, including `lower` and `language` |
| Cursor lifecycle plunit cases | 4 passed | 4 passed |
| Closed-cursor records/bytes at 2,000 / 10,000 / 20,000 closes | 0 / 0 at each size | 0 / 0 at each size |
| Public header definitions | Existing surface passed | 153 declarations, all defined |

`make all test install-check` passed after the last semantic change. The
installed consumer linked `libcmetta.so.1` using pkg-config and booted with
`METTA_PATH` unset. Documentation names, version consistency, hardening,
source/QLF boot parity, allocation failures, runtime restart, hash and thread
tests all passed. The new suites sweep 242 allocation-refusal positions,
100,000-level destruction, 4,096 DAG cases and 64 native/engine unification
pairs. Each case has a `test_` function, `CASE(...)` prose, an executing `main`
and a path from `make test`.

The existing Python oracle compared all **39 corpus programs** with a live C
driver and live Python runtime: zero differences in answer groups,
multiplicity, normalized text or metatype. Its runtime and Python source copy
were isolated under the same battery. The oracle itself was read from
`extensions/python/tests/ch21_another_language_at_the_seam/test_c_binding.py`;
no Python source was edited.

The C citations name executed cases after correcting compact runner braces
and a JSON citation. The final root evidence lane still exits 1 with **51 C
provenance findings** and 15 findings for the concurrent Node component:

```text
commit=1a60e2a3cce69d5d6bda67100186939d707f4397 does not resolve to a commit
GATE FAILED: evidence
```

The commit exists in CMeTTa's repository. The root checker's
`commit_problems` runs every `git cat-file` query in the superproject instead
of the claim's owning repository. That resolver needs an orchestrator fix
outside C's write scope; it was requested explicitly. The required zero-finding
evidence result is therefore still blocked. The tags are pinned to the actual
implementation rather than left as `WORKTREE` to hide the problem.

The C implementation is commit `1a60e2a3cce69d5d6bda67100186939d707f4397`;
commit `80ead0af42ad724328e7bd74ec1ba2ada78ea398` pins 54 evidence tags to it
and changes no executable code.

## Memory results and failures

Valgrind 3.26.0 ran with full leak checking, no added suppressions, and
`--errors-for-leak-kinds=definite,indirect --error-exitcode=99`.

| Execution | Result |
|---|---|
| Native ownership suite | Exit 0; 237,105 allocations and frees; 0 bytes remaining; 0 errors |
| Native iterator suite (`--native`) | Exit 0; 19 allocations and frees; 0 bytes remaining; 0 errors |
| Engine-backed iterator/provider/subscription/extension suites and language example | Exit 99; each reports 41,128 definite and 480 indirect bytes retained by the runtime |
| Transactions with two engine lifetimes | Exit 99; 82,256 definite and 960 indirect bytes |
| Native/engine parity suite | Exit 99; 41,288 definite and 480 indirect bytes |
| Main C suite | Exit 99; 42,752 definite and 480 indirect bytes |

All new suites using the C allocation tracker reach zero owned blocks and
bytes at shutdown. `--track-origins=yes` attributes the engine-backed
uninitialized-value reports to stack and heap allocations inside
`libswipl.so.10.1.14`. These results do not make whole-process Memcheck green.

The separately compiled [SWI-only probe](tests/swi_memory_probe.c) loads neither
CMeTTa nor PeTTa. `make runtime-memory` produced:

```text
SWI memory baseline: exit 99
SWI memory int64: exit 99
SWI memory unicode: exit 99
make: *** [Makefile:269: runtime-memory] Error 99
```

The baseline loses 32,872 definite plus 72 indirect bytes. Integer and Unicode
probes each lose 32,920 definite plus 72 indirect bytes. The strict memory gate
therefore remains blocked by the installed SWI runtime; no suppression or C
cleanup workaround hides it.

The first ownership run under Memcheck aborted with
`Assertion '!a && mt_error() == MT_UNSUPPORTED' failed`. Valgrind models x87
arithmetic at 64-bit precision, so the test's long double arrived as exactly
1.0. The corrected test measures the delivered value, checks its exact result
and releases it. Native execution still tests refusal of the wider value;
Memcheck does not verify 80-bit arithmetic. This limitation is documented in
the [Valgrind manual](https://valgrind.org/docs/manual/manual-core.html#manual-core.limits).

Other repaired regression failures were an old underscore-to-hyphen expectation,
stale thread-local error state in a fixture, a cell rollback expectation based
on an outdated engine comment, Bool's identity being `1` rather than `True`,
and the two native object type lookups before their ownership hook was added.
A transaction witness originally expected a callback in an unused `let`
binding; it did not execute. Its exact failure was
`transaction line 109: raw_scope(m, NULL) >= MT_ERROR; no error`. The corrected
witness asserts invocation before checking refusal, including a raw nested
scope inside a C transaction.

An attempted transaction-depth probe was rejected after nested
`current_transaction/1` enumeration repeated indefinitely. The implemented
guard inspects only the innermost live goal and compares its compound identity.
This follows SWI's [transaction implementation](https://raw.githubusercontent.com/SWI-Prolog/swipl-devel/V10.1.14/src/pl-transaction.c)
and [foreign-interface identity operation](https://raw.githubusercontent.com/SWI-Prolog/swipl-devel/V10.1.14/src/pl-fli.c).

The clone scan included the full 6,000-line implementation rather than the
tool's default 1,000-line file limit. It reported one existing error-setter
clone, 0.1%; extracting those different varargs return paths would not remove
another implementation of language behavior.
