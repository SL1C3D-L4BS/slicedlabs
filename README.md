# Sliced Labs

Unified engine and production platform: a C++23/Vulkan 2D/3D engine and editor, built in the order of the master plan (`docs/plan/SL-PLAN-001-master-plan.md`, 840 episodes in 42 volumes).

## What exists now

The Foundation slice (episodes 041-048, work orders WO-013 to WO-021) is built and gated; the engine itself is still an empty `sl_core` and a `sl_sandbox` executable.

- Governance: 840-episode registry, controlled documents, ADR-0001 to ADR-0008, work-order checks, the factory contract (`FACTORY.md`).
- Build: CMake 3.28+ and Ninja with presets (schema 8) for clang and gcc, sanitizers (asan, ubsan, tsan), C++23 with a feature battery and C++26 probes (`cmake/`, `docs/build/README.md`).
- Vulkan: header, loader and validation-layer detection, slangc and spirv-val (no device is opened).
- Dependencies: one policy and one lock (`deps/`, `tools/deps/sldeps`), providers vendored, system, cpm and vcpkg through `sl_require()`; doctest and cpm are vendored under `third_party/`.
- Local CI: `scripts/ci-local.sh` and the acceptance run for episodes 041-048.
- Records: `docs/episodes/041-048.md` (done, decisions, acceptance, evidence, open issues per episode).

Unverified, by design of this slice (details in `docs/episodes/041-048.md`): Windows MSVC and clang-cl configure (ADR-0006), the CMake 3.28 floor (only 4.4.4 is tested, ADR-0005), `import std` beyond this machine, doctest upstream attestation, the cpm `url_hash` and the vcpkg baseline (ADR-0002).

## Gate commands

| Command | What it does |
|---|---|
| `just fast` | Stop-gate, under 60 s: layout, gates, tool tests, format, ADRs, work orders |
| `just check` | Full gate (`gate.full`): `fast` plus C++ build gates, dependency, feature and Vulkan checks, tests, every negative test and `ci-local` |
| `just check-full` | Release gate: `check` plus the ubsan, tsan, release and gcc workflows |
| `just ci-local` | The whole local CI from the repository root (`scripts/ci-local.sh`: doctor, format, governance checks, clang, gcc and asan workflows); no network |
| `just acceptance-041-048` | The four starter-prompt checks of episodes 041-048, logs in `build/acceptance/` |
| `just episodes-doc-check 041-048` | The episode record has a section per episode with its five sub-headings, a `just` recipe and an ADR or "none" |
| `just doctor` | Installed tools against `toolchain/environments.json` |

`just --list` shows every recipe. A recipe not implemented yet is a stub: `just stub <name>` prints `stub: owner WO-0xx` and exits 3; owners and status are in `tools/governance/recipes.json`.

## Layout (ADR-0001)

```
engine/{core,runtime,render,fabric}   engine libraries (only core gets code in this slice)
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
