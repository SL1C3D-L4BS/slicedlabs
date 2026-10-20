# engine/runtime

The native runtime of Volume 05 (master plan Volume 05, episodes 081-100; subsystem 03, Runtime Core): lifecycle,
platform boundary, memory and allocators, diagnostics, synchronization, jobs and work stealing, clocks, the
determinism kit, the frame scheduler, the archetype ECS, reflection, typed events, snapshots and the runtime side of
the document boundary. Architecture: `docs/arch/SL-ARCH-003-runtime-execution.md`; decisions ADR-0022 to ADR-0028.

Layout (WO-076 writes the CMake once; later work orders only add files):

- `include/sl/runtime/` public headers of `sl_runtime` (namespace `sl::runtime`, sub-namespaces jobs, ecs, mem, time,
  det, link, testing); `src/` its sources, globbed. Links only the standard library, `sl::core` and `sl::fabric`.
- `testing/` `sl_runtime_testing`: scripted platform, failpoint arming, allocator and queue contract suites, world
  generator, sim fixture, Fabric host, stress harness. The only target that links `sl::fabric_testing`.
- `tools/` executables (`sl_runtime_demo`, the headless gate runner; `sl_runtime_dump_contract`) and `tools/demo/`, the
  gate scene library. Never links `sl_fabric_testing`.

Volume 05 is headless: no window, display or input-device library (ADR-0002, ADR-0028). Recipes: `just runtime-test
<area>`, `just runtime-san <asan|ubsan|tsan> [area]`, `just runtime-gcc [area]`, `just runtime-tidy [area]`,
`just runtime-lint`, `just runtime-contracts`, then the gate recipes listed in SL-ARCH-003.

Status (WO-076): the CMake is written. Landed targets: `sl_runtime` (`sl::runtime`; `error.hpp`, the runtime error
domain over `sl::errc` 6000-6999, and `faults.hpp`, failpoints RFP0-RFP10 with SL_RUNTIME_FAULTS=1 in Debug and
sanitizer builds), `sl_runtime_testing` (`sl::runtime_testing`; `testing/faults_arm.hpp`, the only arming), the tool
globs of `tools/` (no tool yet), one `rt_<tier>_<area>` test executable per area of tests/unit/runtime and
tests/integration/runtime with one ctest entry `runtime_<stem>` per file, and the custom target
`sl_runtime_all_tests`. Scripts: `scripts/runtime/runtime-test.sh`, `runtime-san.sh` (fail-closed, log under
build/runtime/san/), `runtime-gcc.sh` and `runtime-tidy.sh`. Deviations: docs/arch/deviations/vol05/WO-076.md.
