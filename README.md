# Sliced Labs

Unified engine and production platform: a C++23/Vulkan 2D/3D engine and editor, built in the order of the master plan (`docs/plan/SL-PLAN-001-master-plan.md`, 840 episodes in 42 volumes).

## What exists now

The Foundation slice (episodes 041-048, WO-013 to WO-021), the infrastructure slice (episodes 049-060, WO-020 and WO-024 to WO-034) the Fabric (Volume 04, episodes 061-080, WO-035 to WO-063) and the runtime foundation (Volume 05, episodes 081-100, WO-075 to WO-101) are built and gated on this machine; the engine is `sl_core` (logging, errors, assertions), `sl_fabric` (the canonical project model, below), `sl_runtime` (the native runtime, below) and a `sl_sandbox` executable. Every episode is still `Planned` in the registry: the founder records evidence and advances status after the merge.

- Governance: 840-episode registry, controlled documents, ADR-0001 to ADR-0009 (all Proposed), work-order checks, the factory contract (`FACTORY.md`).
- Build: CMake 3.28+ and Ninja with presets (schema 8) for clang and gcc, sanitizers (asan, ubsan, tsan), C++23 with a feature battery and C++26 probes (`cmake/`, `docs/build/README.md`).
- Vulkan: header, loader and validation-layer detection, slangc and spirv-val (no device is opened).
- Dependencies: one policy and one lock (`deps/`, `tools/deps/sldeps`), providers vendored, system, cpm and vcpkg through `sl_require()`; doctest and cpm are vendored under `third_party/`.
- Local CI: `scripts/ci-local.sh` (quick and `--full`), `fresh-clone-check`, and the acceptance runs for episodes 041-048 and 049-060.
- Quality gates: format for six source kinds (`fmt-check-all`), clang-tidy, static analysis, sanitizer proofs and hardened-binary checks (`check-full`), test tiers with a JUnit report, fixtures and golden data.
- Traceability: artifact provenance records (`build/<preset>/provenance/`), structured logs with a schema check, generated docs, license and attestation checks for dependencies, a toolchain lock and a strict doctor.
- Hosted CI: `.github/workflows/ci.yml` and `windows.yml`, validated offline by `just ci-yaml-check`; they have not run on GitHub.
- The Fabric (`engine/fabric`, namespace `sl::fabric`): identity algebra, the type system, SLB/1 canonical serialization, schema versioning and migration, the project manifest, ownership (F2) and authority, provenance, event envelopes, a content-addressed revision store with a log-structured backend, transactions (F5), undo, a dependency graph with change impact, the runtime/document boundary, the reference resolver, crash recovery, corruption policies and fsck, schema-generated tooling, and a conformance suite. SL-ARCH-001 and SL-ARCH-002 are InReview 0.3.1; `schemas/fabric/schema.lock.json` pins the seven `fabric.*` modules at 1.0. `just fabric-gate` is its release gate and `just fabric-demo` shows the whole path (create, impact, undo, commit, reopen, crash, recover).
- The runtime (`engine/runtime`, namespace `sl::runtime`; headless, no window or input device): lifecycle and recovery, the platform boundary, tracked memory with allocators and diagnostics, synchronization, a job system with work stealing, clocks and the determinism kit, the frame scheduler, an archetype ECS (entities, storage, queries, command buffers, system scheduling, change tracking), reflection, typed events, snapshots, the Fabric link and a stress suite. SL-ARCH-003 is InReview 0.3.0. `just runtime-gate` is its release gate (it runs `runtime-demo`, the 50000-lantern scene, about 500 s in Debug) and `just runtime-determinism` (about 820 s) proves determinism class D1 across 1 to 16 workers; both are measured in `docs/episodes/081-100.md`.
- Records: `docs/episodes/041-048.md`, `docs/episodes/049-060.md`, `docs/episodes/061-080.md` and `docs/episodes/081-100.md` (done, decisions, acceptance, evidence, open issues per episode).

Unverified (details in `docs/episodes/041-048.md`, `docs/episodes/049-060.md`, `docs/episodes/061-080.md` and `docs/episodes/081-100.md`; for the runtime: Windows (ADR-0006), windowing and input devices (the runtime is headless), cross-compiler and cross-platform determinism (D1 is proven for one binary; clang versus gcc is recorded, not gated), wall-time performance (recorded, not gated); for the Fabric: Windows (ADR-0006), big-endian hosts, real power loss (durability is simulated), wall-time performance, and the open issues the ARCH documents list for a later work order: undo intents, O(delta) reopen, delete-check scans): the GitHub workflows (never executed), Doxygen output (not installed here, the API page is a stub), the development image (`Containerfile` never built), 37 allow-listed long Python lines and 8 allow-listed tidy files, the empty and manual vulnerability register, the ADR acceptances,  Windows MSVC and clang-cl configure (ADR-0006), the CMake 3.28 floor (only 4.4.4 is tested, ADR-0005), `import std` beyond this machine, doctest upstream attestation, the cpm `url_hash` and the vcpkg baseline (ADR-0002).

## Gate commands

| Command | What it does |
|---|---|
| `just fast` | Stop-gate, under 60 s: layout, gates, tool tests, format, ADRs, work orders, the seven Python-only Fabric checks, `runtime-lint` and `runtime-contracts-strict` |
| `just check` | Full gate (`gate.full`): `fast` plus C++ build gates, dependency, feature and Vulkan checks, tests, `fabric-gate`, `runtime-gate`, every negative test and `ci-local` |
| `just check-full` | Release gate: `check` plus the ubsan, tsan, release and gcc workflows, then `tidy`, `analyze`, `sanitize-proof`, `hardening` and `neg-tidy` (WO-025), then the runtime sanitizer runs (`runtime-san` asan, ubsan, tsan) and `runtime-determinism` (WO-101) |
| `just ci-local` | The whole local CI from the repository root (`scripts/ci-local.sh`: doctor, format, governance checks, clang, gcc and asan workflows); no network |
| `just ci-local-full` | `ci-local --full`: adds the ubsan, tsan, release and gcc-release workflows (about 93 s) |
| `just acceptance-041-048` | The four starter-prompt checks of episodes 041-048, logs in `build/acceptance/` |
| `just acceptance-049-060` | Fresh clone, provenance, test report, ci yaml and episodes doc checks of episodes 049-060, logs 5-9 in `build/acceptance/` |
| `just fabric-gate` | The Volume 04 release gate: every Fabric check and every `neg-fabric-*` test |
| `just fabric-demo` | The Fabric end to end: create, impact, undo, commit, reopen, crash at FP4, recover |
| `just acceptance-061-080` | `fabric-gate`, `fabric-demo` and `check-full` for episodes 061-080, logs 10-12 in `build/acceptance/` |
| `just runtime-gate` | The Volume 05 release gate: every runtime check (lint, strict contracts, contract lock, tests per area, stress, `runtime-demo`, memory report, scorecard) and every `neg-runtime-*` test |
| `just runtime-demo` | The lantern-field scene headless (50000 entities, 600 ticks, a scripted fault and recovery), evidence in `build/runtime/gate/` |
| `just acceptance-081-100` | `runtime-gate`, the three sanitizer runs, `runtime-determinism` and `check-full` for episodes 081-100, logs 13-16 in `build/acceptance/`, evidence copied to `build/acceptance/runtime/` |
| `just episodes-doc-check 041-048` | The episode record has a section per episode with its five sub-headings, a `just` recipe and an ADR or "none" (also `049-060`, `061-080` and `081-100`) |
| `just doctor` | Installed tools against `toolchain/environments.json` |

`just --list` shows every recipe. A recipe not implemented yet is a stub: `just stub <name>` prints `stub: owner WO-0xx` and exits 3; owners and status are in `tools/governance/recipes.json`.

## Layout (ADR-0001)

```
engine/{core,runtime,render,fabric}   engine libraries (core, fabric and runtime have code)
editor/  sdk/  platform/               reserved, README only
examples/sandbox/                     first executable (sl_sandbox)
cmake/  CMakeLists.txt  CMakePresets.json   build policy, features, Vulkan, dependency resolver
deps/  third_party/                   manifest, lock, vcpkg files; vendored packages with hashes
toolchain/                            supported environments as data
tests/{unit,gates,governance}         doctest tests, negative gate tests, governance fixtures
tools/{governance,deps,presets}       Python 3 stdlib tooling (python3 -I, no network)
scripts/                              doctor, ci-local, acceptance, evidence helpers
docs/{plan,gov,vol,adr,build,episodes,templates}
registry/  schemas/                   registries, generated views, JSON schemas
work-orders/  evidence/               WO-NNN.md; evidence/WO-NNN.json written by the founder
```

## Where things live

- Work orders: `work-orders/WO-NNN.md` (frozen after landing); evidence: `evidence/WO-NNN.json`, written by the founder with `just evidence-record` after the merge.
- Registries: `registry/` (SL-REG-002 episodes is authoritative); generated views: `registry/views/` (never hand-edited), including `SL-REG-005-dependencies.md` from `just deps-register`.
- Decisions: `docs/adr/`. Factory contract: `FACTORY.md`. Recipe manifest: `tools/governance/recipes.json`.

## Founder close-out

After the merge the founder records evidence and advances the episodes; the builder never does. The steps are in SL-PLAN-003 (`docs/plan/SL-PLAN-003-work-order-protocol.md`, manual mode, ADR-0007): `just evidence-record WO-NNN`, then `just ep-advance SL-EP-04n Demonstrated WO-NNN`. Episodes 041-048 may reach Demonstrated but not Accepted before episode 020 is Accepted (ADR-0003). Push only after the gates pass; the first push enables the windows-latest configure job (ADR-0006).
