# engine/runtime/tools/demo

The gate 100 lantern-field scene (WO-099, episode 100; SL-ARCH-003 ARCH3-R167 to R170, ADR-0028).
`tools/CMakeLists.txt` builds these files into the static library `sl_runtime_demo_scene`, which
`sl_runtime_demo` and the gate tests (`tests/integration/runtime/gate_*_test.cpp`) link. Nothing here
links `sl_fabric_testing` or `sl_runtime_testing` (runtime-lint R-RT-LINK).

- `host.hpp`, `host.cpp`: the in-tool Fabric host. A memory workspace seeded with 128
  `content.scene.Beacon` and 128 `content.scene.Ward` records whose types are registered in code
  through the public `schema.hpp`; it commits the runtime's observations (the proposal sink).
- `stack.hpp`, `stack.cpp`: the runtime stack composed from public pieces the way `runtime::start`
  composes them, plus the world link (deviation D-099-1 in `docs/arch/deviations/vol05/WO-099.md`).
- `systems.cpp`: the scene's systems over the pre_tick, tick, post_tick and observe phases.
- `scene.hpp`, `run.cpp`: the runner's three passes (fresh with the snapshot, fault and restore;
  resume from a snapshot; replay of the recorded wall samples) and its invariant checks.
- `evidence.cpp`: the command line (`--seed`, `--ticks`, `--workers`, `--mode`, `--entities`,
  `--snapshot-every`, `--evidence-dir`) and the evidence files.
