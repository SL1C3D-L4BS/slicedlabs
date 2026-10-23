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

Volume 05 is headless: no window, display or input-device library (ADR-0002, ADR-0028). Linux is the only platform
exercised; Windows is unverified (ADR-0006).

Status (WO-101, the runtime foundation is released; SL-ARCH-003 is InReview 0.3.0, not Approved): the code of all twenty
episodes 081-100 has landed. Public headers by area (the `just runtime-test <area>` label is the test file prefix):

| Area | Headers (`include/sl/runtime/`) | Episode |
| :---- | :---- | :---- |
| err, flt | `error.hpp` (error domain 6000-6999), `faults.hpp` (failpoints RFP0-RFP10) | scaffold |
| life | `runtime.hpp` (lifecycle states, recovery, swap protocol) | 081 |
| plat | `platform.hpp`, `headless_platform.hpp` | 082 |
| mem | `memory.hpp` (tags, tracking, leak report) | 083 |
| alloc | `allocators.hpp` (arena, stack, pool, frame arena) | 084 |
| diag | `memdiag.hpp` (allocation reports, runtime.mem events) | 085 |
| job | `jobs.hpp`, `work_queue.hpp` | 086 |
| sync | `sync.hpp` (cpu_fence, event_count, job_counter) | 087 |
| steal | `ws_deque.hpp`, `scheduler_stats.hpp` | 088 |
| clock, det | `time.hpp`, `replay.hpp` (clocks, stepper, hasher, rng streams, ordered merge) | 089 |
| frame | `frame.hpp` (phases, tasks, boundary hooks) | 090 |
| ent, store, query, cmd, sched, chg | `ecs/entity.hpp`, `ecs/component.hpp`, `ecs/world.hpp`, `ecs/query.hpp`, `ecs/command_buffer.hpp`, `ecs/system.hpp`, `ecs/change.hpp` | 091-095 |
| refl | `reflect.hpp` | 096 |
| evt | `events.hpp` | 097 |
| snap | `snapshot.hpp` | 098 |
| bnd | `bridge.hpp` (the Fabric link: projection in, observations out) | 091, 095 |
| stress | `testing/stress_harness.*`, tests/integration/runtime/stress_* | 099 |
| gate | `tools/demo/`, `tools/sl_runtime_demo.cpp` | 100 |

Tools: `sl_runtime_demo` (the lantern-field scene: 256 content.scene records, 50000 transient lanterns, 8 systems in 4
phases, spawn and despawn churn, a snapshot every 300 ticks, RFP5 at tick 450 with restore recovery, 600 ticks at 60
Hz; flags --seed, --ticks, --workers, --mode, --entities, --snapshot-every, --evidence-dir; exit 0 ok, 1 divergence or
invariant failure, 2 usage, 3 runtime fault) and `sl_runtime_dump_contract` (the contract tables the lock compares).
Python tools live in `tools/runtime/` (lint, contract checks and lock, memory report check, determinism check, stress
report, scorecard).

Gates (all recipes write under `build/`):

- `just runtime-gate` (WO-101): lint, strict contracts, contract lock, `runtime-test` for every area, `runtime-stress`,
  `runtime-demo`, `runtime-memreport-check`, `runtime-scorecard`, `fabric-surface-check` and every `neg-runtime-*`
  recipe. `just check` and `check-ratchet` run it after `fabric-gate`; `just fast` runs `runtime-lint` and
  `runtime-contracts-strict` only.
- `just check-full` adds `runtime-san` asan, ubsan and tsan and `runtime-determinism` (workers 1, 2, 4, 8 and 16, serial
  and parallel, resume from a snapshot, replay of recorded inputs; clang versus gcc is recorded, not gated).
- `just acceptance-081-100` runs `runtime-gate`, the sanitizer runs, `runtime-determinism` and `check-full`, logs
  13-16 in `build/acceptance/` and the evidence copied to `build/acceptance/runtime/`.
- Wall times are measured in this environment and recorded in `docs/episodes/081-100.md`, never asserted: `runtime-demo`
  takes about 500 s in Debug and `runtime-determinism` about 820 s.

Limits that remain (SL-ARCH-003 "Open issues" and `docs/arch/deviations/vol05/`): determinism class D1 for one binary
only; no window or input device; Linux only; one job system per runtime; spawn and despawn are not written back to the
document; the startup RFP0 sweep skips visit 2 (the std::pmr page list aborts by design, ARCH3-R025); a fault in the
first host frame has no recovery baseline and ends in recovery_failed.
