# engine/fabric

The Fabric: the canonical project model of master plan Volume 04 (SL-ARCH-001 data side, SL-ARCH-002 transactions,
undo and recovery), released by WO-063 and hardened by WO-064, both documents at InReview 0.3.1. Target `sl_fabric` (alias `sl::fabric`),
namespace `sl::fabric`. Every fallible call returns `sl::result<T>` with a code of the fabric error range 5000-5999
(ADR-0009, ADR-0010); persisted-path code has no `throw`, `try`, `.at()`, `std::sto*` or unordered container
(FAB-INV-CF-04).

Layout:

| Path | Contents |
| :---- | :---- |
| `include/sl/fabric/` | the 38 public headers |
| `include/sl/fabric/internal/` | headers shared inside the library, not API (the Runtime and Render code must not include them: R-BND-INCLUDE) |
| `src/` | sources, globbed into `sl_fabric` |
| `gen/` | schema-generated code (`gen/include`, `gen/src`), produced by `tools/fabric/slgen.py` and checked by `just fabric-gen-check` |
| `testing/` | `sl_fabric_testing`: model stores, faulty vfs, crash harness, fixtures and the generic conformance suite |
| `tools/` | developer executables: `sl_fabric_cli` (the demo), `sl_fabric_corpus`, `sl_fabric_dump_contract`, `sl_fabric_dump_domains`, `sl_fabric_fsck`, `sl_fabric_mkstore` |

What the headers hold (SL-ARCH-001 "API surface" and SL-ARCH-002 list every type):

| Area | Headers | What |
| :---- | :---- | :---- |
| Identity | `id`, `identity`, `digest`, `bytes`, `clock` | record ids and classes, mint schemes, lifetime and tombstones, SHA-256 object ids, strict LEB128, injected clock |
| Types and schemas | `type`, `value`, `schema`, `migration` | the type system, immutable values, schema registry and lock, compatibility and migrations |
| Encoding | `codec` | SLB/1 binary and text forms, limits, corpus loaders |
| Project data | `manifest`, `ownership`, `authority`, `provenance`, `events` | manifest, F2 domain registry, authority, epistemic classes, event envelopes and bus |
| Store | `store`, `memory_backend`, `log_backend`, `vfs`, `journal`, `checkpoint` | object store seam, the in-memory and the log-structured backends, durability, journal frames |
| History | `tree`, `snapshot`, `revision`, `patch`, `validate`, `transaction`, `undo`, `f5` | the revision trie, patches, validators, the transaction manager (the only mutation path), undo, the F5 ledger |
| Graph | `deps`, `impact` | dependency graph, closure, change impact |
| Boundaries | `boundary`, `resolver` | read-only document projection and proposals from the runtime, the reference resolver |
| Recovery | `recovery`, `corruption`, `fsck`, `failpoints`, `workspace` | crash recovery, corruption classes C1-C12, fsck, fault injection, the workspace that opens a project |

Gates: `just fabric-gate` is the Volume 04 release gate. It runs the strict contract check, the contract lock, the
nothrow lint, the golden corpus and hash vectors, determinism across clang and gcc, the schema lock, generation, F2
ownership, the F6 event schema, the F5 surface lint, the crash sweeps, fsck, the conformance suite, the scorecard and
the demo, then every `neg-fabric-*` test. `just fast` carries the seven Python-only checks, `just check` runs
`fabric-gate` after `test`, and `just acceptance-061-080` writes `build/acceptance/10-*.log` to `12-*.log`. One
area's tests: `just fabric-test <area>` (CTest label `fabric_<area>`); `just fabric-demo` runs the end-to-end
scenario.

Limits (recorded in `docs/episodes/061-080.md`): durability is simulated by a faulty vfs and never power-loss tested
(seam S15); Windows is unverified and `vfs_win32` returns unsupported (ADR-0006, seam S14); big-endian hosts are
untested; wall time is unmeasured (Volume 16). WO-064 fixed most code changes the reconciliation found; the rest (undo intents,
O(delta) reopen, delete-check scans, schema objects per revision) is listed in the open issues of both ARCH documents.

Build: Debug and sanitizer builds define `SL_FABRIC_F5_INSTRUMENT=1` publicly (ADR-0016). Tests live in
`tests/unit/fabric` and `tests/integration/fabric`; fixtures in `tests/fixtures/fabric`.
