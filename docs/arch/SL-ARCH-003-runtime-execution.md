---
id: SL-ARCH-003
kind: ARCH
title: "Runtime and Execution Architecture"
status: Draft
version: 0.2.1
owner: founder
episodes: [81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100]
depends_on: [SL-ARCH-001, SL-ARCH-002]
supersedes: []
decisions: [ADR-0001, ADR-0002, ADR-0009, ADR-0015, ADR-0017, ADR-0021, ADR-0022, ADR-0023, ADR-0024, ADR-0025, ADR-0026, ADR-0027, ADR-0028]
max_lines: 1000
---
# SL-ARCH-003: Runtime and Execution Architecture

## Purpose

Subsystem 03, Runtime Core, owns live simulation state and consumes the component schemas of subsystem 02, the Fabric
(master plan sections 2.1 and 2.2, row 03). This document controls the native runtime of Volume 05 (episodes
081-100): layout and dependencies, the runtime error domain and failpoints, lifecycle and recovery, the platform
boundary, memory contracts, allocators and diagnostics, synchronization, the job system and work stealing, clocks,
the determinism contract and its replay seams, the frame scheduler, the archetype ECS (entities, storage, queries,
command buffers, systems, change tracking), reflection, typed events, snapshots, the runtime side of the document
boundary, stress verification and the P0 gate of episode 100 (master plan section 4.3). It decides what a builder
would otherwise guess: names, sizes, error codes, thread safety of every API, ownership, immutability and which state
is hashed. The document side of the boundary stays in SL-ARCH-001 (ARCH1-R60 to R62); replay itself is SL-ARCH-012
(Volume 17), which is named here and not designed.

## Status and decisions

Draft 0.2.1: written by WO-075 before any Volume 05 code (master plan, Authoring order), reconciled with the code by WO-100 (the builders' decisions are under "Code reconciliation (WO-100)") and its open code issues fixed by WO-109. Binding decisions:
ADR-0001 (layout), ADR-0002 (dependencies: none added; no SDL3 or windowing, the volume is headless), ADR-0009
(errors are values), ADR-0015 (failpoint pattern), ADR-0017 (authority), ADR-0021 (Volume 05 before Volume 02, the
mathematics allowed meanwhile, the Demonstrated cap), and the Volume 05 records ADR-0022 (error domain and fault
injection), ADR-0023 (memory model), ADR-0024 (concurrency and sanitizer policy), ADR-0025 (archetype storage),
ADR-0026 (determinism class D1 and replay seams), ADR-0027 (runtime side of the document boundary) and ADR-0028
(lifecycle, recovery, swap protocol and the P0 gate), all Proposed until the founder accepts them. The founder
approves this document through `registry/SL-REG-001.approvals.json`; builders never edit it (see the deviation rule
below); WO-100 reconciles it with the code (0.2.0, Code reconciliation section) and WO-101 moves it to InReview
(0.3.0). Rule ids are ARCH3-Rnnn; invariants are RT-INV-<AREA>-nn with AREA one of ERR FLT LINT PLAT MEM SYNC ALLOC
JOB ENT CLOCK DET DIAG STEAL REFL STORE FRAME QUERY CMD EVT CHG SCHED SNAP LIFE BND STRESS GATE, and a C++ test file
starts with the lower-case area (the ctest area of `just runtime-test <area>`). Every number a builder needs is in
the `limits` table; every table in an sl-data block is bound to the code by `just runtime-contract-lock` (WO-100).
The panel verdict this draft implements is build/design/vol05-judges.json (critic).

## Normative text

### Layout, dependencies and process (episodes 081-100; WO-076, WO-077)

- ARCH3-R001: Runtime code MUST live in engine/runtime: static library `sl_runtime` (include/sl/runtime/**, src/**), test library `sl_runtime_testing` (testing/**, the only target that links `sl::fabric_testing`) and executables under engine/runtime/tools; namespace `sl::runtime` with exactly the sub-namespaces jobs, ecs, mem, time, det, link and testing; every type is snake_case (ADR-0001).
- ARCH3-R002: `sl_runtime` MUST link only the standard library, `sl::core` and `sl::fabric` (PUBLIC, through sl_require) and MUST NOT add a third-party dependency (ADR-0002; master plan section 5.1, row Platform layer).
- ARCH3-R003: Volume 05 MUST be headless: SDL3, Wayland and every windowing or input-device library stay out, `window` is an interface whose only implementation is headless, and gate 100 runs with no display, window or input device (ADR-0028; ADR-0002; master plan section 4.3).
- ARCH3-R004: Files under engine/runtime/include and engine/runtime/src MUST include directly only these sl/fabric headers: boundary.hpp, id.hpp, value.hpp, digest.hpp, schema.hpp, type.hpp, patch.hpp, events.hpp and clock.hpp, plus vfs.hpp in src/platform_files*.cpp only (ADR-0027; master plan section 3, F5).
- ARCH3-R005: Files under engine/runtime/testing and engine/runtime/tools MAY also include workspace.hpp, transaction.hpp and authority.hpp, and nothing under engine/runtime MAY include sl/fabric/internal/**, store.hpp or f5.hpp (ARCH1-R61; ADR-0027; master plan section 3, F5).
- ARCH3-R006: Code under engine/runtime/include and engine/runtime/src MUST NOT name `transaction_manager`, `transaction`, `workspace` or `tx_options`; write-back leaves only through `link::proposal_sink` (ADR-0027; master plan section 2.1).
- ARCH3-R007: Runtime code MUST use only the mathematics of the ADR-0021 allow-list and MUST NOT define or vendor vector, matrix, quaternion, transform, SIMD, fixed-point, curve or geometry types or include intrinsic headers; transforms are opaque component payloads (ADR-0021).
- ARCH3-R008: Runtime code MUST NOT hold mutable namespace-scope, class-static or function-static state except the failpoint registry in src/faults.cpp (allow-listed in tools/runtime/lint_allow.json), so two runtimes in one process stay independent (ADR-0028; master plan section 2.3).
- ARCH3-R009: A runtime header MUST NOT include a header of a higher layer of the `layers` table, checked by runtime-lint R-RT-LAYER (ADR-0001).
- ARCH3-R010: engine/runtime MUST NOT contain throw, try, catch, `.at(` or `std::sto*`; job bodies, system bodies, method invokers and event handlers MUST be noexcept by type and report errors through `jobs::job_context::fail` (ADR-0009; ADR-0024).
- ARCH3-R011: The build MUST keep exceptions enabled (no -fno-exceptions) because sl_fabric and the standard library report through them; no exception MAY cross a runtime API (ADR-0009).
- ARCH3-R012: `sl_runtime` MUST carry -ffp-contract=off as INTERFACE and PRIVATE compile option and MUST NOT be built with -ffast-math, so systems instantiated in consumer translation units get the same floating-point contract (ADR-0026).
- ARCH3-R013: Builders MUST NOT edit this document; every builder work order MUST record each decision this document leaves open, and each place its code departs from it, in docs/arch/deviations/vol05/WO-0nn.md (decision, why, file and symbol), and WO-100 MUST reconcile them into a Code reconciliation section (master plan section 6.3, rule 4; ADR-0028).
- ARCH3-R014: Every RT-INV row MUST bind to exactly one test file and one owner work order; C++ case names start with `RT-INV-<AREA>-<nn>:` and Python test methods with `test_rt_inv_<area>_<nn>_` (master plan section 6.1; ADR-0020 pattern).
- ARCH3-R015: Runtime recipes and tests MUST write only under build/ (evidence under build/runtime/**); committed goldens MAY change only through `python3 -I tools/runtime/<tool>.py write`, which no acceptance command runs (master plan section 6.1, acceptance suite).
- ARCH3-R016: A unit test case MUST finish under 1 s and a unit ctest entry under 10 s; integration entries MUST finish under 60 s; stress and gate entries MUST scale with SL_RUNTIME_STRESS_SCALE (1.0 by default, 0.1 when SL_SANITIZE is set) and run RUN_SERIAL with RESOURCE_LOCK cpu (master plan section 6.1).

```sl-data
{"layers": [
{"layer": "L0", "headers": "error.hpp, faults.hpp"},
{"layer": "L1", "headers": "platform.hpp, headless_platform.hpp, memory.hpp"},
{"layer": "L2", "headers": "sync.hpp, allocators.hpp, memdiag.hpp, time.hpp, replay.hpp"},
{"layer": "L3", "headers": "jobs.hpp, work_queue.hpp, ws_deque.hpp, scheduler_stats.hpp"},
{"layer": "L4", "headers": "frame.hpp, reflect.hpp"},
{"layer": "L5", "headers": "ecs/entity.hpp, ecs/component.hpp, ecs/world.hpp"},
{"layer": "L6", "headers": "ecs/query.hpp, ecs/command_buffer.hpp"},
{"layer": "L7", "headers": "ecs/change.hpp, ecs/system.hpp, events.hpp"},
{"layer": "L8", "headers": "snapshot.hpp"},
{"layer": "L9", "headers": "runtime.hpp, bridge.hpp (neither includes the other)"}
],
"rule": "a header includes only headers of its own or a lower layer; same-layer edges must be acyclic"}
```

### Errors, assertions and failpoints (ADR-0009, ADR-0022)

- ARCH3-R017: Every fallible runtime call MUST return `sl::result<T>`; runtime codes are `sl::errc` values in 6000-6999 of the `error_codes` table with `error_domain::runtime` appended after `unknown`, all added by WO-076 in one edit and never renumbered (ADR-0022; ADR-0009).
- ARCH3-R018: No runtime API MAY return a runtime code outside the `error_codes` table, or a core code other than invalid_argument, out_of_range, not_found, already_exists, unsupported, internal, io_error and timed_out (ADR-0022).
- ARCH3-R019: SL_ASSERT and SL_DEBUG_ASSERT MUST be reserved for the broken invariants of the `assertions` list; misuse a caller can detect and handle MUST return an error (ADR-0009).
- ARCH3-R020: Failpoints RFP0-RFP10 MUST be the visit points of the `failpoints` table, compiled when SL_RUNTIME_FAULTS=1 (Debug and sanitizer presets) and compiled out of Release, armed only through `sl::runtime::testing` (faults_arm.hpp), and the outcome after each MUST match the table (ADR-0022; ADR-0015 pattern).
- ARCH3-R021: `sl::runtime::install_terminate_report()` MAY be called by hosts (the demo, test mains) and the library MUST NOT call it itself (ADR-0028; ADR-0009).

```sl-data
{"error_codes": [
{"name": "runtime_failure", "number": 6000, "area": "general", "returned_by": "runtime::start when a platform call fails with no more specific code"},
{"name": "invalid_state", "number": 6001, "area": "lifecycle", "returned_by": "runtime lifecycle calls outside rt_states; job_counter::reset with a waiter"},
{"name": "wrong_thread", "number": 6002, "area": "lifecycle", "returned_by": "owner-thread APIs called from another thread (runtime, frame_scheduler, world structural API)"},
{"name": "restart_required", "number": 6003, "area": "lifecycle", "returned_by": "runtime::request_swap result for component_layout, allocator, scheduler and runtime_abi classes"},
{"name": "recovery_failed", "number": 6004, "area": "lifecycle", "returned_by": "runtime::run_frame after max_recoveries faults in the window"},
{"name": "swap_rejected", "number": 6005, "area": "lifecycle", "returned_by": "runtime::swap_status after a failed health check (pre-swap snapshot restored)"},
{"name": "runtime_faulted", "number": 6006, "area": "lifecycle", "returned_by": "runtime::run_frame when a fault stops the runtime under policy stop"},
{"name": "thread_spawn_failed", "number": 6010, "area": "platform", "returned_by": "thread_factory::spawn; runtime::start (RFP1)"},
{"name": "platform_unavailable", "number": 6011, "area": "platform", "returned_by": "make_headless_platform; window calls that need a display"},
{"name": "out_of_memory", "number": 6020, "area": "memory", "returned_by": "mem::resource::try_allocate and every try_* allocation path (RFP0)"},
{"name": "bad_alignment", "number": 6021, "area": "memory", "returned_by": "mem::resource::try_allocate with a non-power-of-two or > 4096 alignment"},
{"name": "arena_exhausted", "number": 6022, "area": "memory", "returned_by": "arena_resource, stack_resource and pool_resource with a fixed capacity and no upstream"},
{"name": "leak_detected", "number": 6023, "area": "memory", "returned_by": "runtime::shutdown in test and gate configurations; mem::resource destruction report"},
{"name": "tag_registry_frozen", "number": 6024, "area": "memory", "returned_by": "mem::tag_registry::add after freeze"},
{"name": "tag_reserved", "number": 6025, "area": "memory", "returned_by": "mem::tag_registry::add with a gpu.* or fabric.* name"},
{"name": "job_capacity", "number": 6030, "area": "jobs", "returned_by": "jobs::job_system::try_submit at capacity"},
{"name": "job_cancelled", "number": 6031, "area": "jobs", "returned_by": "jobs::job_system::wait on a job cancelled by a failed predecessor or by shutdown"},
{"name": "job_failed", "number": 6032, "area": "jobs", "returned_by": "jobs::job_system::wait on a job that called job_context::fail"},
{"name": "scheduler_stopped", "number": 6033, "area": "jobs", "returned_by": "jobs::job_system::submit and try_submit after stop"},
{"name": "deadlock_suspected", "number": 6034, "area": "jobs", "returned_by": "jobs::watchdog report (Debug and sanitizer builds), then SL_ASSERT"},
{"name": "stale_job", "number": 6035, "area": "jobs", "returned_by": "jobs::job_system::status on a handle whose generation was reused"},
{"name": "invalid_tick_rate", "number": 6040, "area": "time", "returned_by": "time::fixed_stepper::make with num or den 0"},
{"name": "schedule_frozen", "number": 6045, "area": "frame", "returned_by": "frame::frame_scheduler::add_task and ecs::system_scheduler::add after build"},
{"name": "stale_entity", "number": 6050, "area": "ecs", "returned_by": "ecs::world checked accessors; ecs::entity_allocator::check and destroy (entity_map answers not_found, never stale_entity)"},
{"name": "entity_capacity", "number": 6051, "area": "ecs", "returned_by": "ecs::world::create and command playback beyond max_entities (RFP10)"},
{"name": "component_unknown", "number": 6052, "area": "ecs", "returned_by": "ecs::world add, get and remove of an unregistered component"},
{"name": "component_missing", "number": 6053, "area": "ecs", "returned_by": "ecs::world::get and remove of a component the entity lacks"},
{"name": "component_exists", "number": 6054, "area": "ecs", "returned_by": "ecs::world::add of a component the entity already has"},
{"name": "component_invalid", "number": 6055, "area": "ecs", "returned_by": "ecs::component_registry::add of a type above 2048 bytes or 64-byte alignment, or beyond 1024 types"},
{"name": "component_registry_frozen", "number": 6056, "area": "ecs", "returned_by": "ecs::component_registry::add after freeze"},
{"name": "archetype_limit", "number": 6057, "area": "ecs", "returned_by": "ecs::world structural calls that would exceed 4096 archetypes or 32 components"},
{"name": "structural_change_forbidden", "number": 6058, "area": "ecs", "returned_by": "ecs::world structural calls inside a phase"},
{"name": "query_invalid", "number": 6059, "area": "ecs", "returned_by": "ecs::query construction with contradictory terms"},
{"name": "schedule_cycle", "number": 6070, "area": "scheduling", "returned_by": "ecs::system_scheduler::build"},
{"name": "undeclared_access", "number": 6071, "area": "scheduling", "returned_by": "the Debug access guard, through job_context::fail of the offending system"},
{"name": "access_conflict", "number": 6072, "area": "scheduling", "returned_by": "ecs::system_scheduler::add when a system's queries need access its access_set lacks"},
{"name": "system_unknown", "number": 6073, "area": "scheduling", "returned_by": "ecs::system_scheduler::build for an unknown before/after name"},
{"name": "type_unknown", "number": 6080, "area": "reflection", "returned_by": "type_registry::find and type_registry::method (static)"},
{"name": "field_unknown", "number": 6081, "area": "reflection", "returned_by": "type_registry::field (static) lookup by name"},
{"name": "reflect_mismatch", "number": 6082, "area": "reflection", "returned_by": "method_info invoke with wrong argument kinds"},
{"name": "type_registry_frozen", "number": 6083, "area": "reflection", "returned_by": "type_registry::add and event_registry::add after freeze"},
{"name": "component_schema_mismatch", "number": 6084, "area": "reflection", "returned_by": "fabric_binding::bind against the frozen schema_registry"},
{"name": "event_overflow", "number": 6090, "area": "events", "returned_by": "event_channel<E>::try_write at event_capacity (RFP7)"},
{"name": "event_type_unknown", "number": 6091, "area": "events", "returned_by": "event_registry::find; event_channel lookup of an unregistered type"},
{"name": "event_payload_invalid", "number": 6092, "area": "events", "returned_by": "event_channel<E>::write payload validation (Debug)"},
{"name": "snapshot_corrupt", "number": 6100, "area": "snapshot", "returned_by": "snapshot::load and restore on a trailer or structure mismatch"},
{"name": "snapshot_version", "number": 6101, "area": "snapshot", "returned_by": "snapshot::load of an unknown major"},
{"name": "snapshot_schema_mismatch", "number": 6102, "area": "snapshot", "returned_by": "snapshot::restore when a type digest differs"},
{"name": "snapshot_mid_tick", "number": 6103, "area": "snapshot", "returned_by": "snapshot::take and restore outside end_frame"},
{"name": "snapshot_abi_mismatch", "number": 6104, "area": "snapshot", "returned_by": "snapshot::restore when runtime_abi_digest differs"},
{"name": "snapshot_contributor_unknown", "number": 6105, "area": "snapshot", "returned_by": "snapshot::restore when a recorded contributor (name, version) is not registered"},
{"name": "link_observation_rejected", "number": 6110, "area": "link", "returned_by": "link::world_link flush_report entries when the sink refuses a patch (counted, never a frame error)"},
{"name": "link_transient_component", "number": 6111, "area": "link", "returned_by": "link::world_link::observe_component for a component with no binding"},
{"name": "projection_stale", "number": 6112, "area": "link", "returned_by": "link::world_link::post_projection with a generation at or below the current one"}
],
"domain": "error_domain::runtime (appended after unknown; domain() maps 6000-6999)",
"core_codes_reused": ["invalid_argument", "out_of_range", "not_found", "already_exists", "unsupported", "internal", "io_error", "timed_out"],
"reserved": "6007-6009, 6012-6019, 6026-6029, 6036-6039, 6041-6044, 6046-6049, 6060-6069, 6074-6079, 6085-6089, 6093-6099, 6106-6109, 6113-6999",
"table_version": 1}
```

```sl-data
{"assertions": [
"unchecked access through a stale entity handle (SL_DEBUG_ASSERT)",
"a structural call while a query iterates (Debug iteration guard)",
"an out-of-order stack_resource free",
"an owner-thread allocator used from another thread (Debug owner check)",
"a double free or a free to the wrong resource (Debug)",
"do_allocate with no upstream when the resource is exhausted (abort with the tag report)",
"ws_deque push or pop from a thread other than its owner worker (Debug)",
"the watchdog after a deadlock_suspected report (Debug and sanitizer builds)"
]}
```

```sl-data
{"failpoints": [
{"id": "RFP0", "name": "alloc.fail", "site": "mem::resource::try_allocate (Nth call or per tag)", "outcome": "out_of_memory; the operation leaves its object unchanged; inside playback the frame faults; testing::failing_resource injects without visiting RFP0"},
{"id": "RFP1", "name": "thread.spawn", "site": "platform thread_factory::spawn", "outcome": "thread_spawn_failed; runtime::start unwinds to Created with no thread left"},
{"id": "RFP2", "name": "job.error", "site": "jobs::job_system before running a job body", "outcome": "the job fails with job_failed; dependents job_cancelled; runtime state unchanged"},
{"id": "RFP3", "name": "job.fatal", "site": "jobs::job_system before running a job body", "outcome": "fail_fatal: job_failed, the worker continues, the runtime moves to Faulted at end_frame"},
{"id": "RFP4", "name": "worker.stall", "site": "jobs worker loop at before_pop, after_steal, before_complete and in_wait", "outcome": "a stall of min(param, 2000) us; results and state hashes unchanged"},
{"id": "RFP5", "name": "system.fail", "site": "ecs::system_scheduler after a system body", "outcome": "visited after the system body returns: the system job fails with job_failed; Faulted at end_frame; recovery per policy"},
{"id": "RFP6", "name": "clock.jump", "site": "time::frame_clock::sample", "outcome": "a backward jump gives delta 0 and non_monotonic + 1; a hitch is clamped to max_frame_delta"},
{"id": "RFP7", "name": "event.overflow", "site": "event_channel<E>::write", "outcome": "visited by write and try_write: the channel behaves as full, the event is dropped and counted (try_write returns event_overflow)"},
{"id": "RFP8", "name": "snapshot.io", "site": "snapshot save, once before atomic_write", "outcome": "one visit per save before atomic_write: io_error and no primitive runs (the old snapshot loads); a crash after each primitive comes from the faulty vfs"},
{"id": "RFP9", "name": "suspend.request", "site": "runtime at each boundary slot and mid-phase", "outcome": "visited in every boundary slot and at every phase end: request_suspend issued there; it takes effect at end_frame and the hash is unchanged"},
{"id": "RFP10", "name": "entity.exhaust", "site": "ecs::entity_allocator::create", "outcome": "param 1 fast-forwards the chosen slot to generation 0xFFFFFFFE so it retires; any other param forces entity_capacity"}
],
"compiled": "SL_RUNTIME_FAULTS=1 in debug, gcc-debug, ci-asan, ci-ubsan, ci-tsan; 0 in release presets",
"armed_by": "sl::runtime::testing::arm(failpoint, at_visit, sticky, param) in engine/runtime/testing/faults_arm.hpp",
"table_version": 1}
```

### Runtime lifecycle and recovery (episode 081; ADR-0028)

- ARCH3-R022: `runtime` MUST follow exactly the `rt_states` table; any other (state, event) pair returns invalid_state and changes nothing (ADR-0028; master plan, Volume 05, episode 081).
- ARCH3-R023: The owner thread (the frame thread) is the thread that called `runtime::create`; lifecycle calls and run_frame MUST run on it (wrong_thread otherwise), and request_suspend, request_stop and request_swap MAY be called from any thread and take effect at the next end_frame (master plan section 2.5).
- ARCH3-R024: `runtime::run_frame()` and `run_frames(n)` MUST be the only frame-loop primitives: the runtime never runs an internal infinite loop or sleeps for wall time, the host pumps frames (master plan section 2.3; ADR-0028).
- ARCH3-R025: Startup MUST be all-or-nothing: platform check, memory root, tag freeze, registry freezes, workers, phase graphs and worlds in that order, and a failure at any step unwinds the completed steps in reverse and leaves no thread, tracked byte or registration (ADR-0028; master plan, Volume 05, episode 081). Test limit (WO-109): RFP0 reaches two allocations of start, the root block and the world's location page table, and life_startup_unwind_test fails each; the entity allocator's page list goes through std::pmr, where out_of_memory aborts by design (ADR-0023, WO-088), the job system allocates nothing tracked, and the runtime builds its own resources, so testing::failing_resource cannot wrap them from outside.
- ARCH3-R026: Suspend MUST take effect at end_frame, park the workers and freeze every clock: no tick accrues while suspended, the first wall delta after resume is 0 and the logical tick is unchanged (ADR-0028; master plan, Volume 05, episodes 081 and 089).
- ARCH3-R027: Shutdown MUST cancel pending background jobs (job_cancelled), drain frame jobs, join every worker, flush diagnostics and produce a leak report; in test and gate configurations live tracked bytes at shutdown make shutdown return leak_detected (ADR-0023; master plan, Volume 05, episode 085).
- ARCH3-R028: A fault (a fatal job failure, RFP3, RFP5, a system error or out_of_memory during playback) MUST move Running to Faulted at end_frame without playing back that frame's command buffers, flipping its events or flushing its observations, and MUST produce one sl-runtime-fault/1 report (ADR-0028).
- ARCH3-R029: Recovery policy MUST be `stop` by default (production) or `restore_snapshot` (tests and the gate): restore the newest snapshot at or before the fault tick and continue, bounded by max_recoveries per window, beyond which the runtime stops with recovery_failed (ADR-0028; master plan, Volume 05, episodes 081 and 098).
- ARCH3-R030: Background jobs MUST NOT touch world state; their results enter as recorded external inputs drained at begin_frame, and a restore cancels in-flight background jobs by generation and discards their results (ADR-0024; ADR-0026).
- ARCH3-R031: `request_swap` MUST apply only at end_frame: classes data and gameplay_function take a pre-swap snapshot, apply, run the host health check for health_ticks ticks and restore the snapshot on failure (swap_rejected); classes component_layout, allocator, scheduler and runtime_abi return restart_required (master plan section 2.5; ADR-0028).
- ARCH3-R032: `runtime_abi_digest` MUST be SHA-256 over chunk_bytes, handle layout, the component registry layout digest, the errc table version, the failpoint table version and the ordered_key layout version, and MUST be written into every snapshot (master plan section 2.5; ADR-0025).
- ARCH3-R033: Each runtime MUST carry the host-supplied `runtime_config::instance_id` in its log correlation scope (`runtime/<id>`), and two runtimes in one process MUST share no mutable state (master plan section 2.3; ADR-0028).
- ARCH3-R034: Every lifecycle transition MUST emit exactly one sl::log event, system `runtime.lifecycle`, fields from, to, tick and instance (master plan section 3, F6).

```sl-data
{"rt_states": {"states": ["Created", "Starting", "Ready", "Running", "Suspended", "Stopping", "Stopped", "Faulted", "Recovering"], "transitions": [["Created", "start", "Starting", "runtime::start (owner thread)"], ["Starting", "start_ok", "Ready", "every startup step succeeded"], ["Starting", "start_fail", "Created", "completed steps unwound in reverse; start returns the error"], ["Ready", "run_frame", "Running", "first run_frame"], ["Running", "suspend", "Suspended", "request_suspend took effect at end_frame; clocks frozen"], ["Suspended", "resume", "Running", "runtime::resume (owner thread); first delta 0"], ["Ready", "stop", "Stopping", "request_stop or shutdown"], ["Running", "stop", "Stopping", "request_stop took effect at end_frame, or shutdown"], ["Suspended", "stop", "Stopping", "request_stop or shutdown"], ["Running", "fault", "Faulted", "a fault at end_frame; the faulted frame is not played back"], ["Faulted", "recover", "Recovering", "policy restore_snapshot and the window is under max_recoveries"], ["Recovering", "recover_ok", "Running", "snapshot restored; the next run_frame continues from its tick"], ["Recovering", "recover_fail", "Stopping", "restore refused; run_frame returns recovery_failed"], ["Faulted", "stop", "Stopping", "policy stop (default) or max_recoveries exceeded"], ["Stopping", "stopped", "Stopped", "jobs drained, workers joined, diagnostics flushed, leak report written"]], "events": ["start", "start_ok", "start_fail", "run_frame", "suspend", "resume", "stop", "fault", "recover", "recover_ok", "recover_fail", "stopped"], "readable_any_thread": ["state()", "last_fault()", "memory_report()"]}}
```

### Platform boundary (episode 082)

- ARCH3-R035: The runtime MUST reach the OS only through `platform` (clock_source, wall_clock, thread_factory, input_source, file_access, window, record_sink): outside src/platform_*.cpp no runtime file calls a std::chrono clock's now(), constructs std::thread or std::jthread, uses std::random_device or opens a file (master plan section 5.1, row Platform layer; ADR-0026).
- ARCH3-R036: Volume 05 MUST ship `headless_platform` (Linux) and `testing::scripted_platform` (manual clock, scripted input, failing thread factory arming RFP1, faulty vfs) and no windowed platform (master plan section 5.1; ADR-0006).
- ARCH3-R037: Platform handles MUST be RAII and move-only and MUST be released in reverse acquisition order (master plan section 5.1, row Platform layer).
- ARCH3-R038: `file_access` MUST be implemented over `sl::fabric::vfs` so the faulty vfs injects storage faults into runtime I/O, and `atomic_write` writes a temp file, fsyncs it, renames it and fsyncs the directory (ADR-0015 pattern; master plan section 5.1).
- ARCH3-R039: Every value that enters the simulation from outside (wall samples, input events, file reads, background results) MUST pass `record_sink` exactly once in arrival order, stamped with the frame index and tick that consume it (Volume 17 seam; master plan section 4.2).
- ARCH3-R040: `thread_factory` MUST name threads `slrt<instance>-w<k>` and accept and ignore an affinity hint (master plan section 5.1, row Job system).

### Memory allocation contracts (episode 083; ADR-0023)

- ARCH3-R041: Every runtime allocator MUST derive from `mem::resource`, a std::pmr::memory_resource that adds `try_allocate(bytes, align, site) noexcept -> sl::result<void*>`; `is_equal` is identity and resources are neither copyable nor movable (ADR-0023).
- ARCH3-R042: `do_allocate` MUST forward to the configured upstream or, with none, SL_ASSERT-abort with a tag report; it never throws across runtime code, and runtime code on fallible paths calls try_allocate (ADR-0023; ADR-0009).
- ARCH3-R043: Alignment MUST be a power of two no greater than 4096 (bad_alignment otherwise), a zero-byte request is served as one byte, and the returned address is aligned to max(align, alignof(std::max_align_t)) (ADR-0023).
- ARCH3-R044: Each resource MUST have one owner and lifetimes MUST nest; destroying a resource with live blocks reports each block's tag, size and, in Debug and sanitizer builds, its std::source_location site (ADR-0023; master plan, Volume 05, episode 083).
- ARCH3-R045: Every tracked allocation MUST carry a `mem::memory_tag` from the tag registry, which freezes at runtime start (tag_registry_frozen after); the prefixes gpu.* and fabric.* are reserved for `reserve_external` (tag_reserved through add) (ADR-0023; master plan section 5.1, row Memory subsystem).
- ARCH3-R046: Debug builds MUST fill fresh memory with 0xCD and freed memory with 0xDD, and recycling resources MUST poison freed and reset ranges with ASAN_POISON_MEMORY_REGION under ASan (ADR-0023).
- ARCH3-R047: Volume 05 MUST NOT replace global operator new or delete; untracked allocations stay outside the diagnostics (seam V16-3; ADR-0023).

### Foundational allocators (episode 084)

- ARCH3-R048: Volume 05 MUST provide arena_resource (markers, rewind, reset), stack_resource (LIFO markers), pool_resource (fixed blocks of at least 16 bytes from 64 KiB slabs, intrusive LIFO free list), frame_arena (double-buffered), synchronized_resource (mutex adapter) and general_resource (upstream adapter), each passing the allocator contract suite (ADR-0023; master plan, Volume 05, episode 084).
- ARCH3-R049: Only synchronized_resource and general_resource MAY be used from more than one thread; the others are owner-thread only and SL_DEBUG_ASSERT the owner (ADR-0024).
- ARCH3-R050: Memory allocated from a frame arena in frame N MUST stay valid until the end_frame of frame N+1 and no longer; the flip that ends N+1 resets and poisons the buffer of N (ADR-0023; master plan, Volume 05, episode 090).
- ARCH3-R051: After a 10-frame warm-up a steady frame MUST make zero upstream allocations (ADR-0023; master plan section 6.1, work-bound performance).

### Allocation diagnostics (episode 085)

- ARCH3-R052: `memory_report` MUST give per tag live bytes and blocks, peak bytes, allocation and failure counts, per allocator fragmentation in per-mille (1000 - 1000 x largest free run / total free, 0 with no free space), and the external tags separately (ADR-0023; master plan, Volume 05, episode 085).
- ARCH3-R053: Reports MUST serialize as sl-runtime-memreport/1 valid against schemas/runtime/memreport.schema.json, and the runtime MUST emit one log event of system `runtime.mem` every report_interval frames and at shutdown (master plan section 3, F6; section 5.1, row Memory subsystem).
- ARCH3-R054: Leak reports MUST list each live block's tag, site and size sorted by (tag, site, size), byte-identical across runs and worker counts (ADR-0026).
- ARCH3-R055: Counters MUST be relaxed atomics or per-thread; reading a report MUST NOT block allocation, and Release tracking costs at most one relaxed atomic add per counter per call (ADR-0024).
- ARCH3-R056: Memory the runtime does not allocate MUST be reported through `report_external(tag, bytes)` under a reserved tag (fabric.projection, gpu.*) and never counted in tracked live bytes (ADR-0023; ADR-0027).

### Synchronization primitives (episode 087; ADR-0024)

- ARCH3-R057: Synchronization MUST use std::atomic, std::mutex, std::condition_variable, std::atomic wait and notify, std::latch, std::barrier and std::counting_semaphore only; std::atomic_thread_fence, volatile as synchronization, inline assembly and raw futex calls are forbidden (ADR-0024).
- ARCH3-R058: A memory_order weaker than seq_cst MUST carry a `// pairs: <symbol>` comment naming its pairing operation, checked by runtime-lint R-RT-ORDER (ADR-0024).
- ARCH3-R059: `cpu_fence` MUST be one-shot (released when its count reaches zero, waiters released once, never reset), and `job_counter` MAY be reset only with no waiter (invalid_state otherwise) (ADR-0024; master plan, Volume 05, episode 087).
- ARCH3-R060: Waits MUST spin at most spin_limit iterations, then park; `event_count` keeps its epoch and sleeper count seq_cst so a notify racing a park is never lost (ADR-0024).

### Job system (episode 086; ADR-0024)

- ARCH3-R061: A job MUST be a `job_desc` {fn: a noexcept callable stored inline in 48 bytes, priority frame or background, affinity any or main, a static debug name}; it runs exactly once, after all of its dependencies completed (master plan section 5.1, row Job system; ADR-0024).
- ARCH3-R062: Job records MUST come from a bounded pool of job_capacity records of 128 bytes aligned to 64, addressed by `job_handle` {index u32, generation u32}; `status()` of a stale handle is stale_job and `wait()` on it returns ok (ADR-0024).
- ARCH3-R063: Dependencies MUST be job_counter or cpu_fence values of already-submitted jobs, at most 8 per submit, so a dependency cycle cannot be expressed (master plan, Volume 05, episode 086).
- ARCH3-R064: `wait` MUST help: a waiting worker or the owner thread runs other ready jobs, and fork-join nested to depth 64 completes on one worker (ADR-0024).
- ARCH3-R065: `submit` at capacity MUST help by running ready jobs until a record frees, and only a job with no predecessors MAY run inline on the calling thread; `try_submit` returns job_capacity (ADR-0024).
- ARCH3-R066: Main-affinity jobs MUST run only on the owner thread, drained at phase boundaries or inside an owner-thread wait (master plan section 5.1, row Platform layer).
- ARCH3-R067: `job_context::fail(code)` MUST mark the job job_failed, cancel its transitive dependents with job_cancelled and surface the code to the waiter; `fail_fatal(code)` also raises a runtime fault for the next end_frame; a running job is never interrupted (ADR-0009; ADR-0028).
- ARCH3-R068: A job MUST run under the submitter's sl::log correlation {id, scope, tick}; correlation ids are diagnostic only and never enter hashed or snapshotted state, an ordered_key or an event order (ADR-0026; master plan section 3, F6).
- ARCH3-R069: In Debug and sanitizer builds a watchdog MUST report deadlock_suspected when no job completes for watchdog_ms while jobs are pending, logging every worker's current job, queue depth and wait target, then SL_ASSERT, before the ctest timeout (ADR-0024; master plan, Volume 05, episode 099).
- ARCH3-R070: The job system MUST take its queues and victim policy through the `work_queue` and `scheduler_policy` interfaces of work_queue.hpp; `locked_deque` with the reference policy is the default, and `make_ws_queue` returns unsupported until WO-086 (ADR-0024).
- ARCH3-R071: The worker count MUST default to hardware_concurrency - 1 - reserved_threads (at least 1), with the owner thread helping; `workers = 0` runs every job on the owner thread (ADR-0024; master plan section 5.1, row Job system).

### Work stealing and scheduling policy (episode 088)

- ARCH3-R072: `ws_deque` MUST be Chase-Lev with top and bottom as std::atomic<std::int64_t> on separate 64-byte lines: push stores the element relaxed then bottom release, pop and steal use seq_cst on bottom, top and the claiming CAS, with no standalone fence; the ring starts at 4096 cells, doubles up to 65536 and retired rings are freed only at the next quiescent end_frame (ADR-0024).
- ARCH3-R073: `locked_deque` and `ws_deque` MUST both pass the queue contract suite; the lock-free deque becomes the runtime default only when the STEAL invariants pass under `just runtime-san tsan steal` (ADR-0024).
- ARCH3-R074: The stealing policy MUST pick victims with a per-worker xorshift64 seeded from (seed, worker index), at most 2 x workers attempts per round, poll the injection queue every 61 local pops, spin spin_limit rounds and then park on the scheduler event_count (ADR-0024; master plan, Volume 05, episode 088).
- ARCH3-R075: A background job waiting more than aging_limit scheduler rounds MUST be promoted ahead of frame jobs at the next pop, so background work never starves (master plan, Volume 05, episode 088).
- ARCH3-R076: `scheduler_stats` MUST count steals, failed steals, parks, wakes, injection polls and aging promotions per worker as relaxed counters summed on read from any thread; they are diagnostic and never asserted as times (ADR-0024; master plan section 6.1).

### Clocks and logical time (episode 089)

- ARCH3-R077: `time::tick` MUST be a std::uint64_t starting at 0 after ingest that advances only by whole fixed steps, never derived from wall time (ADR-0026; master plan section 4.2).
- ARCH3-R078: Time MUST be integer nanoseconds; the fixed stepper keeps a rational `tick_rate` {num, den} (ticks per second num/den), an accumulator in units of ns x num, and one tick costs den x 10^9 units, so no floating point enters tick progression and there is no drift (ADR-0026).
- ARCH3-R079: Wall time MUST enter as one monotonic `clock_source` sample per frame in begin_frame; a sample at or before the previous one gives delta 0 and counts non_monotonic (RFP6), and a delta above max_frame_delta is clamped with the excess added to dropped_ns (ADR-0026; master plan, Volume 05, episode 089).
- ARCH3-R080: A frame MUST run at most max_catchup ticks; whole ticks beyond the cap are dropped and counted in dropped_ticks and the remainder stays below one tick (ADR-0026).
- ARCH3-R081: logical clock, fixed stepper and frame clock MUST be owned by the frame thread; jobs read the immutable `time::frame_time` value {frame_index, tick, delta_ns, alpha_num, alpha_den} (ADR-0024).
- ARCH3-R082: `wall_clock` (std::chrono::system_clock) MUST be used only for log time stamps, never for simulation (ADR-0026; master plan section 4.2).
- ARCH3-R083: While a tick runs, `time::tick_scope` MUST set the calling thread's sl::log correlation tick to the tick and its scope to `runtime/<instance>`, and jobs submitted inside it inherit both (master plan section 3, F6; ADR-0009).

### Determinism contract and replay seams (ADR-0026)

- ARCH3-R084: The runtime MUST offer `det::determinism_mode` {throughput, deterministic}; in deterministic mode the per-tick state hash, event order and observation patch bytes MUST be pure functions of (initial projection, seed, recorded external inputs, configuration) on the same binary for any worker count (ADR-0026; master plan section 4.2).
- ARCH3-R085: Volume 05 MUST claim determinism class D1 only (same binary, same host ISA); clang-versus-gcc equality is recorded and gated only by Volume 17 (SL-ARCH-012) (ADR-0026).
- ARCH3-R086: Every merge of per-job buffers (command buffers, events, observations, background results) MUST go through `det::merge_ordered` over `det::ordered_key` {tick u64, phase u8, system_index u16, chunk_index u32, local_seq u32}, compared lexicographically, at exactly the merge points of the `determinism` table (ADR-0026).
- ARCH3-R087: Deterministic paths MUST NOT iterate unordered containers, order by pointer, address, thread id or std::hash, or use std::random_device, default-seeded engines or std::*_distribution (ADR-0026; master plan section 4.2).
- ARCH3-R088: State hashing MUST use `det::state_hasher`: SHA-256 (sl::fabric digest) over little-endian fixed-width integers with the width encoded, floats with -0.0 written as +0.0 and every NaN written as 0x7FC00000 or 0x7FF8000000000000, and length-prefixed byte strings (ADR-0026; ADR-0012 pattern).
- ARCH3-R089: Randomness MUST come from `det::rng_stream(seed, stream)` over std::mt19937_64 seeded from SHA-256("slr-rng/1" || seed u64le || stream u64le)[0:8], with engine-defined mappings next_u64, next_below(n) (rejection of the biased tail) and next_unit ((x >> 11) x 2^-53); its state is capturable (ADR-0026; ADR-0021).
- ARCH3-R090: `execution_policy::serial_canonical` MUST run every system job of a frame on one thread in ordered_key order and is the oracle: deterministic parallel results MUST equal it (ADR-0026).
- ARCH3-R091: Snapshot participation MUST go through `det::snapshot_contributor` {name, version, capture, restore, hash} declared in replay.hpp; clocks, rng streams and events register contributors (ADR-0026; master plan, Volume 05, episode 098).
- ARCH3-R092: Gate systems MUST use only + - * / and std::sqrt on floats and integer arithmetic, so libm folding cannot differ between builds (ADR-0026; ADR-0021).
- ARCH3-R093: Volume 05 MUST NOT define the replay format, recording files or orchestration; it leaves the seams of the `seams` table for SL-ARCH-012 (master plan, Volume 17; ADR-0026).

```sl-data
{"determinism": {
"classes": ["hashed", "snapshotted", "transient-diagnostic", "external-input"],
"rows": [
{"state": "entity allocator (indices, generations, free FIFO)", "class": "hashed, snapshotted"},
{"state": "archetypes, chunks, row order", "class": "snapshotted (layout); hashed through component values in iteration order"},
{"state": "components flagged hashed / snapshot", "class": "hashed / snapshotted per reflected field flag"},
{"state": "change ticks (added, changed)", "class": "snapshotted, not hashed"},
{"state": "world resources", "class": "hashed, snapshotted when reflected with the flags"},
{"state": "logical tick, fixed-stepper accumulator, frame index", "class": "hashed (tick), snapshotted (all)"},
{"state": "rng_stream state", "class": "snapshotted (contributor), hashed"},
{"state": "pending events", "class": "snapshotted (contributor), hashed without correlation"},
{"state": "entity_map of the link", "class": "snapshotted, hashed"},
{"state": "observation outbox", "class": "transient (flushed every end_frame); its patch bytes are a determinism output"},
{"state": "job graph, queues, scheduler state, steal order", "class": "transient-diagnostic (never snapshotted)"},
{"state": "correlation ids, log events, frame_stats, scheduler_stats, memory reports", "class": "transient-diagnostic"},
{"state": "wall clock samples, scripted or device input, file reads, background job results, projection revisions, worker count", "class": "external-input (record_sink)"}
],
"ordered_key": [{"field": "tick", "type": "u64"}, {"field": "phase", "type": "u8 (phases.index)"}, {"field": "system_index", "type": "u16"}, {"field": "chunk_index", "type": "u32 (batch index; 0 for single-job systems)"}, {"field": "local_seq", "type": "u32 (per job)"}],
"merge_points": [{"point": "command buffer playback", "where": "ecs/command_buffer.hpp", "wo": "WO-091"}, {"point": "event delivery at a phase boundary", "where": "events.hpp", "wo": "WO-092"}, {"point": "observation outbox per-job buffers", "where": "bridge.hpp", "wo": "WO-097"}, {"point": "structural signals (from flush_report order)", "where": "ecs/change.hpp", "wo": "WO-093"}, {"point": "background job results drained at begin_frame", "where": "jobs.hpp", "wo": "WO-082"}, {"point": "par_for_each reductions in reference systems and the gate scene", "where": "testing/sim_fixture.hpp, tools/demo", "wo": "WO-094, WO-099"}],
"sorted_not_merged": ["leak report (tag, site, size)", "outbox coalesced patch (record id bytes, encoded path)", "load_plan (document, record id)", "registry dumps (name)"]}}
```

### Frame scheduler (episode 090)

- ARCH3-R094: A frame MUST run the `phases` table in order; simulate runs 0..k fixed ticks, each with the sub-phases pre_tick, tick and post_tick, and the end of every phase is a sync boundary (master plan, Volume 05, episode 090).
- ARCH3-R095: At a sync boundary, in order: every job of the phase completes, command buffers play back by ordered_key, the phase's events become readable and change signals are published; nothing else changes world structure (ADR-0025; ADR-0026).
- ARCH3-R096: end_frame MUST be the safe boundary: zero frame jobs in flight, then the owner-thread hook slots in the order of the `boundary_slots` table (master plan section 2.5).
- ARCH3-R097: Phase graphs MUST be built once per schedule change and reused; adding a task after build returns schedule_frozen (master plan, Volume 05, episode 090; ADR-0024).
- ARCH3-R098: The frame index MUST increment at begin_frame, and `retire_after(frames, fn)` (frames >= 1) MUST run fn on the owner thread at the end_frame `frames` frames later (Volume 07 seam; master plan section 2.5).
- ARCH3-R099: present MUST be a no-op hook slot in Volume 05 (Volume 07 seam; ADR-0028).
- ARCH3-R100: `frame_stats` MUST record per-phase job counts, the critical path in jobs and per-phase wall time; wall time is recorded, never asserted (master plan section 6.1; Volume 16 seam).

```sl-data
{"phases": [
{"index": 0, "phase": "begin_frame", "runs": "owner thread: frame index + 1, one clock sample, drain background results and recorded external inputs (record_sink)", "systems": false},
{"index": 1, "phase": "apply_inputs", "runs": "systems that read input events", "systems": true},
{"index": 2, "phase": "pre_tick", "runs": "simulate sub-phase, once per fixed tick (0..max_catchup)", "systems": true},
{"index": 3, "phase": "tick", "runs": "simulate sub-phase, once per fixed tick", "systems": true},
{"index": 4, "phase": "post_tick", "runs": "simulate sub-phase, once per fixed tick; logical tick + 1 after it", "systems": true},
{"index": 5, "phase": "observe", "runs": "link observe jobs and observer systems (read-only on document-bound components)", "systems": true},
{"index": 6, "phase": "present", "runs": "no-op hook slot (Volume 07)", "systems": false},
{"index": 7, "phase": "end_frame", "runs": "safe boundary: zero frame jobs in flight, then the boundary_slots in order", "systems": false}
],
"boundary_slots": ["fault_check", "observation_flush", "projection_rebase", "snapshot", "swap", "lifecycle_requests", "retire_after", "arena_flip", "diagnostics"]}
```

### ECS entity lifecycle and id mapping (episode 091; ADR-0025)

- ARCH3-R101: An entity MUST be 64 bits {index u32, generation u32}; generation starts at 1, the all-zero value is null, and a slot whose generation reaches 0xFFFFFFFF is retired forever (ADR-0025).
- ARCH3-R102: Freed indices MUST be reused in FIFO order of destruction and only while at least 1024 are free, so handle assignment is a pure function of the operation history (ADR-0025; ADR-0026).
- ARCH3-R103: Stale handles MUST be detected: is_alive is false, checked accessors return stale_entity, unchecked accessors SL_DEBUG_ASSERT (ADR-0025; master plan, Volume 05, episode 091).
- ARCH3-R104: The entity table MUST be paged (4096 records per page, pages never move), grow only at sync boundaries and refuse growth beyond max_entities with entity_capacity (ADR-0025).
- ARCH3-R105: `ecs::entity_map<Key>` MUST map an external key (a fabric record_id in the link) to one entity and back, one-to-one (already_exists for a duplicate), iterate in ascending key order and change only at sync points (ADR-0025; ARCH1-R08; master plan section 2.2, row 03).

### Archetype storage (episode 092; ADR-0025)

- ARCH3-R106: Component types MUST be registered before the registry freezes (component_registry_frozen after), MUST be nothrow move constructible and nothrow destructible (static check), at most 2048 bytes and 64-byte alignment (component_invalid), at most 1024 types, and get their component id in registration order (ADR-0025).
- ARCH3-R107: An archetype MUST be the sorted set of at most 32 component ids, created on demand in playback order, kept in creation order and never destroyed in Volume 05, at most 4096 per world (archetype_limit) (ADR-0025).
- ARCH3-R108: A chunk MUST be 16384 bytes aligned to 64 from the chunk pool (tag ecs.chunks): the entity column first, then component columns ordered by (alignment descending, component id ascending), each at a 64-byte aligned offset, plus u32 added and changed tick columns per component; capacity is the largest n <= 1024 whose layout fits (ADR-0025).
- ARCH3-R109: Structural changes (create, destroy, add, remove) MUST happen only on the owner thread outside phases (structural_change_forbidden inside a phase) or through command buffers played back at sync boundaries (ADR-0025).
- ARCH3-R110: A structural change MUST reserve destination space with try_allocate before moving any row, so on out_of_memory the world equals its state before the change (ADR-0025; ADR-0023).
- ARCH3-R111: Iteration order MUST be (archetype creation index, chunk index, row), removal is swap-remove, and empty chunks return to the archetype's chunk pool at the boundary (ADR-0025; ADR-0026).
- ARCH3-R112: A structural call while a query iterates MUST trip the Debug iteration guard (SL_DEBUG_ASSERT) (ADR-0025).
- ARCH3-R113: The component registry layout digest MUST be SHA-256 over (name, size, alignment, field layout) of every component in id order, and it is an input of runtime_abi_digest (ADR-0025; master plan section 2.5).

### ECS queries (episode 093)

- ARCH3-R114: A query MUST declare its terms at construction (read<T>, write<T>, with<T>, without<T>, optional<T>, changed<T>(since), added<T>(since)) and derive its access set from them; contradictory terms are query_invalid (ADR-0025).
- ARCH3-R115: Match caches MUST update incrementally from the world's archetype generation, visiting only archetypes created since the last match (master plan section 6.1, work-bound performance; ADR-0025).
- ARCH3-R116: `par_for_each` MUST split matches into batches of whole chunks of at least 4096 rows, a partition that depends only on the data, keep per-chunk row order and visit each match exactly once whatever the worker count (ADR-0026).
- ARCH3-R117: Iteration through a write<T> term MUST stamp each visited row's changed tick for T (conservative), and changed<T>(since) and added<T>(since) select rows whose tick is later than the explicit since tick (ADR-0025).

### Command buffers (episodes 091 and 092)

- ARCH3-R118: Command buffers MUST be one per job, recorded in frame memory, with spawn returning a `pending_entity`, and destroy, add, remove and set accepting real or pending entities (ADR-0025).
- ARCH3-R119: Playback MUST run on the owner thread at phase boundaries through the world's public structural API in ordered_key order, never completion order, resolving pending entities to real handles in key order (ADR-0026; ADR-0025).
- ARCH3-R120: A command naming a stale entity MUST be skipped and counted, and playback MUST return a `flush_report` listing the applied operations in order (ADR-0025).

### System scheduling contracts (episode 094; master plan section 1.5, row Runtime model)

- ARCH3-R121: A system MUST declare an `access_set`: component reads and writes (from its queries), resources by resource_key (an event channel is a resource), structural (uses command buffers) or exclusive (owner thread, whole world) (master plan section 1.5, row Runtime model).
- ARCH3-R122: Two systems conflict iff one writes what the other reads or writes, or either is exclusive; non-conflicting systems of a phase MAY run in parallel and conflicting ones run in before/after order, then registration order (master plan section 1.5, row Runtime model; ADR-0024).
- ARCH3-R123: system_index MUST be the position of a system in its phase's topological order (Kahn's algorithm, ties by registration order) (ADR-0026).
- ARCH3-R124: A constraint cycle MUST be schedule_cycle naming every system in it, and an unknown before/after name is system_unknown, both at build time (master plan, Volume 05, episode 094).
- ARCH3-R125: In Debug and sanitizer builds an access to an undeclared component, resource or event MUST fail the system with undeclared_access (ADR-0024).
- ARCH3-R126: The schedule MUST export as sl-runtime-schedule/1 JSON, byte-identical across runs and worker counts (master plan, Volume 05, episode 094; Volume 16 seam).

### Change tracking (episode 095)

- ARCH3-R127: Insertion MUST set the added and changed ticks of every component of the row, and `mark_changed<T>` sets the changed tick explicitly (ADR-0025).
- ARCH3-R128: Each system's last-run tick MUST be kept by `ecs::last_run_ticks` so changed<T> and added<T> see each change exactly once per reader (master plan, Volume 05, episode 095).
- ARCH3-R129: `ecs::structural_log` MUST be built from each flush_report and publish archetype_created, entity_created, entity_destroyed, component_added and component_removed signals in playback order at the boundary (ADR-0026).
- ARCH3-R130: The world version MUST increment on every structural change and the archetype generation on every new archetype; caches invalidate on these values only (master plan, Volume 05, episode 095).
- ARCH3-R131: Change ticks MUST be u32 with a boundary sweep every 2^30 ticks that clamps ticks older than 2^31 (ADR-0025).
- ARCH3-R132: Writes made by a link rebase MUST NOT stamp changed ticks (source document) (ADR-0027).

### Runtime reflection (episode 096)

- ARCH3-R133: `type_registry` MUST describe each type: stable name, size, alignment, fields (name, offset, scalar kind, flags transient, snapshot, hashed), methods (name, signature, a noexcept invoker returning sl::result) and an optional Fabric schema type; it freezes with the world (type_registry_frozen after) (master plan, Volume 05, episode 096; ADR-0009).
- ARCH3-R134: Encoding and hashing MUST walk fields in declaration order, little-endian, floats by canonical bit pattern; raw object bytes and padding never reach a hash or a snapshot (ADR-0026).
- ARCH3-R135: A document-bound component MUST have one `fabric_binding` to one record type of the frozen schema_registry with matching field numbers and kinds (component_schema_mismatch otherwise), and a transient type MUST carry no schema type (ARCH1-R62; ADR-0013; ADR-0027).
- ARCH3-R136: The registry MUST dump deterministically (types sorted by name) for inspectors (Volume 06 seam; master plan, Volume 05, episode 096).

### Typed runtime events (episode 097)

- ARCH3-R137: An event type MUST be a reflected struct registered in the event registry, read and written through `event_channel<E>` held as a world resource; no void-pointer or string-keyed payload exists (master plan section 5.1, row Events and input).
- ARCH3-R138: Each event MUST carry an envelope {type id, seq from 1 per channel per frame, emitting system_index, tick, cause (event ref or none), correlation (diagnostic)} (master plan section 3, F6; ARCH1-R46).
- ARCH3-R139: Events written in phase p MUST be merged by ordered_key at p's boundary and readable from then until the end of the following frame (ADR-0026).
- ARCH3-R140: Debug builds MUST validate payloads against reflected field flags and reject invalid ones with event_payload_invalid (master plan, Volume 05, episode 097).
- ARCH3-R141: Each channel MUST hold at most event_capacity events per frame; overflow (RFP7) drops the newest, counts it and logs once per frame, and try_write returns event_overflow (master plan, Volume 05, episode 097; ADR-0022).
- ARCH3-R142: Export to fabric.event envelopes MUST go through the runtime's own `sl::fabric::event_bus` fed by exactly one event_source per runtime (ARCH1-R46; master plan section 3, F6).

### Runtime snapshots and state hash (episode 098)

- ARCH3-R143: A snapshot MUST be taken or restored only at end_frame (snapshot_mid_tick otherwise) and hold the entity allocator, archetypes in creation order with row order and change ticks, snapshot-flagged components, resources, contributors, the schedule digest and runtime_abi_digest, never the job graph (master plan, Volume 05, episode 098; ADR-0026).
- ARCH3-R144: The format MUST be SLRS/1: header {magic SLRS, major 1, minor 0, tick, frame index, runtime_abi_digest, type digests, contributors (name, version), state hash}, field-walked body, SHA-256 trailer; refusals are snapshot_version, snapshot_abi_mismatch, snapshot_schema_mismatch, snapshot_contributor_unknown and snapshot_corrupt, and a refused restore changes nothing (ADR-0026; ADR-0012 pattern).
- ARCH3-R145: Restore MUST reproduce entity handles, iteration order and state hash, and continuing from it MUST equal continuing the original (ADR-0026; master plan, Volume 17 seam).
- ARCH3-R146: `state_hash` MUST be det::state_hasher over the hashed state with a per-archetype and per-component breakdown that localizes a divergence (ADR-0026).
- ARCH3-R147: Saving MUST be atomic through file_access::atomic_write, and a crash after any storage primitive (RFP8) leaves the old or the new snapshot loadable (ADR-0015 pattern; ADR-0026).

### Fabric link: projection in, observations out (ADR-0027)

- ARCH3-R148: The runtime MUST read authoritative data only through a host-supplied `sl::fabric::document_projection` wrapped by `link::document_source`, never a snapshot, store or workspace (ARCH1-R60; ADR-0027; master plan section 2.1).
- ARCH3-R149: The runtime actor MUST be a service actor (denied by default) with host-supplied write:<domain> grants; tests and the gate grant write:content.scene only (ARCH1-R40; ADR-0017; ADR-0027).
- ARCH3-R150: Live state (entities, handles, layouts, ticks, job, scheduler and clock state, event queues, allocator state, snapshots) MUST be transient: never a schema type, an identity or a Fabric record (ARCH1-R08; ARCH1-R62; master plan section 2.2, row 03).
- ARCH3-R151: A component type MUST be document-bound (one fabric_binding, ingested from the projection, observable) or transient (no binding, never observed) (ARCH1-R62; ADR-0027).
- ARCH3-R152: `document_source::load_plan` MUST list records grouped by document ascending, then record id ascending, so ingest and entity creation order are pure functions of the projection (ADR-0027; master plan section 4.2).
- ARCH3-R153: Ingest MUST spawn through a command buffer keyed in load_plan order and record each (record_id, entity) pair in the link's entity_map; records whose type has no binding are skipped and counted (ADR-0027).
- ARCH3-R154: The observe phase MUST turn changed<T>(since last observe) of document-bound components into observed_change values from the binding, drop values equal to the projected value (echo suppression), merge per-job buffers by ordered_key, coalesce to the last value per (record, path) and order by (record id bytes, encoded path) (ADR-0027; ADR-0026).
- ARCH3-R155: At end_frame the outbox MUST become one patch through propose_from_observation with origin {runtime actor, epistemic observed} and go to the injected `link::proposal_sink`; a rejection is counted as link_observation_rejected and never aborts the frame (ADR-0027; ADR-0009; master plan section 2.1).
- ARCH3-R156: The host MUST commit sink patches with commit_policy::rebase_if_commutes and its own durability, and a preview runtime MUST use a sink that never commits (ADR-0027; master plan section 2.3).
- ARCH3-R157: A new projection posted with `world_link::post_projection` (any thread) MUST apply only at end_frame as a diff keyed by (root_digest, generation): created records spawn, changed ones update in place, retired ones despawn, unchanged records keep their handles (ADR-0027; master plan section 2.5).
- ARCH3-R158: The link MUST hold at most two projections (current and pending; a newer post replaces the pending one), release the old one at end_frame, and report their summed body bytes as external tag fabric.projection (ADR-0027; ADR-0023).
- ARCH3-R159: The host MUST stop the runtime before it destroys the workspace, and Debug builds check a host-supplied lifetime token at every boundary (ADR-0027).
- ARCH3-R160: Spawn and despawn MUST NOT be observed back; only fields of live document-bound records are proposed (ADR-0027; ARCH1-R60).
- ARCH3-R161: A runtime workload MUST leave the F5 ledger's bypassing count at 0; the ledger is read only by tests outside engine/runtime (bnd_sink_e2e_test, gate_fabric_test), because R-BND-INCLUDE forbids f5.hpp everywhere under engine/runtime (master plan section 3, F5).

### Concurrency and memory stress (episode 099)

- ARCH3-R162: The `stress_matrix` cells MUST run in the stress area under debug and under runtime-san asan, ubsan and tsan with SL_RUNTIME_STRESS_SCALE, each file within its cap (master plan, Volume 05, episode 099).
- ARCH3-R163: `runtime-san` MUST be fail-closed: a sanitizer runtime that cannot start, or any sanitizer report, is a failure, never `unproven` (ADR-0024).
- ARCH3-R164: Every stress test MUST print its seed and a rerun command on failure, and `just runtime-soak` runs random seeds outside every gate (master plan, Volume 05, episode 099).
- ARCH3-R165: Stress tests MUST assert invariants, never timings; the only time limits are the file cap and the watchdog (master plan section 6.1).
- ARCH3-R166: Steady-frame code MUST report through counters: sl::log events only at boundaries, faults and lifecycle transitions, so a steady frame emits zero log events (master plan, Volume 05, episode 099; ADR-0024).

```sl-data
{"stress_matrix": [
{"cell": "S1", "name": "job DAG fuzz", "workload": "workers {1,2,4,8,16}, 1e4 jobs x 20 iterations, 8 seeds", "test": "tests/integration/runtime/stress_jobs_test.cpp", "cap_s": 20},
{"cell": "S2", "name": "deque linearizability", "workload": "1 owner + {1,3,7,15} thieves, 1e6 items, both queues", "test": "tests/integration/runtime/stress_deque_test.cpp", "cap_s": 20},
{"cell": "S3", "name": "nested fork-join", "workload": "depth 16-64 on {1,2,8} workers", "test": "tests/integration/runtime/stress_jobs_test.cpp", "cap_s": 20},
{"cell": "S4", "name": "allocator churn", "workload": "8 threads x 1e6 operations, sizes 8 B-64 KiB", "test": "tests/integration/runtime/stress_alloc_test.cpp", "cap_s": 20},
{"cell": "S5", "name": "RFP0 sweep", "workload": "every allocation index of a 2000-allocation structural batch", "test": "tests/integration/runtime/stress_alloc_test.cpp", "cap_s": 20},
{"cell": "S6", "name": "ECS churn", "workload": "entities {0,1,1e3,1e5}, 300 ticks, 5% spawn/despawn and 5% add/remove per tick, 8 systems, workers {1,2,4,8,16}", "test": "tests/integration/runtime/stress_ecs_test.cpp", "cap_s": 20},
{"cell": "S7", "name": "determinism matrix and chaos", "workload": "workers {1,2,4,8,16} x {parallel, serial_canonical}, RFP4 stalls over 8 seeds, perturbed correlation, 300 ticks", "test": "tests/integration/runtime/stress_det_matrix_test.cpp", "cap_s": 20},
{"cell": "S8", "name": "lifecycle storm", "workload": "500 random lifecycle operations with RFP1, RFP5 and RFP9", "test": "tests/integration/runtime/stress_lifecycle_test.cpp", "cap_s": 20},
{"cell": "S9", "name": "two runtimes", "workload": "two runtimes concurrently in one process, 300 ticks", "test": "tests/integration/runtime/stress_lifecycle_test.cpp", "cap_s": 20},
{"cell": "S10", "name": "event flood", "workload": "1e6 events per frame against capacity 65536", "test": "tests/integration/runtime/stress_events_test.cpp", "cap_s": 20}
],
"scale": "SL_RUNTIME_STRESS_SCALE: 1.0 debug, 0.1 under ci-asan, ci-ubsan, ci-tsan; counts floor at 1",
"presets": ["debug", "ci-asan", "ci-ubsan", "ci-tsan"],
"ctest": "RUN_SERIAL, RESOURCE_LOCK cpu, label runtime_stress"}
```

### Gate 100: the runtime foundation (master plan section 4.3; ADR-0028)

- ARCH3-R167: Gate 100 MUST be a running product: `sl_runtime_demo` runs the lantern-field scene headless and writes every file of `gate_evidence` under --evidence-dir only (master plan section 4.3; ADR-0028).
- ARCH3-R168: The headless runner MUST accept exactly --seed, --ticks, --workers, --mode serial|parallel, --entities, --snapshot-every and --evidence-dir (default build/runtime/gate) and exit 0 ok, 1 divergence or invariant failure, 2 usage, 3 runtime fault (master plan section 2.3; ADR-0028).
- ARCH3-R169: The scene MUST be: a memory workspace seeded with 256 content.scene records whose types are registered in code through public schema.hpp (no sl_fabric_testing link), 50000 transient lanterns, 8 systems over 4 phases with spawn and despawn churn, events, a change observer, a snapshot every 300 ticks, RFP5 armed at tick 450 with restore recovery, 600 ticks at 60 Hz and observations committed by the host through the sink (master plan section 4.3; ADR-0028).
- ARCH3-R170: The gate MUST prove D1: per-tick hashes equal for workers {1, 2, 4, 8, 16}, serial_canonical versus parallel, resume from snapshot and replay of recorded inputs; the clang-versus-gcc comparison is recorded only (ADR-0026; master plan section 4.3).
- ARCH3-R171: `runtime-scorecard` MUST fail when leaks, sanitizer reports, hash divergences, F5 bypasses or stress failures are non-zero or invariant coverage is below 1.0 (master plan section 3.3).
- ARCH3-R172: `runtime-gate` MUST compose every runtime check and every neg-runtime-* test; `check` runs it after fabric-gate and `check-full` adds runtime-san asan, ubsan and tsan and runtime-determinism (master plan section 4.3; ADR-0028).
- ARCH3-R173: Episodes 081-100 MAY reach Demonstrated on recorded evidence and MUST NOT reach Accepted before episodes 021-040 are Accepted (ADR-0021; master plan section 6.2).

```sl-data
{"gate_evidence": [
{"file": "demo.log", "format": "text", "holds": "run log ending runtime-demo: ok"},
{"file": "demo-report.json", "format": "sl-runtime-gate/1", "holds": "ticks, entities, workers, per-phase stats, scheduler counters, recovery {fault_tick 450, restored_tick 300, hash_equal}, fabric {commits, proposed_changes, rejected}, final state hash"},
{"file": "f5.json", "format": "sl-runtime-f5/1", "holds": "{mutations, bypassing 0}, written by gate_fabric_test (f5.hpp is readable only outside engine/runtime)"},
{"file": "memreport.json", "format": "sl-runtime-memreport/1", "holds": "leaks 0, steady upstream allocations per frame 0"},
{"file": "fault-450.json", "format": "sl-runtime-fault/1", "holds": "the RFP5 fault report"},
{"file": "schedule.json", "format": "sl-runtime-schedule/1", "holds": "the exported schedule"},
{"file": "hashes.json", "format": "sl-runtime-hashes/1", "holds": "per-step state hashes of each run, the input of determinism_check (schemas/runtime/hashes.schema.json; the command-line run's final hash is tests/golden/runtime/demo_hash.json)"},
{"file": "hash-matrix.json", "format": "sl-runtime-hash-matrix/1", "holds": "workers x mode x resume x replay, all equal (D1); clang vs gcc recorded"},
{"file": "stress-matrix.json", "format": "sl-runtime-stress-matrix/1", "holds": "cells S1-S10 x presets, pass, wall seconds, seed"},
{"file": "../san/<asan|ubsan|tsan>-<area|all>.log", "format": "text", "holds": "zero sanitizer report lines (runtime-san; check-full only)"},
{"file": "../scorecard.json", "format": "sl-runtime-scorecard/1", "holds": "build/runtime/scorecard.json"},
{"file": "../../acceptance/13-runtime-gate.log .. 16-check-full.log", "format": "text", "holds": "just acceptance-081-100"}
],
"directory": "build/runtime/gate (the --evidence-dir default)"}
```

### Limits (every number; ADR-0025, ADR-0024, ADR-0023)

- ARCH3-R174: Builders MUST take every size, capacity, timeout and scale from the `limits` table and MUST NOT choose another; a value that does not fit is a deviation (ADR-0028; master plan section 6.3, rule 4).

Every row is bound to a constant or default of the code by `just runtime-contract-lock`, except the rows marked `"bound": false` (formulas, test tiers, CMake values and constants private to one source file), which the lock skips and the code does not export.

```sl-data
{"limits": [
{"name": "chunk_bytes", "value": 16384, "unit": "bytes", "note": "ADR-0025; runtime ABI"},
{"name": "chunk_alignment", "value": 64, "unit": "bytes", "note": "ADR-0025"},
{"name": "column_alignment", "value": 64, "unit": "bytes", "note": "ADR-0025"},
{"name": "max_rows_per_chunk", "value": 1024, "unit": "rows", "note": "ADR-0025"},
{"name": "max_components_per_archetype", "value": 32, "unit": "components", "note": "ADR-0025"},
{"name": "max_component_types", "value": 1024, "unit": "types", "note": "ADR-0025"},
{"name": "max_component_size", "value": 2048, "unit": "bytes", "note": "ADR-0025"},
{"name": "max_component_alignment", "value": 64, "unit": "bytes", "note": "ADR-0025"},
{"name": "max_archetypes", "value": 4096, "unit": "per world", "note": "ADR-0025"},
{"name": "max_query_terms", "value": 32, "unit": "terms", "note": "= max_components_per_archetype"},
{"name": "entity_index_bits", "value": 32, "unit": "bits", "note": "ADR-0025; runtime ABI"},
{"name": "entity_generation_bits", "value": 32, "unit": "bits", "note": "ADR-0025; runtime ABI"},
{"name": "entity_generation_first", "value": 1, "unit": "", "note": "0 is null"},
{"name": "entity_generation_retire", "value": 4294967295, "unit": "", "note": "slot retired"},
{"name": "entity_reuse_fifo_threshold", "value": 1024, "unit": "free indices", "note": "ADR-0025"},
{"name": "entity_page_records", "value": 4096, "unit": "records", "note": "pages never move"},
{"name": "max_entities_default", "value": 4194304, "unit": "entities", "note": "runtime_config.max_entities"},
{"name": "job_capacity_default", "value": 65536, "unit": "records", "note": "runtime_config.job_capacity"},
{"name": "job_capacity_max", "value": 16777216, "unit": "records", "note": "2^24; start refuses more (invalid_argument)"},
{"name": "job_record_bytes", "value": 128, "unit": "bytes, aligned 64", "note": "ADR-0024"},
{"name": "job_record_alignment", "value": 64, "unit": "bytes", "note": "ADR-0024"},
{"name": "job_inline_payload_bytes", "value": 48, "unit": "bytes", "note": "ADR-0024"},
{"name": "job_payload_alignment", "value": 16, "unit": "bytes", "note": "job_fn and task_fn inline storage"},
{"name": "job_max_dependencies", "value": 8, "unit": "per submit", "note": "ADR-0024"},
{"name": "job_nested_wait_depth", "value": 64, "unit": "levels", "note": "tested"},
{"name": "ws_deque_initial_cells", "value": 4096, "unit": "cells", "note": "doubles"},
{"name": "ws_deque_max_cells", "value": 65536, "unit": "cells", "note": "= job_capacity_default"},
{"name": "injection_queue_capacity", "value": "job_capacity", "unit": "per queue", "note": "frame, background and main-affinity locked_deque rings take runtime_config.job_capacity; no constant of its own", "bound": false},
{"name": "injection_poll_every", "value": 61, "unit": "local pops", "note": "starvation bound"},
{"name": "steal_attempts_per_round", "value": "2 x workers", "unit": "attempts", "note": "a formula in policy_steal.cpp (attempt >= 2 x workers skips)", "bound": false},
{"name": "spin_limit", "value": 64, "unit": "iterations", "note": "then park"},
{"name": "steal_spin_limit", "value": 64, "unit": "iterations", "note": "steal_options default"},
{"name": "aging_limit", "value": 64, "unit": "scheduler rounds", "note": "background promotion"},
{"name": "steal_aging_limit", "value": 64, "unit": "scheduler rounds", "note": "steal_options default"},
{"name": "max_workers", "value": 64, "unit": "threads", "note": "runtime_config.workers"},
{"name": "workers_default", "value": "hardware_concurrency - 1 - reserved_threads, at least 1", "unit": "", "note": "a formula in jobs::default_workers()", "bound": false},
{"name": "reserved_threads_default", "value": 0, "unit": "threads", "note": "Volume 07 render submit and I/O"},
{"name": "watchdog_ms", "value": 5000, "unit": "ms", "note": "Debug and sanitizer builds"},
{"name": "watchdog_period_ms", "value": 100, "unit": "ms", "note": "watchdog poll period"},
{"name": "tick_rate_default", "value": "60/1", "unit": "ticks per second", "note": "rational num/den"},
{"name": "max_frame_delta", "value": 250, "unit": "ms", "note": "excess counted in dropped_ns"},
{"name": "max_catchup", "value": 8, "unit": "ticks per frame", "note": "beyond: dropped_ticks"},
{"name": "change_tick_bits", "value": 32, "unit": "bits", "note": "wrap sweep"},
{"name": "change_tick_sweep_every", "value": 1073741824, "unit": "ticks (2^30)", "note": "clamps ticks older than 2^31"},
{"name": "change_tick_max_age", "value": 1073741823, "unit": "ticks (2^30 - 1)", "note": "sweep_ticks clamps older stamps to now - max_age"},
{"name": "last_run_slots", "value": 2048, "unit": "slots", "note": "last_run_ticks: phase x 256 + system index"},
{"name": "frame_arena_block_bytes", "value": 1048576, "unit": "bytes per worker per buffer", "note": "grows only in warm-up"},
{"name": "command_buffer_block_bytes", "value": 16384, "unit": "bytes", "note": "from the frame arena"},
{"name": "command_buffer_block_alignment", "value": 64, "unit": "bytes", "note": ""},
{"name": "warmup_frames", "value": 10, "unit": "frames", "note": "before zero-upstream counting"},
{"name": "event_capacity_default", "value": 65536, "unit": "events per channel per frame", "note": "RFP7"},
{"name": "par_batch_min_rows", "value": 4096, "unit": "rows", "note": "whole chunks"},
{"name": "max_systems_per_phase", "value": 256, "unit": "systems", "note": ""},
{"name": "max_tasks_per_phase", "value": 256, "unit": "tasks", "note": "frame_scheduler"},
{"name": "max_fields_per_type", "value": 64, "unit": "fields", "note": "reflection"},
{"name": "max_method_arity", "value": 4, "unit": "arguments", "note": "reflection"},
{"name": "report_interval_frames", "value": 600, "unit": "frames", "note": "runtime.mem log event"},
{"name": "max_recoveries", "value": 3, "unit": "per window", "note": "restore_snapshot policy"},
{"name": "recovery_window_ticks", "value": 600, "unit": "ticks", "note": ""},
{"name": "health_ticks_default", "value": 60, "unit": "ticks", "note": "swap health check"},
{"name": "max_projections_held", "value": 2, "unit": "projections", "note": "current and pending"},
{"name": "max_alignment", "value": 4096, "unit": "bytes", "note": "ADR-0023"},
{"name": "max_memory_tags", "value": 256, "unit": "tags", "note": "tag registry"},
{"name": "fault_stall_max_us", "value": 2000, "unit": "us", "note": "RFP4 stall upper bound"},
{"name": "pool_slab_bytes", "value": 65536, "unit": "bytes", "note": ""},
{"name": "pool_min_block_bytes", "value": 16, "unit": "bytes", "note": ""},
{"name": "arena_block_bytes_default", "value": 65536, "unit": "bytes", "note": ""},
{"name": "chunk_pool_slab_bytes", "value": 2097152, "unit": "bytes, aligned 4096", "note": "tag ecs.chunks"},
{"name": "chunk_pool_slab_alignment", "value": 4096, "unit": "bytes", "note": ""},
{"name": "debug_fill_fresh", "value": "0xCD", "unit": "byte", "note": ""},
{"name": "debug_fill_freed", "value": "0xDD", "unit": "byte", "note": ""},
{"name": "input_payload_bytes", "value": 48, "unit": "bytes", "note": "platform input event payload"},
{"name": "os_thread_name_bytes", "value": 15, "unit": "bytes", "note": "Linux pthread name limit, private to platform_threads.cpp; thread_handle::name() keeps the full name", "bound": false},
{"name": "error_code_table_version", "value": 1, "unit": "", "note": "error_codes table_version; runtime_abi_digest input"},
{"name": "failpoint_table_version", "value": 1, "unit": "", "note": "failpoints table_version; runtime_abi_digest input"},
{"name": "ordered_key_layout_version", "value": 1, "unit": "", "note": "runtime_abi_digest input"},
{"name": "snapshot_major", "value": 1, "unit": "", "note": "SLRS/1"},
{"name": "snapshot_minor", "value": 0, "unit": "", "note": "SLRS/1"},
{"name": "canonical_nan_f32", "value": "0x7FC00000", "unit": "bits", "note": "ADR-0026; a constant private to reflect_codec.cpp and replay.hpp state_hasher", "bound": false},
{"name": "canonical_nan_f64", "value": "0x7FF8000000000000", "unit": "bits", "note": "ADR-0026; a constant private to reflect_codec.cpp and replay.hpp state_hasher", "bound": false},
{"name": "unit_case_cap", "value": 1, "unit": "s per case", "note": "unit tier; test policy, not code", "bound": false},
{"name": "unit_entry_timeout", "value": 10, "unit": "s per ctest entry", "note": "unit tier; ctest TIMEOUT", "bound": false},
{"name": "integration_entry_timeout", "value": 60, "unit": "s per ctest entry", "note": "integration tier; ctest TIMEOUT", "bound": false},
{"name": "stress_file_cap", "value": 20, "unit": "s Debug at scale 1", "note": "stress area; ctest TIMEOUT", "bound": false},
{"name": "gate_test_cap", "value": 15, "unit": "s Debug at scale 1", "note": "gate area; ctest TIMEOUT", "bound": false},
{"name": "SL_RUNTIME_STRESS_SCALE_default", "value": 1.0, "unit": "factor", "note": "0.1 when SL_SANITIZE is set; iteration counts floor at 1; a CMake environment value", "bound": false},
{"name": "gate_records", "value": 256, "unit": "content.scene records", "note": "gate scene"},
{"name": "gate_lanterns", "value": 50000, "unit": "entities", "note": "gate scene"},
{"name": "gate_systems", "value": 8, "unit": "systems over 4 phases", "note": "gate scene: 7 scene systems (demo/systems.cpp) plus the link's observe system; no constant", "bound": false},
{"name": "gate_ticks", "value": 600, "unit": "ticks at 60 Hz", "note": "gate scene"},
{"name": "gate_snapshot_every", "value": 300, "unit": "ticks", "note": "gate scene"},
{"name": "gate_fault_tick", "value": 450, "unit": "tick", "note": "RFP5"},
{"name": "gate_max_recoveries", "value": 3, "unit": "per window", "note": "gate scene recovery bound"},
{"name": "gate_test_scale", "value": 0.1, "unit": "of the scene", "note": "gate_*_test; scaled(gate_lanterns / 10), no constant", "bound": false}
]}
```

### Thread safety, ownership and immutability

- ARCH3-R175: Every public API MUST keep the class of the `thread_safety` table: owner-thread (the frame thread that created the runtime; wrong_thread or a Debug assertion otherwise), any-thread, or worker-only (inside a job or system body) (ADR-0024; master plan section 5.1, row Job system).
- ARCH3-R176: Object ownership, lifetime and destruction order MUST follow the `ownership` table, and what the `immutability` table lists MUST NOT change after its freeze point (ADR-0023; ADR-0027; master plan section 2.5).

```sl-data
{"thread_safety": [
{"api": "runtime::create, start, run_frame, run_frames, resume, shutdown", "header": "runtime.hpp", "class": "owner-thread", "note": "the creating thread is the frame thread; wrong_thread otherwise"},
{"api": "runtime::request_suspend, request_stop, request_swap", "header": "runtime.hpp", "class": "any-thread", "note": "effective at the next end_frame"},
{"api": "runtime::state, last_fault, memory_report, swap_status", "header": "runtime.hpp", "class": "any-thread", "note": "snapshots of atomics"},
{"api": "install_terminate_report", "header": "runtime.hpp", "class": "owner-thread", "note": "host main only"},
{"api": "platform clock_source::now, wall_clock::now", "header": "platform.hpp", "class": "any-thread", "note": ""},
{"api": "platform thread_factory::spawn", "header": "platform.hpp", "class": "owner-thread", "note": "called by job_system::start"},
{"api": "platform input_source::poll, window::*", "header": "platform.hpp", "class": "owner-thread", "note": ""},
{"api": "platform file_access::*", "header": "platform.hpp", "class": "any-thread", "note": "one call per path at a time; no state of its own, the vfs is thread-safe"},
{"api": "platform record_sink::*", "header": "platform.hpp", "class": "owner-thread", "note": "called in begin_frame"},
{"api": "make_headless_platform, testing::scripted_platform", "header": "headless_platform.hpp", "class": "owner-thread", "note": "construction"},
{"api": "mem::tag_registry::add, reserve_external, freeze", "header": "memory.hpp", "class": "owner-thread", "note": "before freeze"},
{"api": "mem::tag_registry::find, name", "header": "memory.hpp", "class": "any-thread", "note": "after freeze, read-only"},
{"api": "mem::tracking_resource::try_allocate, deallocate", "header": "memory.hpp", "class": "any-thread", "note": "iff its upstream is thread-safe; counters atomic"},
{"api": "arena_resource, stack_resource, pool_resource, frame_arena", "header": "allocators.hpp", "class": "owner-thread", "note": "Debug owner assertion; frame_arena has one buffer pair per worker, each worker-only"},
{"api": "frame_arena::flip", "header": "allocators.hpp", "class": "owner-thread", "note": "end_frame slot arena_flip"},
{"api": "synchronized_resource, general_resource", "header": "allocators.hpp", "class": "any-thread", "note": ""},
{"api": "make_report, report_external, to_json, from_json", "header": "memdiag.hpp", "class": "any-thread", "note": "never blocks allocation; allocator free space is sampled on the owner thread at add and end_frame"},
{"api": "diagnostics add, on_frame_end", "header": "memdiag.hpp", "class": "owner-thread", "note": "add before any other thread reads reports"},
{"api": "leak report at shutdown", "header": "memdiag.hpp", "class": "owner-thread", "note": ""},
{"api": "cpu_fence, job_counter, event_count, counting_semaphore", "header": "sync.hpp", "class": "any-thread", "note": "job_counter::reset only without waiters"},
{"api": "jobs::job_system::start, stop, drain_results", "header": "jobs.hpp", "class": "owner-thread", "note": ""},
{"api": "jobs::job_system::submit, try_submit, wait, status, stats", "header": "jobs.hpp", "class": "any-thread", "note": "main-affinity jobs run on the owner thread"},
{"api": "jobs::job_context::fail, fail_fatal, post_result", "header": "jobs.hpp", "class": "worker-only", "note": "inside a job body"},
{"api": "jobs::work_queue push, pop", "header": "work_queue.hpp", "class": "worker-only", "note": "owner worker of the queue"},
{"api": "jobs::work_queue steal", "header": "work_queue.hpp", "class": "any-thread", "note": ""},
{"api": "jobs::ws_deque push, pop / steal", "header": "ws_deque.hpp", "class": "worker-only", "note": "steal any-thread"},
{"api": "jobs::scheduler_stats read", "header": "scheduler_stats.hpp", "class": "any-thread", "note": "relaxed sums"},
{"api": "time::frame_clock, fixed_stepper, logical clock", "header": "time.hpp", "class": "owner-thread", "note": "jobs read frame_time values"},
{"api": "time::frame_time", "header": "time.hpp", "class": "any-thread", "note": "immutable value"},
{"api": "time::tick_scope", "header": "time.hpp", "class": "any-thread", "note": "per-thread RAII"},
{"api": "det::rng_stream", "header": "replay.hpp", "class": "worker-only", "note": "a value with one owner (one per system or chunk key), never shared"},
{"api": "det::state_hasher, merge_ordered, ordered_buffer", "header": "replay.hpp", "class": "any-thread", "note": "a buffer has one writer job"},
{"api": "det::snapshot_contributor capture, restore", "header": "replay.hpp", "class": "owner-thread", "note": "end_frame only"},
{"api": "frame_scheduler add_task, build, add_boundary_hook, add_phase_end_hook, retire_after, run_frame", "header": "frame.hpp", "class": "owner-thread", "note": "add_task before build; retire_after returns result<void> (wrong_thread elsewhere); hooks added during run_frame are ignored"},
{"api": "frame tasks", "header": "frame.hpp", "class": "worker-only", "note": "run as frame jobs"},
{"api": "reflect type_registry add, freeze", "header": "reflect.hpp", "class": "owner-thread", "note": "before freeze"},
{"api": "reflect find, encode, decode, hash_into, dump", "header": "reflect.hpp", "class": "any-thread", "note": "after freeze; reentrant"},
{"api": "ecs::entity_allocator, entity_map mutation", "header": "ecs/entity.hpp", "class": "owner-thread", "note": "at sync points"},
{"api": "ecs::entity_map lookup, is_alive", "header": "ecs/entity.hpp", "class": "any-thread", "note": "during phases, read-only"},
{"api": "ecs::component_registry add, freeze", "header": "ecs/component.hpp", "class": "owner-thread", "note": "before freeze"},
{"api": "ecs::world create, destroy, add, remove, set (structural)", "header": "ecs/world.hpp", "class": "owner-thread", "note": "outside phases; inside phases use command buffers"},
{"api": "ecs::world get<T>, column access", "header": "ecs/world.hpp", "class": "worker-only", "note": "inside a system with the declared access; owner thread outside phases"},
{"api": "ecs::query for_each, for_each_chunk, par_for_each", "header": "ecs/query.hpp", "class": "worker-only", "note": "inside a system with matching declared access; one query object is never iterated from two threads at once"},
{"api": "ecs::command_queue open", "header": "ecs/command_buffer.hpp", "class": "any-thread", "note": "under the queue mutex; open(base, worker) binds the worker's frame arena"},
{"api": "ecs::command_buffer record", "header": "ecs/command_buffer.hpp", "class": "worker-only", "note": "one buffer per job, single writer"},
{"api": "ecs::command_buffer playback", "header": "ecs/command_buffer.hpp", "class": "owner-thread", "note": "phase boundary"},
{"api": "ecs::system_scheduler add, build, export_json", "header": "ecs/system.hpp", "class": "owner-thread", "note": ""},
{"api": "system bodies", "header": "ecs/system.hpp", "class": "worker-only", "note": "exclusive systems run on the owner thread"},
{"api": "ecs::mark_changed, last_run_ticks", "header": "ecs/change.hpp", "class": "worker-only", "note": "mark_changed needs write access, distinct rows from parallel batches; last_run_ticks owner-thread at boundaries"},
{"api": "ecs::structural_log read", "header": "ecs/change.hpp", "class": "owner-thread", "note": "boundary"},
{"api": "event_registry add, freeze", "header": "events.hpp", "class": "owner-thread", "note": "before freeze"},
{"api": "event_channel<E> write, try_write, read", "header": "events.hpp", "class": "worker-only", "note": "systems that declare the event access"},
{"api": "event export to fabric.event", "header": "events.hpp", "class": "owner-thread", "note": "end_frame"},
{"api": "snapshot take, restore, save, load, state_hash", "header": "snapshot.hpp", "class": "owner-thread", "note": "end_frame only (snapshot_mid_tick otherwise)"},
{"api": "link::world_link post_projection", "header": "bridge.hpp", "class": "any-thread", "note": "applied at the next end_frame"},
{"api": "link::world_link attach, ingest, flush, rebase, stats", "header": "bridge.hpp", "class": "owner-thread", "note": "boundary slots"},
{"api": "link::proposal_sink::submit", "header": "bridge.hpp", "class": "owner-thread", "note": "called by the runtime at end_frame; the host may hand off"},
{"api": "testing::* (faults arming, fabric_host, sim_fixture, stress_harness)", "header": "testing/", "class": "owner-thread", "note": "test thread, before the workload"}
],
"frame_thread": "the owner thread: the thread that called runtime::create; it runs run_frame, every boundary slot, command playback and main-affinity jobs",
"header_root": "engine/runtime/include/sl/runtime/"}
```

```sl-data
{"ownership": [
{"object": "runtime", "owner": "host", "lifetime": "created and destroyed on the owner thread; stopped before the host destroys the workspace", "move_only": true},
{"object": "platform", "owner": "host", "lifetime": "outlives the runtime", "move_only": true},
{"object": "memory root, allocators, tag registry", "owner": "runtime", "lifetime": "destroyed after worlds and jobs, in reverse creation order", "move_only": true},
{"object": "job_system and workers", "owner": "runtime", "lifetime": "joined in shutdown before worlds are destroyed", "move_only": true},
{"object": "frame_scheduler, system_scheduler", "owner": "runtime", "lifetime": "", "move_only": true},
{"object": "worlds (ecs::world)", "owner": "runtime", "lifetime": "", "move_only": true},
{"object": "component, type and event registries", "owner": "runtime", "lifetime": "frozen at start, destroyed with the runtime", "move_only": true},
{"object": "command buffers, frame arenas", "owner": "runtime (per job, per worker)", "lifetime": "frame memory; valid until end of next frame", "move_only": true},
{"object": "snapshots (bytes)", "owner": "the caller of take", "lifetime": "immutable value; the runtime keeps the newest for recovery per policy", "move_only": false},
{"object": "workspace, transaction manager, schema registry", "owner": "host", "lifetime": "outlive the runtime", "move_only": true},
{"object": "document_projection", "owner": "host creates; link holds copies", "lifetime": "at most two held (current, pending); old released at end_frame", "move_only": false},
{"object": "proposal_sink", "owner": "host", "lifetime": "outlives the world_link", "move_only": true},
{"object": "world_link, document_source", "owner": "host composition (gate demo, fabric_host)", "lifetime": "destroyed before the runtime", "move_only": true},
{"object": "fault reports, memory reports, frame_stats", "owner": "caller", "lifetime": "values", "move_only": false}
],
"destruction_order": ["world_link and document_source (host)", "runtime: request_stop, shutdown (jobs drained, workers joined)", "worlds", "schedulers", "job_system", "registries", "allocators and memory root", "platform (host)", "workspace and transaction manager (host)"]}
```

```sl-data
{"immutability": [
{"what": "tag registry", "frozen": "frozen at runtime start", "how": "add returns tag_registry_frozen"},
{"what": "component registry", "frozen": "frozen at runtime start", "how": "component_registry_frozen"},
{"what": "type registry", "frozen": "frozen with the world", "how": "type_registry_frozen"},
{"what": "event registry", "frozen": "frozen at runtime start", "how": "add returns type_registry_frozen"},
{"what": "phase graphs and schedule", "frozen": "frozen at build", "how": "schedule_frozen until a schedule change at a boundary"},
{"what": "document_projection", "frozen": "immutable by type", "how": "Fabric value semantics"},
{"what": "time::frame_time", "frozen": "immutable by type", "how": "a per-frame value handed to jobs"},
{"what": "snapshot bytes", "frozen": "immutable by type", "how": "SLRS/1 with a trailer"},
{"what": "error_codes, failpoints, phases, rt_states tables", "frozen": "frozen by the contract lock", "how": "WO-100 schemas/runtime/contract.lock.json"},
{"what": "restart-class configuration (runtime_abi_digest inputs)", "frozen": "fixed for the life of a runtime", "how": "chunk_bytes, handle layout, component layout digest, errc table version, failpoint table version, ordered_key layout version; a change is restart_required"}
]}
```

### Seams left for later volumes

- ARCH3-R177: Each seam of the `seams` table MUST exist as the named API, owned by the listed work order, and MUST NOT be implemented beyond it in Volume 05 (master plan section 6.3, rule 3; ADR-0026).

```sl-data
{"seams": [
{"id": "V06-1", "volume": "06", "seam": "host-pumped frames: run_frame is the only loop primitive; the editor event loop pumps it", "api": "runtime::run_frame", "wo": "WO-096"},
{"id": "V06-2", "volume": "06", "seam": "main-affinity job queue drained by the frame thread (Wayland main-thread rule)", "api": "jobs::job_desc.affinity main; frame_scheduler", "wo": "WO-089"},
{"id": "V06-3", "volume": "06", "seam": "editor preview: in-process runtime with a no-commit proposal_sink; one job system per runtime, a shared-job-system mode is named, not built", "api": "link::proposal_sink; runtime_config", "wo": "WO-097"},
{"id": "V06-4", "volume": "06", "seam": "input routing (episode 108): input_source recording and scripted replay", "api": "platform input_source, record_sink", "wo": "WO-078"},
{"id": "V06-5", "volume": "06", "seam": "property inspector (episode 113): deterministic type_registry dump", "api": "reflect type_registry::dump", "wo": "WO-087"},
{"id": "V07-1", "volume": "07", "seam": "present phase hook slot", "api": "frame phases.present", "wo": "WO-089"},
{"id": "V07-2", "volume": "07", "seam": "deferred GPU retirement (episode 134): frame index and retire_after(frames, fn)", "api": "frame_scheduler::retire_after", "wo": "WO-089"},
{"id": "V07-3", "volume": "07", "seam": "GPU heap accounting (episode 126): report_external under reserved gpu.*", "api": "memdiag report_external", "wo": "WO-085"},
{"id": "V07-4", "volume": "07", "seam": "gpu_fence beside cpu_fence (episode 129)", "api": "sync cpu_fence", "wo": "WO-080"},
{"id": "V07-5", "volume": "07", "seam": "render-submit and I/O threads outside the job workers", "api": "runtime_config.reserved_threads", "wo": "WO-096"},
{"id": "V16-1", "volume": "16", "seam": "profiler feed: frame_stats, scheduler_stats, memreport JSON, runtime.mem events, schedule export", "api": "frame.hpp, scheduler_stats.hpp, memdiag.hpp, ecs/system.hpp", "wo": "WO-085"},
{"id": "V16-2", "volume": "16", "seam": "wall-time benchmarks: recorded only in docs/episodes/081-100.md", "api": "frame_stats", "wo": "WO-101"},
{"id": "V16-3", "volume": "16", "seam": "global operator new and third-party allocation tracking", "api": "none (untracked)", "wo": "WO-079"},
{"id": "V17-1", "volume": "17", "seam": "determinism classes beyond D1 (episode 321)", "api": "det::determinism_mode", "wo": "WO-084"},
{"id": "V17-2", "volume": "17", "seam": "seeded random streams (episode 322)", "api": "det::rng_stream", "wo": "WO-084"},
{"id": "V17-3", "volume": "17", "seam": "stable iteration order (episode 323)", "api": "ordered_key, iteration order, FIFO reuse", "wo": "WO-088"},
{"id": "V17-4", "volume": "17", "seam": "fixed-step contract (episode 324)", "api": "time fixed_stepper, tick", "wo": "WO-084"},
{"id": "V17-5", "volume": "17", "seam": "input and external-input recording (episodes 325, 336)", "api": "record_sink, external inputs", "wo": "WO-078"},
{"id": "V17-6", "volume": "17", "seam": "event recording and causality (episode 326)", "api": "event envelope cause and seq", "wo": "WO-092"},
{"id": "V17-7", "volume": "17", "seam": "state hashing and divergence localization (episodes 327, 331)", "api": "state_hash breakdown", "wo": "WO-095"},
{"id": "V17-8", "volume": "17", "seam": "snapshot serialization, resume, schema-aware restore and deltas (episodes 328-330)", "api": "SLRS/1, snapshot_contributor", "wo": "WO-095"},
{"id": "V17-9", "volume": "17", "seam": "cross-compiler determinism (SL-ARCH-012, not designed here)", "api": "hash-matrix.json records clang vs gcc", "wo": "WO-099"},
{"id": "V19-1", "volume": "19", "seam": "hot-reload boundary (episodes 367-371): AwaitBoundary, Swap, HealthCheck, RolledBack", "api": "runtime::request_swap, swap classes", "wo": "WO-096"},
{"id": "V19-2", "volume": "19", "seam": "ABI fingerprint deciding AwaitBoundary vs RestartRequired", "api": "runtime_abi_digest", "wo": "WO-096"},
{"id": "V19-3", "volume": "19", "seam": "isolated dev runtime process (master plan section 2.3)", "api": "runtime_config.process_mode = in_process only", "wo": "WO-096"},
{"id": "V02-1", "volume": "02", "seam": "vector, transform and geometry component payloads (ADR-0021)", "api": "opaque payload components", "wo": "WO-088"},
{"id": "V26-1", "volume": "26", "seam": "Windows and macOS platforms (ADR-0006)", "api": "platform interfaces", "wo": "WO-078"}
]}
```

### Code reconciliation (WO-100)

What the Volume 05 code does where version 0.1.0 was silent or said otherwise, from the 312 entries of docs/arch/deviations/vol05/WO-076.md to WO-099.md; each item is descriptive and the rules above stand. Items the code still has to change are open issues below, each naming its order or volume.

- Errors and failpoints (WO-076): the runtime `to_string(errc)` cases live in sl/core/error.hpp (constexpr), grouped by `// runtime <area>` comments; `is_runtime_code` is false for reserved numbers; `make_error` has a `const char *` overload so unqualified calls are not ambiguous by ADL; the table rows are `error_code_info {name, number, area}` and `failpoint_info {id, name, site}` (no outcome column) with `error_code_table_version`, `failpoint_table_version` and `failpoint_count = 11`. The registry is a function-local `static constinit` array in src/faults.cpp (one 64-byte slot per point, allow-listed for R-RT-GLOBAL); `testing::fault_arming` is the only friend of `detail::fault_registry_hooks`; `arm(point, at_visit, sticky, param)` counts visits from the arm call, re-arming replaces, `disarm(point)` and `scoped_arm::status()` exist; SL_RUNTIME_FAULTS is always defined (1 in Debug and every sanitizer build); RT-INV-ERR-03's golden also forbids new numbers below 6000; runtime-san runs ctest with -V into build/runtime/san/<san>-<area|all>.log.
- Guardrails (WO-077): tools/runtime is a Python package (WO-103); lint_allow.json excuses the whole of src/faults.cpp for R-RT-GLOBAL; a malformed allow-list is R-RT-ALLOW; R-RT-LAYER checks only headers in the `layers` table; R-RT-FABRIC, R-RT-MATH, R-RT-GLOBAL, R-RT-ORDER, R-RT-DIST, R-RT-PLATFORM, R-RT-NOEXCEPT and R-RT-OUTPUT are text scanners with the scopes documented in tools/runtime/runtime_lint.py; contracts_check also reads the work-order files (R-RT-WO); the LINT rows keep their verified_by recipes while their tests run under `just tools-test`; the runtime-tidy area map names every engine/runtime/testing source, checked by tools/runtime/test_runtime_tidy_areas.py (WO-109).
- Platform (WO-078): `thread_group` releases newest first; threads are created with pthread_create behind a pimpl, named `slrt<instance>-<name>` with the OS name cut to its first 15 bytes (`os_thread_name_bytes`); the interfaces share `platform_interface` and the headless types are public in headless_platform.hpp; `file_access` takes string_view paths relative to `root()`, maps every vfs code except not_found to io_error and is any-thread with one call per path at a time; `window` is headless(), width(), height(); the recording helpers and `content_digest` live in platform.hpp; `input_payload_bytes = 48`; `struct platform` stays an aggregate of references under scoped NOLINT and cppcheck suppressions (WO-104); atomic_write visits no failpoint; the RFP1 branch is compiled only with SL_RUNTIME_FAULTS.
- Memory (WO-079): `tracking_resource` takes a backing resource and `set_upstream` sets a separate pmr fallback, whose blocks are remembered; the pmr path is the public `forward_allocate` and the `do_*` overrides are final; every tracked block carries a 16-byte header {size, tag, log2 align, magic}; `try_allocate_from` and `release_to` give nothrow allocation from any upstream; tag ids are 1..256 (0 untagged), checked against the registry; tag_registry checks frozen, grammar, reserved, duplicate, capacity in that order; tracking_resource fills 0xCD and 0xDD but leaves poisoning to recycling resources; the leak report is sorted by (tag label, file, line, size); failing_resource injects without visiting RFP0; `set_upstream` names its parameter `fallback` (WO-109).
- Synchronization (WO-080): `spin_limit = 64` is a named constant of sync.hpp; a spin step is `std::this_thread::yield()`; every atomic of the primitives is seq_cst; `cpu_fence(0)` is released at construction and over-signals clamp; `event_count` always advances its epoch; `job_counter::waiters()` exists; the sync area visits no failpoint.
- Allocators (WO-081): arena, stack, pool and frame_arena derive `owner_thread_resource` (a Debug owner check that synchronized_resource lifts); stack_resource enforces LIFO only inside marker scopes and defers out-of-order releases outside them; the allocators visit no failpoint and pass upstream errors through; blocks, entries and strides are as documented in allocators.hpp; the four size constants carry the `limits` names; the churn test holds blocks in a fixed array (WO-108).
- Jobs (WO-082): background results are `jobs::background_result` delivered after every earlier background job completes; `current_job_priority` is a member found by thread id; `fatal_report` and `job_config::on_fatal` carry fatal failures; RFP2 fails before the body, RFP3 through on_fatal, RFP4 stalls min(param, 2000) us; every aging_limit-th round polls the background queue first; the frame, background and main-affinity queues are locked_deque rings of job_capacity cells and only the per-worker queues come from make_queue; wakeups notify_all; counter and fence dependencies are polled gates; only workers and the owner help in wait; an inline run at capacity and a failed `submit` return the null handle; a stale predecessor counts as satisfied; records are 128 bytes with a cold side array; names are pointer plus 16-bit length; workers are 0..64 and capacity 1..2^24 (`job_capacity_max`); job_fn checks by static_assert and its ops table uses named static functions (WO-107); its implicit constructor carries a scoped cppcheck noExplicitConstructor suppression (WO-109).
- Entities (WO-083): `entity::bits()` puts the index in the high word; a page is 33,280 bytes (generations, intrusive FIFO links, alive bits) and the page table is sized once; pages carry the resource's default tag; RFP10 param 1 fast-forwards to 0xFFFFFFFE, any other param forces entity_capacity; the allocator has no owner-thread check (phases guarantee it); capture and restore validate before changing anything; stale_entity comes from the world's checked accessors and `entity_allocator::check` and `destroy`, while `entity_map` answers not_found and never stale_entity; `assume_alive` is an NDEBUG-gated SL_DEBUG_ASSERT.
- Clocks and determinism (WO-084): `frame_clock::frame_index()` counts samples; every clock has `state` capture and restore; `frame_time` is {frame_index, now, delta, a {num, den}} with plain members; the stepper reduces the rate by gcd and uses a two-word product; a non-monotonic sample keeps the highest baseline, freeze and thaw return 0; tick_scope asserts the `runtime/` prefix; state_hasher tags str 0x31 and bool 0x40; merge_ordered takes a span of buffer pointers and skips nulls; rng_stream restore parses into a copy and refuses malformed text with snapshot_corrupt; the runtime.clock and runtime.rng contributor layouts are documented in replay.hpp; the goldens come from an independent stdlib script.
- Diagnostics (WO-085): `report_external` returns result<void> and replaces the value; allocator free space is sampled on the owner thread at add and on_frame_end (the WO-081 owner-check item is closed); reports list every registered tag sorted by name and stay monotone under concurrency; leaks are taken at shutdown only, site `<file>:<line>`; `upstream_allocs_last_frame` is the tracked-allocation delta; the runtime.mem event fires every report_interval frames (0 means 600) and the shutdown event is the runtime's (WO-096); to_json writes the layout of schemas/runtime/examples/memreport.json and from_json is strict.
- Work stealing (WO-086): `make_ws_queue` rounds up to a power of two capped at 65536 cells, starting at 4096; `ws_deque::create`, `cells()` and `retired_rings()` exist; the owner worker is the first thread that pushes or pops (Debug check); retired rings live in a 17-slot in-object array freed at quiesce with no reader; only push's cell store (relaxed) and bottom store (release) are weaker than seq_cst; slot seeds are splitmix64(seed ^ job seed ^ i); the owner thread has its own policy slot; injection polls once per non-zero multiple of 61; jobs_run reads 0 from the stealing policy; make_steal_policy refuses 0 for injection_poll_every and aging_limit. Since WO-096 the runtime defaults to `queue_kind::work_stealing` (ws_deque with the stealing policy) while `job_config` alone still defaults to locked_deque (R070 and R073 describe the WO-082 starting point; WO-106 fixed the stale make_ws_queue statements).
- Reflection (WO-087): a type is transient when it binds no field; add copies every string and checks the layout; the registry's owner thread adds and freezes, reads are any-thread once frozen; `type_registry::method` and `type_registry::field` are static lookups (type_unknown, field_unknown); the encoding has no header and walks fields selected by flag in declaration order; bind needs a frozen registry and a matching struct record type; every function of reflect.hpp is noexcept and an allocation failure terminates.
- Storage (WO-088): `component_info` carries a type key and the registry maps types to ids; add_raw refuses in the order frozen, empty name, size, alignment, null hooks, the 1024-type limit, then duplicates; `world::set_job_system` enables the background-job assertion; the world's owner is the thread that called make, and the guard order is owner, iteration guard, phase; `set` adds a missing component (structural) and document writes stamp changed 0; chunks use tag ecs.chunks when registered; every world allocation goes through try_allocate; empty chunks stay until `release_empty_chunks()`; `world_stats::last_rows_moved` counts moved rows.
- Frame scheduler (WO-089): every frame type is in `sl::runtime`; `retire_after` returns result<void> (wrong_thread, invalid_argument for 0 frames or an empty fn); task_fn is a non-consuming 48-byte inline body; task names are copied; tasks go only to apply_inputs through observe; a second build is ok; hooks added during run_frame are ignored; `boundary_context` is {slot, frame_index, tick} and each slot runs the scheduler's action, then its hooks; phase end hooks run after every phase but end_frame; `wall` is two platform clock reads per phase; after warmup_frames a non-zero upstream delta logs a warning; background results become frame inputs keyed at begin_frame; a failed task does not fail run_frame (the runtime raises the fault from the system scheduler's failures, WO-096); the runtime visits RFP9 in every boundary slot (WO-096); `set_frame_faulted` marks the running frame faulted for the event hooks and `rewind_logical` serves a snapshot restore (WO-109).
- Queries (WO-090): read, write and optional pass arguments, with, without, changed and added are filters; reads include the change filters' tick columns; contradictory terms beyond the brief are query_invalid and an unregistered type component_unknown; change filters combine with AND and read ticks before write terms stamp; for_each_chunk passes runs of selected rows; the match cache holds creation indices reserved for max_archetypes; `max_query_terms = 32`; par_for_each batches close at par_batch_min_rows counting every row of a chunk; one query object is never iterated from two threads at once; bodies are nothrow by static_assert; the query area visits no failpoint.
- Command buffers (WO-091, WO-094): `pending_entity {buffer, seq}` holds the buffer's open index within the queue, so the pending values depend on which worker opened first, while the entities they resolve to are the same for every worker count because playback and resolution run in ordered_key order (R118 and R119 hold for resolution; a pending handle is not itself a deterministic value). open is any-thread under a mutex and binds the worker's frame arena; blocks are 16384 bytes aligned 64 (`command_buffer_block_alignment`); recording failures are sticky; playback skips stale targets and reports `skipped_stale` and `skipped_refused`; flush destroys every payload; a set that adds is reported as `set` with `op::added` true (WO-109); structural systems record on their worker's buffer and par_for_each batches get none.
- Events (WO-092): `writer_key` is the ordered_key without local_seq and the channel numbers events per writer; writer buffers live in the channel under a mutex and are cleared, never erased; capacity is enforced per writer and again at the merge; `dropped()` counts every refused event; one overflow warning per frame at end_frame; RFP7 is visited by write and try_write; deliver runs as a phase end hook and the older list is dropped in arena_flip; from the phase end at which the runtime marks the frame faulted (a fatal report, a failed system, a playback out_of_memory) deliver is skipped and arena_flip keeps the older list and drops the undelivered writes, while a restore unmarks the frame so the restored lists flip as the snapshot frame's (WO-109); `read_refs` gives event refs; `channel_key` uses the reserved base 0x45560000; Debug validation refuses non-finite floats; the runtime.events contributor writes both lists; export goes through one publish_batch per frame.
- Change tracking (WO-093, WO-094): `tick_after` is the query filter rule; `mark_changed` returns result<void> and finds the row by scanning chunks; `last_run_ticks` has 2048 slots keyed phase x 256 + index with a floor of 0xFFFFFFFF; exactly-once needs the change tick to advance between conflicting runs, which the system scheduler guarantees by writing at change_tick + 1 + system_index and keeping each system's last run in `last_run_ticks` under phase x 256 + index, so a first run reads the floor (WO-109); the structural log emits archetype_created first, then one signal per applied op, and a set that adds a component gives component_added (WO-109); the sweep clamps stamps older than `change_tick_max_age` (2^30 - 1) every 2^30 ticks, which also covers a document stamp of 0.
- Systems (WO-094): the scheduler is built on a job system and binds frame scheduler, world and queue at build; `system_desc::queries` lists the queries a body runs and add refuses one the access_set does not cover; access sets are normalised; names are unique across phases; Kahn's algorithm uses a min-heap on registration index and a cycle names only its members; edges join only conflicting systems, transitively reduced; exclusive systems run through an owner job; serial_canonical is one inline chain; the scheduler marks the world phase-active through post_tick; RFP5 fails a system job after its body (the visit moved there from before the body in WO-109); the Debug access guard fails a system once with undeclared_access.
- Snapshots (WO-095): restore rebuilds values from zero bytes and the encoded snapshot fields, so snapshotted components are trivially relocatable; component-typed resources are snapshotted and runtime-owned resources stay in the target world; RFP8 is one visit per save before atomic_write; the type digest covers name, size, alignment, relocatability and the reflected fields, and the trailer is checked first; contributors are sorted by name, checked, backed up and restored before the world, and rolled back when `world::restore_from` refuses; invalid_argument from restore_from becomes snapshot_corrupt; the runtime computes runtime_abi_digest (WO-096), whose layout digest walks each component's reflected fields (name, offset, size, kind; WO-109).
- Runtime (WO-096): `runtime_config::queue` defaults to work_stealing; systems and contributors are staged in Created and handed over at start; start runs platform check, memory root, tag freeze, registry freezes, jobs, frames and the world, unwinding in reverse; the first recovery snapshot is the baseline at the end of the first host frame, so a fault in that frame finds none and ends in recovery_failed; a recovery point holds the snapshot and the systems' last runs, and a restore (recovery or swap rollback) rewinds the logical tick and the last runs while wall time goes on (WO-109); restore and shutdown discard undelivered background results (`discarded_results()`, no epoch counter); a faulted frame clears the command queue after a fatal report; system_fail names `<name>#<index>`; `request_swap` stays void with `swap_status()` and `swap_code()`; requests take effect in the lifecycle_requests slot; the runtime emits the shutdown runtime.mem event; `next_state` and `rt_event` exist, and the `rt_states` table equals `next_state` (bound by the lock).
- Link (WO-097): `post_projection` returns result<void> (projection_stale, invalid_argument); `link_config::projection_tag` is reserved external and attach checks it and the lifetime token; link.observe reads every bound component and observe_component selects; bound components are trivially copyable of their reflected size, at most 2048 bytes; spawn values start from zero bytes and rebase keeps runtime-only bytes; ingest spawns through one command buffer in load_plan order; the outbox orders by record id, then path_segment ordering; body bytes are a payload measure; records of no document come first; a faulted frame clears the outbox.
- Stress (WO-098): S1 runs 20 DAG iterations over 8 seeds on the ws queue; S4 checks first and last bytes; S5 sweeps RFP0 over a 2000-operation batch; S6 pre-warms frame arenas, and sim_fixture warms every worker arena of both sets with 1 MiB blocks before frame 0 (WO-109); S7 runs 20 ticks per cell; the watchdog's spawn visits RFP1, so tests arm RFP1 inside the watched body; checks run on the test thread; RFP0 is armed at the memory root or around world operations only; stress-matrix.json is written by stress_report.py, one cell per S row.
- Gate (WO-099): `sl_runtime_demo` composes the runtime's public pieces with the link itself (runtime::start exposes no attach point) and recovers as runtime_recovery.cpp does; frame arenas are warmed before the first frame; the CLI cannot arm failpoints, so the runner plants a scripted fail through lantern.decay at tick 450 and gate_recovery_test arms the real RFP5; hashes are world state hashes indexed by applied steps, written to hashes.json (schemas/runtime/hashes.schema.json, and the command-line run's final hash is tests/golden/runtime/demo_hash.json, WO-109); the demo keeps its own recovery and moves the change tick above every system's last run after a restore (the runtime's restore rewinds instead, WO-109); `wall_ns` is 0 under the manual clock; the scene has 7 systems plus the link's observe system; the 5 x 2 matrix runs the gcc-release demo and one Debug run is the cross-compiler record.
- Contract lock and scorecard (WO-100): `sl_runtime_dump_contract` prints the contract to stdout (R-RT-PLATFORM keeps file output out of engine/runtime) and runtime-contract-lock.sh writes it to build/runtime/contract-dump.json; contract_lock.py adds the sections the Python tools own (stress_matrix from stress_report.py, the hash-matrix, stress-matrix and scorecard formats, the header files for `layers`); rt_states compare as (from, event, to) triples, failpoints by id, name and site, gate_evidence by its non-text rows; the scorecard treats sanitizer logs and hash-matrix.json as optional (`not_run`) and the demo report, memory report, F5 ledger and stress matrix as required; runtime-scorecard.sh reruns runtime_gate_fabric_test in Debug so f5.json comes from a build with the F5 ledger.

### Invariants

Every invariant has exactly one owning work order and one test file; a C++ test case name, or a Python test
method in `test_rt_inv_<area>_<nn>_` form, starts with the id. `just runtime-contracts` (WO-077) binds both ways and
is pending-tolerant; `just runtime-contracts-strict` (WO-100) also requires every test to exist and pass. A work
order owns at most 8 rows; heavy sweeps live in tests/integration and scale with SL_RUNTIME_STRESS_SCALE.

```sl-data
{"invariants": [
{"id": "RT-INV-ERR-01", "rule": "ARCH3-R017", "text": "runtime errc values are unique, lie in 6000-6999, and to_string round trips every name of the error_codes table", "test": "tests/unit/runtime/err_codes_test.cpp", "wo": "WO-076", "verified_by": "just runtime-test err"},
{"id": "RT-INV-ERR-02", "rule": "ARCH3-R017", "text": "domain() of every runtime code is error_domain::runtime, to_string(error_domain::runtime) is \"runtime\" and a code in 7000-7999 stays unknown", "test": "tests/unit/runtime/err_domain_test.cpp", "wo": "WO-076", "verified_by": "just runtime-test err"},
{"id": "RT-INV-ERR-03", "rule": "ARCH3-R017", "text": "every errc number below 6000 equals the committed list of the pre-Volume-05 enumerators (no renumbering)", "test": "tests/unit/runtime/err_stability_test.cpp", "wo": "WO-076", "verified_by": "just runtime-test err"},
{"id": "RT-INV-FLT-01", "rule": "ARCH3-R020", "text": "an armed failpoint trips exactly at the Nth visit, counts every visit from 8 threads, fires once unless sticky, and an unarmed point returns proceed", "test": "tests/unit/runtime/flt_registry_test.cpp", "wo": "WO-076", "verified_by": "just runtime-test flt"},
{"id": "RT-INV-FLT-02", "rule": "ARCH3-R020", "text": "failpoint_table() lists RFP0-RFP10 with the names and sites of the failpoints table, in order", "test": "tests/unit/runtime/flt_table_test.cpp", "wo": "WO-076", "verified_by": "just runtime-test flt"},
{"id": "RT-INV-LINT-01", "rule": "ARCH3-R009", "text": "runtime_lint rejects one planted defect per rule (R-RT-LAYER, FABRIC, TXCALL, MATH, GLOBAL, FENCE, ORDER, DIST, PLATFORM, THROW, NOEXCEPT, LINK, OUTPUT) naming rule and file:line, and the clean tree passes", "test": "tools/runtime/test_runtime_lint.py", "wo": "WO-077", "verified_by": "just runtime-lint"},
{"id": "RT-INV-LINT-02", "rule": "ARCH3-R005", "text": "the fabric allow-lists are scoped: workspace.hpp is accepted under testing/ and tools/ and refused under include/ and src/, vfs.hpp only in src/platform_files*.cpp", "test": "tools/runtime/test_runtime_lint.py", "wo": "WO-077", "verified_by": "just runtime-lint"},
{"id": "RT-INV-LINT-03", "rule": "ARCH3-R014", "text": "contracts_check binds every RT-INV id to one test file and one existing work order both ways; an orphan test, duplicate id, unknown order or wrong location is refused naming rule and id", "test": "tools/runtime/test_contracts_check.py", "wo": "WO-077", "verified_by": "just runtime-contracts"},
{"id": "RT-INV-LINT-04", "rule": "ARCH3-R014", "text": "a missing test or a dispatch stub script is pending by default and an error under runtime-contracts-strict", "test": "tools/runtime/test_contracts_check.py", "wo": "WO-077", "verified_by": "just runtime-contracts"},
{"id": "RT-INV-PLAT-01", "rule": "ARCH3-R036", "text": "headless_platform runs start, 100 frames and stop with no display, window or input device; the window reports headless", "test": "tests/unit/runtime/plat_headless_test.cpp", "wo": "WO-078", "verified_by": "just runtime-test plat"},
{"id": "RT-INV-PLAT-02", "rule": "ARCH3-R036", "text": "a run driven by scripted_platform's manual clock and scripted input is bit-identical across two executions", "test": "tests/unit/runtime/plat_scripted_test.cpp", "wo": "WO-078", "verified_by": "just runtime-test plat"},
{"id": "RT-INV-PLAT-03", "rule": "ARCH3-R079", "text": "clock_source never goes backward across 1e5 samples on each of 8 threads (scaled)", "test": "tests/unit/runtime/plat_clock_test.cpp", "wo": "WO-078", "verified_by": "just runtime-test plat"},
{"id": "RT-INV-PLAT-04", "rule": "ARCH3-R037", "text": "thread_factory names threads slrt<instance>-w<k>; a spawn failing under RFP1 is thread_spawn_failed with no thread left; handles release in reverse acquisition order", "test": "tests/unit/runtime/plat_thread_test.cpp", "wo": "WO-078", "verified_by": "just runtime-test plat"},
{"id": "RT-INV-PLAT-05", "rule": "ARCH3-R039", "text": "every external input passes record_sink exactly once, in arrival order, stamped with the consuming frame index and tick", "test": "tests/unit/runtime/plat_record_test.cpp", "wo": "WO-078", "verified_by": "just runtime-test plat"},
{"id": "RT-INV-PLAT-06", "rule": "ARCH3-R038", "text": "file_access over the faulty vfs returns io_error for an injected fault and atomic_write leaves the old or the new content after a crash at each primitive", "test": "tests/unit/runtime/plat_files_test.cpp", "wo": "WO-078", "verified_by": "just runtime-test plat"},
{"id": "RT-INV-MEM-01", "rule": "ARCH3-R043", "text": "every resource honours each power-of-two alignment up to 4096, refuses others with bad_alignment and serves a zero-byte request", "test": "tests/unit/runtime/mem_alignment_test.cpp", "wo": "WO-079", "verified_by": "just runtime-test mem"},
{"id": "RT-INV-MEM-02", "rule": "ARCH3-R045", "text": "allocated minus freed equals live bytes per tag and overall after 1e4 seeded operations", "test": "tests/unit/runtime/mem_tracking_test.cpp", "wo": "WO-079", "verified_by": "just runtime-test mem"},
{"id": "RT-INV-MEM-03", "rule": "ARCH3-R044", "text": "a resource destroyed with live blocks reports each block's tag, size and (Debug) site", "test": "tests/unit/runtime/mem_leak_report_test.cpp", "wo": "WO-079", "verified_by": "just runtime-test mem"},
{"id": "RT-INV-MEM-04", "rule": "ARCH3-R042", "text": "try_allocate under RFP0 is out_of_memory with counters unchanged and never throws; do_allocate forwards to the upstream and, without one, invokes the captured assert handler with the tag report", "test": "tests/unit/runtime/mem_failure_test.cpp", "wo": "WO-079", "verified_by": "just runtime-test mem"},
{"id": "RT-INV-MEM-05", "rule": "ARCH3-R045", "text": "add after freeze is tag_registry_frozen, gpu.* or fabric.* through add is tag_reserved and through reserve_external succeeds, a duplicate name is already_exists", "test": "tests/unit/runtime/mem_tags_test.cpp", "wo": "WO-079", "verified_by": "just runtime-test mem"},
{"id": "RT-INV-MEM-06", "rule": "ARCH3-R046", "text": "Debug fill patterns 0xCD and 0xDD are present on fresh and freed blocks, and under ASan freed and reset ranges are poisoned (skips with a message elsewhere)", "test": "tests/unit/runtime/mem_fill_poison_test.cpp", "wo": "WO-079", "verified_by": "just runtime-test mem"},
{"id": "RT-INV-SYNC-01", "rule": "ARCH3-R059", "text": "a cpu_fence signalled from N threads releases all M waiters exactly once and a later signal has no effect", "test": "tests/unit/runtime/sync_fence_test.cpp", "wo": "WO-080", "verified_by": "just runtime-test sync"},
{"id": "RT-INV-SYNC-02", "rule": "ARCH3-R059", "text": "resetting a job_counter with a waiter is invalid_state and without one succeeds", "test": "tests/unit/runtime/sync_counter_test.cpp", "wo": "WO-080", "verified_by": "just runtime-test sync"},
{"id": "RT-INV-SYNC-03", "rule": "ARCH3-R057", "text": "the counting semaphore never admits more holders than its count under 16 threads", "test": "tests/unit/runtime/sync_semaphore_test.cpp", "wo": "WO-080", "verified_by": "just runtime-test sync"},
{"id": "RT-INV-SYNC-04", "rule": "ARCH3-R060", "text": "event_count loses no wakeup across 1e6 seeded prepare, notify and wait interleavings (scaled), every wait bounded at 5 s", "test": "tests/integration/runtime/sync_eventcount_test.cpp", "wo": "WO-080", "verified_by": "just runtime-test sync"},
{"id": "RT-INV-SYNC-05", "rule": "ARCH3-R058", "text": "publication through each primitive makes all prior relaxed payload writes visible (TSan clean) and a wait spins at most spin_limit times before parking", "test": "tests/integration/runtime/sync_publish_test.cpp", "wo": "WO-080", "verified_by": "just runtime-test sync"},
{"id": "RT-INV-ALLOC-01", "rule": "ARCH3-R048", "text": "the allocator contract suite passes on all six resources", "test": "tests/unit/runtime/alloc_contract_test.cpp", "wo": "WO-081", "verified_by": "just runtime-test alloc"},
{"id": "RT-INV-ALLOC-02", "rule": "ARCH3-R048", "text": "arena rewind to a marker frees exactly the later allocations, reset returns usage to 0, and an out-of-order stack free trips the captured assertion", "test": "tests/unit/runtime/alloc_arena_stack_test.cpp", "wo": "WO-081", "verified_by": "just runtime-test alloc"},
{"id": "RT-INV-ALLOC-03", "rule": "ARCH3-R048", "text": "pool blocks never overlap and every freed block is on the free list exactly once over 1e5 seeded operations", "test": "tests/unit/runtime/alloc_pool_test.cpp", "wo": "WO-081", "verified_by": "just runtime-test alloc"},
{"id": "RT-INV-ALLOC-04", "rule": "ARCH3-R050", "text": "frame-N memory survives the end_frame of N and is reset, filled 0xDD (Debug) and poisoned (ASan) by the flip that ends N+1", "test": "tests/unit/runtime/alloc_frame_test.cpp", "wo": "WO-081", "verified_by": "just runtime-test alloc"},
{"id": "RT-INV-ALLOC-05", "rule": "ARCH3-R049", "text": "synchronized_resource under 8 threads of seeded churn balances its tracking and is TSan clean", "test": "tests/integration/runtime/alloc_sync_churn_test.cpp", "wo": "WO-081", "verified_by": "just runtime-test alloc"},
{"id": "RT-INV-ALLOC-06", "rule": "ARCH3-R049", "text": "an owner-thread resource used from another thread trips the captured owner assertion", "test": "tests/unit/runtime/alloc_owner_test.cpp", "wo": "WO-081", "verified_by": "just runtime-test alloc"},
{"id": "RT-INV-JOB-01", "rule": "ARCH3-R061", "text": "every job of seeded random DAGs (1e4 jobs, fan-in up to 8, scaled) runs exactly once and after all of its dependencies", "test": "tests/unit/runtime/job_dag_test.cpp", "wo": "WO-082", "verified_by": "just runtime-test job"},
{"id": "RT-INV-JOB-02", "rule": "ARCH3-R064", "text": "fork-join nested to depth 64 completes on 1 worker and on 8", "test": "tests/unit/runtime/job_nested_wait_test.cpp", "wo": "WO-082", "verified_by": "just runtime-test job"},
{"id": "RT-INV-JOB-03", "rule": "ARCH3-R065", "text": "submit at capacity helps and progresses, runs inline only jobs without predecessors, try_submit is job_capacity, status of a stale handle is stale_job and wait on it is ok", "test": "tests/unit/runtime/job_capacity_test.cpp", "wo": "WO-082", "verified_by": "just runtime-test job"},
{"id": "RT-INV-JOB-04", "rule": "ARCH3-R066", "text": "main-affinity jobs run on the owner thread only", "test": "tests/unit/runtime/job_affinity_test.cpp", "wo": "WO-082", "verified_by": "just runtime-test job"},
{"id": "RT-INV-JOB-05", "rule": "ARCH3-R067", "text": "fail cancels exactly the transitive dependents (job_cancelled) and the waiter receives job_failed; fail_fatal (RFP3) reaches the fault sink and the worker runs the next job", "test": "tests/unit/runtime/job_failure_test.cpp", "wo": "WO-082", "verified_by": "just runtime-test job"},
{"id": "RT-INV-JOB-06", "rule": "ARCH3-R068", "text": "the correlation id, scope and tick seen inside a job equal the submitter's", "test": "tests/unit/runtime/job_correlation_test.cpp", "wo": "WO-082", "verified_by": "just runtime-test job"},
{"id": "RT-INV-JOB-07", "rule": "ARCH3-R069", "text": "a planted lost wakeup makes the watchdog report deadlock_suspected naming the stuck job within watchdog_ms (500 ms in the test)", "test": "tests/unit/runtime/job_watchdog_test.cpp", "wo": "WO-082", "verified_by": "just runtime-test job"},
{"id": "RT-INV-JOB-08", "rule": "ARCH3-R030", "text": "under a saturating frame stream a background job completes, and its result reaches the owner only through drain_results in submission order", "test": "tests/unit/runtime/job_background_test.cpp", "wo": "WO-082", "verified_by": "just runtime-test job"},
{"id": "RT-INV-ENT-01", "rule": "ARCH3-R103", "text": "a destroyed handle is never alive again across 1e6 seeded create and destroy cycles (scaled)", "test": "tests/unit/runtime/ent_stale_test.cpp", "wo": "WO-083", "verified_by": "just runtime-test ent"},
{"id": "RT-INV-ENT-02", "rule": "ARCH3-R101", "text": "generation saturation (RFP10 fast-forward) retires the slot and it is never returned", "test": "tests/unit/runtime/ent_retire_test.cpp", "wo": "WO-083", "verified_by": "just runtime-test ent"},
{"id": "RT-INV-ENT-03", "rule": "ARCH3-R102", "text": "freed indices are reused FIFO only while at least 1024 are free, and the index sequence is identical across runs", "test": "tests/unit/runtime/ent_reuse_test.cpp", "wo": "WO-083", "verified_by": "just runtime-test ent"},
{"id": "RT-INV-ENT-04", "rule": "ARCH3-R104", "text": "capacity exhaustion is entity_capacity and changes nothing; pages never move", "test": "tests/unit/runtime/ent_capacity_test.cpp", "wo": "WO-083", "verified_by": "just runtime-test ent"},
{"id": "RT-INV-ENT-05", "rule": "ARCH3-R105", "text": "entity_map is one-to-one, refuses a duplicate key or entity with already_exists and iterates in ascending key order", "test": "tests/unit/runtime/ent_map_test.cpp", "wo": "WO-083", "verified_by": "just runtime-test ent"},
{"id": "RT-INV-ENT-06", "rule": "ARCH3-R103", "text": "checked access through a stale handle is stale_entity and unchecked access trips the captured debug assertion", "test": "tests/unit/runtime/ent_access_test.cpp", "wo": "WO-083", "verified_by": "just runtime-test ent"},
{"id": "RT-INV-CLOCK-01", "rule": "ARCH3-R078", "text": "60 Hz over 3600 s of seeded jittered deltas yields exactly 216000 ticks, and rates 30, 50, 120, 144 and 60000/1001 keep exact counts", "test": "tests/unit/runtime/clock_drift_test.cpp", "wo": "WO-084", "verified_by": "just runtime-test clock"},
{"id": "RT-INV-CLOCK-02", "rule": "ARCH3-R080", "text": "a 30 s delta yields at most max_catchup ticks, dropped_ns = 30 s - max_frame_delta, dropped ticks counted and remainder below one tick", "test": "tests/unit/runtime/clock_catchup_test.cpp", "wo": "WO-084", "verified_by": "just runtime-test clock"},
{"id": "RT-INV-CLOCK-03", "rule": "ARCH3-R079", "text": "a backward or equal sample (RFP6) gives delta 0 and non_monotonic + 1, and tick and frame index never decrease", "test": "tests/unit/runtime/clock_monotonic_test.cpp", "wo": "WO-084", "verified_by": "just runtime-test clock"},
{"id": "RT-INV-CLOCK-04", "rule": "ARCH3-R026", "text": "suspend and resume across a 10 s manual jump accrue no tick and the first delta after resume is 0", "test": "tests/unit/runtime/clock_suspend_test.cpp", "wo": "WO-084", "verified_by": "just runtime-test clock"},
{"id": "RT-INV-CLOCK-05", "rule": "ARCH3-R083", "text": "inside a tick_scope the thread's sl::log correlation tick equals the tick and the scope is runtime/<instance>", "test": "tests/unit/runtime/clock_correlation_test.cpp", "wo": "WO-084", "verified_by": "just runtime-test clock"},
{"id": "RT-INV-DET-01", "rule": "ARCH3-R089", "text": "rng_stream equals tests/golden/runtime/rng_vectors.json for 16 (seed, stream) pairs and a stream restored from its captured state continues identically", "test": "tests/unit/runtime/det_rng_test.cpp", "wo": "WO-084", "verified_by": "just runtime-test det"},
{"id": "RT-INV-DET-02", "rule": "ARCH3-R088", "text": "state_hasher equals tests/golden/runtime/hash_vectors.json; -0.0 and +0.0 hash equal, all NaN payloads hash equal, u32(5) and u64(5) differ", "test": "tests/unit/runtime/det_hash_test.cpp", "wo": "WO-084", "verified_by": "just runtime-test det"},
{"id": "RT-INV-DET-03", "rule": "ARCH3-R086", "text": "merge_ordered yields the same sequence for 1, 2, 4 and 8 producers over 200 seeded runs", "test": "tests/unit/runtime/det_merge_test.cpp", "wo": "WO-084", "verified_by": "just runtime-test det"},
{"id": "RT-INV-DIAG-01", "rule": "ARCH3-R052", "text": "peak bytes equal the oracle maximum of live bytes, fragmentation per-mille equals the oracle on planted layouts, and external tags never enter tracked live bytes", "test": "tests/unit/runtime/diag_peak_frag_test.cpp", "wo": "WO-085", "verified_by": "just runtime-test diag"},
{"id": "RT-INV-DIAG-02", "rule": "ARCH3-R053", "text": "to_json then from_json round trips every counter and equal reports serialize byte-identically", "test": "tests/unit/runtime/diag_json_test.cpp", "wo": "WO-085", "verified_by": "just runtime-test diag"},
{"id": "RT-INV-DIAG-03", "rule": "ARCH3-R054", "text": "the leak report is sorted by (tag, site, size) and byte-identical across runs and worker counts", "test": "tests/unit/runtime/diag_leak_order_test.cpp", "wo": "WO-085", "verified_by": "just runtime-test diag"},
{"id": "RT-INV-DIAG-04", "rule": "ARCH3-R055", "text": "reports read concurrently with 8 allocating threads are TSan clean and monotone in totals", "test": "tests/integration/runtime/diag_concurrent_test.cpp", "wo": "WO-085", "verified_by": "just runtime-test diag"},
{"id": "RT-INV-DIAG-05", "rule": "ARCH3-R053", "text": "one runtime.mem log event is emitted per report_interval frames with the sl-log-event/1 fields and correlation scope runtime/<instance>", "test": "tests/unit/runtime/diag_log_event_test.cpp", "wo": "WO-085", "verified_by": "just runtime-test diag"},
{"id": "RT-INV-DIAG-06", "rule": "ARCH3-R053", "text": "memreport_check accepts the schema example and rejects each planted bad report naming the field", "test": "tools/runtime/test_memreport_check.py", "wo": "WO-085", "verified_by": "just runtime-memreport-check"},
{"id": "RT-INV-STEAL-01", "rule": "ARCH3-R073", "text": "every pushed item is popped or stolen exactly once on both queues with 1 owner and {1, 3, 7, 15} thieves (scaled)", "test": "tests/integration/runtime/steal_linearizable_test.cpp", "wo": "WO-086", "verified_by": "just runtime-test steal"},
{"id": "RT-INV-STEAL-02", "rule": "ARCH3-R073", "text": "single-threaded owner pop is LIFO and steal is FIFO on both queues", "test": "tests/unit/runtime/steal_order_test.cpp", "wo": "WO-086", "verified_by": "just runtime-test steal"},
{"id": "RT-INV-STEAL-03", "rule": "ARCH3-R072", "text": "ws_deque growth from 4096 cells under a 1e5-push burst with concurrent thieves loses nothing", "test": "tests/integration/runtime/steal_growth_test.cpp", "wo": "WO-086", "verified_by": "just runtime-test steal"},
{"id": "RT-INV-STEAL-04", "rule": "ARCH3-R075", "text": "a background job starts within aging_limit rounds under saturation, the injection queue is polled every 61 pops, and the victim sequence is identical for the same seed", "test": "tests/unit/runtime/steal_fairness_test.cpp", "wo": "WO-086", "verified_by": "just runtime-test steal"},
{"id": "RT-INV-STEAL-05", "rule": "ARCH3-R073", "text": "the DAG workload gives identical results on locked_deque and ws_deque and scheduler counters sum to the jobs run", "test": "tests/unit/runtime/steal_equiv_test.cpp", "wo": "WO-086", "verified_by": "just runtime-test steal"},
{"id": "RT-INV-STEAL-06", "rule": "ARCH3-R074", "text": "idle workers park after spin_limit and every submit after a park is run (1e4 rounds, scaled)", "test": "tests/integration/runtime/steal_park_test.cpp", "wo": "WO-086", "verified_by": "just runtime-test steal"},
{"id": "RT-INV-REFL-01", "rule": "ARCH3-R133", "text": "field metadata (offset, size, kind) equals offsetof and sizeof for every reference type", "test": "tests/unit/runtime/refl_fields_test.cpp", "wo": "WO-087", "verified_by": "just runtime-test refl"},
{"id": "RT-INV-REFL-02", "rule": "ARCH3-R134", "text": "objects with equal fields and different padding bytes encode and hash identically", "test": "tests/unit/runtime/refl_padding_test.cpp", "wo": "WO-087", "verified_by": "just runtime-test refl"},
{"id": "RT-INV-REFL-03", "rule": "ARCH3-R134", "text": "encode then decode round trips every reference type bit-exactly with NaN canonical", "test": "tests/unit/runtime/refl_roundtrip_test.cpp", "wo": "WO-087", "verified_by": "just runtime-test refl"},
{"id": "RT-INV-REFL-04", "rule": "ARCH3-R133", "text": "invoking a method with wrong arguments is reflect_mismatch, unknown names are type_unknown or field_unknown, and nothing throws", "test": "tests/unit/runtime/refl_invoke_test.cpp", "wo": "WO-087", "verified_by": "just runtime-test refl"},
{"id": "RT-INV-REFL-05", "rule": "ARCH3-R135", "text": "a binding with a mismatched field number or kind is component_schema_mismatch, a transient type has no schema type, and registration after freeze is type_registry_frozen", "test": "tests/unit/runtime/refl_binding_test.cpp", "wo": "WO-087", "verified_by": "just runtime-test refl"},
{"id": "RT-INV-REFL-06", "rule": "ARCH3-R136", "text": "the registry dump is sorted by name and byte-identical across runs", "test": "tests/unit/runtime/refl_dump_test.cpp", "wo": "WO-087", "verified_by": "just runtime-test refl"},
{"id": "RT-INV-STORE-01", "rule": "ARCH3-R107", "text": "after seeded churn every live entity is in exactly one archetype row and row counts equal live counts", "test": "tests/unit/runtime/store_rows_test.cpp", "wo": "WO-088", "verified_by": "just runtime-test store"},
{"id": "RT-INV-STORE-02", "rule": "ARCH3-R110", "text": "component values survive arbitrary add and remove moves (oracle map) and each value is destroyed exactly once", "test": "tests/unit/runtime/store_move_test.cpp", "wo": "WO-088", "verified_by": "just runtime-test store"},
{"id": "RT-INV-STORE-03", "rule": "ARCH3-R108", "text": "chunks are 16384 bytes, columns 64-byte aligned in (alignment desc, id asc) order, and capacity is the largest n <= 1024 that fits", "test": "tests/unit/runtime/store_layout_test.cpp", "wo": "WO-088", "verified_by": "just runtime-test store"},
{"id": "RT-INV-STORE-04", "rule": "ARCH3-R106", "text": "the frozen registry refuses with component_registry_frozen, a 4096-byte or 128-aligned type is component_invalid, and a throwing-move type is rejected by a requires-expression check", "test": "tests/unit/runtime/store_registry_test.cpp", "wo": "WO-088", "verified_by": "just runtime-test store"},
{"id": "RT-INV-STORE-05", "rule": "ARCH3-R110", "text": "RFP0 at every allocation index of a structural-change batch leaves the world hash unchanged and leaks nothing", "test": "tests/integration/runtime/store_alloc_sweep_test.cpp", "wo": "WO-088", "verified_by": "just runtime-test store"},
{"id": "RT-INV-STORE-06", "rule": "ARCH3-R111", "text": "iteration order equals the oracle (archetype creation, chunk, row) for identical histories", "test": "tests/unit/runtime/store_iteration_order_test.cpp", "wo": "WO-088", "verified_by": "just runtime-test store"},
{"id": "RT-INV-STORE-07", "rule": "ARCH3-R109", "text": "a structural call inside a phase is structural_change_forbidden, during iteration it trips the captured guard, and empty chunks return to the pool at the boundary", "test": "tests/unit/runtime/store_guard_test.cpp", "wo": "WO-088", "verified_by": "just runtime-test store"},
{"id": "RT-INV-FRAME-01", "rule": "ARCH3-R094", "text": "phases run in table order and simulate repeats pre_tick, tick and post_tick exactly the stepper's count", "test": "tests/unit/runtime/frame_phase_order_test.cpp", "wo": "WO-089", "verified_by": "just runtime-test frame"},
{"id": "RT-INV-FRAME-02", "rule": "ARCH3-R095", "text": "no job of phase p+1 starts before every job of phase p completed, and end_frame starts with zero frame jobs in flight", "test": "tests/unit/runtime/frame_sync_boundary_test.cpp", "wo": "WO-089", "verified_by": "just runtime-test frame"},
{"id": "RT-INV-FRAME-03", "rule": "ARCH3-R096", "text": "boundary hooks run on the owner thread in boundary_slots order between end_frame and the next begin_frame, and main-affinity jobs are drained there", "test": "tests/unit/runtime/frame_hooks_test.cpp", "wo": "WO-089", "verified_by": "just runtime-test frame"},
{"id": "RT-INV-FRAME-04", "rule": "ARCH3-R051", "text": "after 10 warm-up frames the upstream allocation delta per frame is 0 and the frame arena flips once per end_frame", "test": "tests/unit/runtime/frame_zero_alloc_test.cpp", "wo": "WO-089", "verified_by": "just runtime-test frame"},
{"id": "RT-INV-FRAME-05", "rule": "ARCH3-R100", "text": "intra-phase dependencies are honoured, independent tasks overlap on 2 or more workers, and frame_stats reports per-phase job counts and the critical path", "test": "tests/unit/runtime/frame_task_deps_test.cpp", "wo": "WO-089", "verified_by": "just runtime-test frame"},
{"id": "RT-INV-FRAME-06", "rule": "ARCH3-R098", "text": "a retire_after(2) callback runs exactly at the end_frame two frames later, on the owner thread", "test": "tests/unit/runtime/frame_retire_test.cpp", "wo": "WO-089", "verified_by": "just runtime-test frame"},
{"id": "RT-INV-QUERY-01", "rule": "ARCH3-R114", "text": "query results equal a brute-force filter for seeded worlds and term combinations including optional and without", "test": "tests/unit/runtime/query_oracle_test.cpp", "wo": "WO-090", "verified_by": "just runtime-test query"},
{"id": "RT-INV-QUERY-02", "rule": "ARCH3-R115", "text": "after a new archetype the cache visits only archetypes created since its last match", "test": "tests/unit/runtime/query_cache_test.cpp", "wo": "WO-090", "verified_by": "just runtime-test query"},
{"id": "RT-INV-QUERY-03", "rule": "ARCH3-R116", "text": "par_for_each visits each match exactly once on 1 to 16 workers and the batch partition is independent of the worker count", "test": "tests/integration/runtime/query_parallel_test.cpp", "wo": "WO-090", "verified_by": "just runtime-test query"},
{"id": "RT-INV-QUERY-04", "rule": "ARCH3-R114", "text": "contradictory terms are query_invalid, the derived access set equals the terms, and a structural call during iteration trips the guard", "test": "tests/unit/runtime/query_terms_test.cpp", "wo": "WO-090", "verified_by": "just runtime-test query"},
{"id": "RT-INV-QUERY-05", "rule": "ARCH3-R117", "text": "write terms stamp the changed tick of each visited row and changed/added(since) select exactly rows with a later tick", "test": "tests/unit/runtime/query_change_filter_test.cpp", "wo": "WO-090", "verified_by": "just runtime-test query"},
{"id": "RT-INV-CMD-01", "rule": "ARCH3-R119", "text": "playback order is the ordered_key order whatever the job completion order (1 to 16 workers, permuted completion)", "test": "tests/unit/runtime/cmd_playback_order_test.cpp", "wo": "WO-091", "verified_by": "just runtime-test cmd"},
{"id": "RT-INV-CMD-02", "rule": "ARCH3-R119", "text": "pending entities resolve at flush in key order, handles are identical across worker counts, and set on a pending entity applies after its spawn", "test": "tests/unit/runtime/cmd_pending_test.cpp", "wo": "WO-091", "verified_by": "just runtime-test cmd"},
{"id": "RT-INV-CMD-03", "rule": "ARCH3-R120", "text": "commands on stale entities are skipped and counted and flush_report lists the applied operations in order", "test": "tests/unit/runtime/cmd_flush_report_test.cpp", "wo": "WO-091", "verified_by": "just runtime-test cmd"},
{"id": "RT-INV-CMD-04", "rule": "ARCH3-R118", "text": "command buffers draw only frame memory: zero upstream allocations per steady frame after warm-up", "test": "tests/unit/runtime/cmd_memory_test.cpp", "wo": "WO-091", "verified_by": "just runtime-test cmd"},
{"id": "RT-INV-EVT-01", "rule": "ARCH3-R139", "text": "delivery order is (phase, system_index, chunk_index, seq) whatever the worker count, and perturbed correlation ids change neither order nor bytes", "test": "tests/unit/runtime/evt_order_test.cpp", "wo": "WO-092", "verified_by": "just runtime-test evt"},
{"id": "RT-INV-EVT-02", "rule": "ARCH3-R139", "text": "an event is visible from the next boundary until the end of the following frame and never after", "test": "tests/unit/runtime/evt_lifetime_test.cpp", "wo": "WO-092", "verified_by": "just runtime-test evt"},
{"id": "RT-INV-EVT-03", "rule": "ARCH3-R137", "text": "an unregistered event type is event_type_unknown at lookup and cannot be written", "test": "tests/unit/runtime/evt_typed_test.cpp", "wo": "WO-092", "verified_by": "just runtime-test evt"},
{"id": "RT-INV-EVT-04", "rule": "ARCH3-R142", "text": "exported envelopes are valid fabric.event envelopes from one source with strictly increasing seq", "test": "tests/unit/runtime/evt_export_test.cpp", "wo": "WO-092", "verified_by": "just runtime-test evt"},
{"id": "RT-INV-EVT-05", "rule": "ARCH3-R141", "text": "overflow drops the newest, counts exactly, keeps the order of the kept events, and try_write is event_overflow", "test": "tests/unit/runtime/evt_overflow_test.cpp", "wo": "WO-092", "verified_by": "just runtime-test evt"},
{"id": "RT-INV-EVT-06", "rule": "ARCH3-R140", "text": "an invalid payload is event_payload_invalid in Debug", "test": "tests/unit/runtime/evt_validate_test.cpp", "wo": "WO-092", "verified_by": "just runtime-test evt"},
{"id": "RT-INV-EVT-07", "rule": "ARCH3-R091", "text": "the pending-event contributor round trips and its bytes and hash exclude correlation", "test": "tests/unit/runtime/evt_snapshot_test.cpp", "wo": "WO-092", "verified_by": "just runtime-test evt"},
{"id": "RT-INV-CHG-01", "rule": "ARCH3-R128", "text": "changed<T> yields exactly the rows written since the reading system's last run (oracle) and each change is seen once per reader across frames", "test": "tests/unit/runtime/chg_changed_test.cpp", "wo": "WO-093", "verified_by": "just runtime-test chg"},
{"id": "RT-INV-CHG-02", "rule": "ARCH3-R127", "text": "added<T> includes created and component-added rows only", "test": "tests/unit/runtime/chg_added_test.cpp", "wo": "WO-093", "verified_by": "just runtime-test chg"},
{"id": "RT-INV-CHG-03", "rule": "ARCH3-R129", "text": "structural signals arrive in playback order and equal the flush_report history", "test": "tests/unit/runtime/chg_signals_test.cpp", "wo": "WO-093", "verified_by": "just runtime-test chg"},
{"id": "RT-INV-CHG-04", "rule": "ARCH3-R130", "text": "the world version increments iff a structural change happened and the archetype generation iff an archetype was created", "test": "tests/unit/runtime/chg_versions_test.cpp", "wo": "WO-093", "verified_by": "just runtime-test chg"},
{"id": "RT-INV-CHG-05", "rule": "ARCH3-R131", "text": "after a fast-forwarded tick wrap changed<T> and added<T> stay correct", "test": "tests/unit/runtime/chg_wrap_test.cpp", "wo": "WO-093", "verified_by": "just runtime-test chg"},
{"id": "RT-INV-SCHED-01", "rule": "ARCH3-R122", "text": "no two conflicting systems overlap in time over 1000 seeded schedules (instrumented intervals, scaled)", "test": "tests/integration/runtime/sched_no_overlap_test.cpp", "wo": "WO-094", "verified_by": "just runtime-test sched"},
{"id": "RT-INV-SCHED-02", "rule": "ARCH3-R122", "text": "non-conflicting systems overlap when 2 or more workers exist", "test": "tests/unit/runtime/sched_parallelism_test.cpp", "wo": "WO-094", "verified_by": "just runtime-test sched"},
{"id": "RT-INV-SCHED-03", "rule": "ARCH3-R123", "text": "conflicting systems run in constraint order, then registration order, system_index is deterministic, and an unknown name is system_unknown", "test": "tests/unit/runtime/sched_order_test.cpp", "wo": "WO-094", "verified_by": "just runtime-test sched"},
{"id": "RT-INV-SCHED-04", "rule": "ARCH3-R124", "text": "a constraint cycle is schedule_cycle naming every system in it", "test": "tests/unit/runtime/sched_cycle_test.cpp", "wo": "WO-094", "verified_by": "just runtime-test sched"},
{"id": "RT-INV-SCHED-05", "rule": "ARCH3-R125", "text": "an undeclared write fails the system with undeclared_access", "test": "tests/unit/runtime/sched_access_guard_test.cpp", "wo": "WO-094", "verified_by": "just runtime-test sched"},
{"id": "RT-INV-SCHED-06", "rule": "ARCH3-R090", "text": "serial_canonical and parallel give identical per-tick state hashes for the sim_fixture reference systems on 1, 2, 4 and 8 workers", "test": "tests/integration/runtime/sched_canonical_test.cpp", "wo": "WO-094", "verified_by": "just runtime-test sched"},
{"id": "RT-INV-SCHED-07", "rule": "ARCH3-R126", "text": "the exported schedule is byte-identical across runs and worker counts", "test": "tests/unit/runtime/sched_export_test.cpp", "wo": "WO-094", "verified_by": "just runtime-test sched"},
{"id": "RT-INV-SNAP-01", "rule": "ARCH3-R145", "text": "take then restore reproduces entity handles, iteration order and state hash", "test": "tests/unit/runtime/snap_roundtrip_test.cpp", "wo": "WO-095", "verified_by": "just runtime-test snap"},
{"id": "RT-INV-SNAP-02", "rule": "ARCH3-R145", "text": "300 ticks after restore equal 300 ticks of the original, hash per tick", "test": "tests/integration/runtime/snap_continue_test.cpp", "wo": "WO-095", "verified_by": "just runtime-test snap"},
{"id": "RT-INV-SNAP-03", "rule": "ARCH3-R144", "text": "every single-bit flip of a small snapshot is snapshot_corrupt", "test": "tests/unit/runtime/snap_bitflip_test.cpp", "wo": "WO-095", "verified_by": "just runtime-test snap"},
{"id": "RT-INV-SNAP-04", "rule": "ARCH3-R147", "text": "a crash after every storage primitive of save under each faulty-vfs crash policy (RFP8) leaves the old or the new snapshot loadable", "test": "tests/integration/runtime/snap_crash_sweep_test.cpp", "wo": "WO-095", "verified_by": "just runtime-test snap"},
{"id": "RT-INV-SNAP-05", "rule": "ARCH3-R144", "text": "take inside a phase is snapshot_mid_tick, and a type-digest, runtime_abi_digest or contributor mismatch is refused with its code and restores nothing", "test": "tests/unit/runtime/snap_compat_test.cpp", "wo": "WO-095", "verified_by": "just runtime-test snap"},
{"id": "RT-INV-SNAP-06", "rule": "ARCH3-R146", "text": "the breakdown localizes a planted single-component divergence to its archetype and component", "test": "tests/unit/runtime/snap_breakdown_test.cpp", "wo": "WO-095", "verified_by": "just runtime-test snap"},
{"id": "RT-INV-LIFE-01", "rule": "ARCH3-R022", "text": "every (state, event) pair outside rt_states is invalid_state with the state unchanged, and each legal transition emits exactly one runtime.lifecycle event", "test": "tests/unit/runtime/life_state_table_test.cpp", "wo": "WO-096", "verified_by": "just runtime-test life"},
{"id": "RT-INV-LIFE-02", "rule": "ARCH3-R025", "text": "startup failing at each step (RFP1 at worker k, RFP0 at the memory root) leaves zero threads, zero tracked bytes and state Created", "test": "tests/unit/runtime/life_startup_unwind_test.cpp", "wo": "WO-096", "verified_by": "just runtime-test life"},
{"id": "RT-INV-LIFE-03", "rule": "ARCH3-R027", "text": "shutdown joins all workers, cancels pending background jobs and reports zero leaks, and lifecycle calls from another thread are wrong_thread", "test": "tests/unit/runtime/life_shutdown_test.cpp", "wo": "WO-096", "verified_by": "just runtime-test life"},
{"id": "RT-INV-LIFE-04", "rule": "ARCH3-R026", "text": "suspend and resume at end_frame give the final state hash of an uninterrupted run and no tick accrues while suspended", "test": "tests/unit/runtime/life_suspend_test.cpp", "wo": "WO-096", "verified_by": "just runtime-test life"},
{"id": "RT-INV-LIFE-05", "rule": "ARCH3-R028", "text": "the faulted frame's command buffers, events and observations are never applied and one sl-runtime-fault/1 report is produced", "test": "tests/unit/runtime/life_fault_isolation_test.cpp", "wo": "WO-096", "verified_by": "just runtime-test life"},
{"id": "RT-INV-LIFE-06", "rule": "ARCH3-R029", "text": "restore_snapshot recovery ends with the fault-free hash, more than max_recoveries faults in a window stop with recovery_failed, and the default policy is stop", "test": "tests/integration/runtime/life_recovery_test.cpp", "wo": "WO-096", "verified_by": "just runtime-test life"},
{"id": "RT-INV-LIFE-07", "rule": "ARCH3-R031", "text": "a swap applies only at end_frame, restart classes return restart_required, and a failing health check restores the pre-swap snapshot so the next 60 ticks hash equal to a run without the swap", "test": "tests/integration/runtime/life_swap_test.cpp", "wo": "WO-096", "verified_by": "just runtime-test life"},
{"id": "RT-INV-LIFE-08", "rule": "ARCH3-R030", "text": "a restore while background jobs are in flight discards their results and the continued run equals the original", "test": "tests/unit/runtime/life_background_test.cpp", "wo": "WO-096", "verified_by": "just runtime-test life"},
{"id": "RT-INV-BND-01", "rule": "ARCH3-R152", "text": "load_plan groups records by document then id; equal projections give equal plans, independent of commit order over 100 seeded projects", "test": "tests/unit/runtime/bnd_load_plan_test.cpp", "wo": "WO-097", "verified_by": "just runtime-test bnd"},
{"id": "RT-INV-BND-02", "rule": "ARCH3-R153", "text": "ingest maps every document-bound in-scope record to exactly one entity, entity_map round trips, and unbound types are skipped and counted", "test": "tests/unit/runtime/bnd_ingest_test.cpp", "wo": "WO-097", "verified_by": "just runtime-test bnd"},
{"id": "RT-INV-BND-03", "rule": "ARCH3-R157", "text": "rebase applies only the diff, keeps the handles of unchanged records, despawns retired ones and stamps no changed tick", "test": "tests/unit/runtime/bnd_rebase_test.cpp", "wo": "WO-097", "verified_by": "just runtime-test bnd"},
{"id": "RT-INV-BND-04", "rule": "ARCH3-R154", "text": "the outbox coalesces to the last value per (record, path) and orders by (record id bytes, path) over 500 seeded permutations; transient components never appear", "test": "tests/unit/runtime/bnd_outbox_test.cpp", "wo": "WO-097", "verified_by": "just runtime-test bnd"},
{"id": "RT-INV-BND-05", "rule": "ARCH3-R154", "text": "after one observe-commit-rebase round trip with no runtime writes, 300 further frames propose zero patches", "test": "tests/integration/runtime/bnd_echo_test.cpp", "wo": "WO-097", "verified_by": "just runtime-test bnd"},
{"id": "RT-INV-BND-06", "rule": "ARCH3-R158", "text": "after rebase exactly one projection is referenced and external fabric.projection equals the summed body bytes", "test": "tests/unit/runtime/bnd_projection_memory_test.cpp", "wo": "WO-097", "verified_by": "just runtime-test bnd"},
{"id": "RT-INV-BND-07", "rule": "ARCH3-R161", "text": "through testing::fabric_host an observation commits with origin observed under rebase_if_commutes, F5 bypassing stays 0, an out-of-scope observation is counted as rejected and the frame continues", "test": "tests/integration/runtime/bnd_sink_e2e_test.cpp", "wo": "WO-097", "verified_by": "just runtime-test bnd"},
{"id": "RT-INV-BND-08", "rule": "ARCH3-R148", "text": "world_link and document_source expose no member returning a fabric snapshot, store, workspace or transaction (requires-expression checks)", "test": "tests/unit/runtime/bnd_surface_test.cpp", "wo": "WO-097", "verified_by": "just runtime-test bnd"},
{"id": "RT-INV-STRESS-01", "rule": "ARCH3-R162", "text": "S1 job DAG fuzz and S3 nested fork-join hold exactly-once and ordering in every cell with the watchdog silent", "test": "tests/integration/runtime/stress_jobs_test.cpp", "wo": "WO-098", "verified_by": "just runtime-stress"},
{"id": "RT-INV-STRESS-02", "rule": "ARCH3-R162", "text": "S2 deque linearizability holds on both queues at stress scale", "test": "tests/integration/runtime/stress_deque_test.cpp", "wo": "WO-098", "verified_by": "just runtime-stress"},
{"id": "RT-INV-STRESS-03", "rule": "ARCH3-R162", "text": "S4 allocator churn balances with zero leaks and S5 RFP0 sweeps leave the world unchanged", "test": "tests/integration/runtime/stress_alloc_test.cpp", "wo": "WO-098", "verified_by": "just runtime-stress"},
{"id": "RT-INV-STRESS-04", "rule": "ARCH3-R166", "text": "S6 ECS churn: no stale handle resolves, rows equal live entities, the hash is schedule-independent, and steady frames emit zero log events and make zero upstream allocations", "test": "tests/integration/runtime/stress_ecs_test.cpp", "wo": "WO-098", "verified_by": "just runtime-stress"},
{"id": "RT-INV-STRESS-05", "rule": "ARCH3-R084", "text": "S7: per-tick hashes are equal for workers {1, 2, 4, 8, 16}, serial_canonical, RFP4 chaos stalls under 8 seeds and perturbed correlation ids", "test": "tests/integration/runtime/stress_det_matrix_test.cpp", "wo": "WO-098", "verified_by": "just runtime-stress"},
{"id": "RT-INV-STRESS-06", "rule": "ARCH3-R033", "text": "S8 lifecycle storm with RFP1, RFP5 and RFP9 never hangs, leaks or leaves the table, and S9 two runtimes in one process hash equal with independent memory reports", "test": "tests/integration/runtime/stress_lifecycle_test.cpp", "wo": "WO-098", "verified_by": "just runtime-stress"},
{"id": "RT-INV-STRESS-07", "rule": "ARCH3-R141", "text": "S10 event flood keeps delivery order and counts overflow exactly", "test": "tests/integration/runtime/stress_events_test.cpp", "wo": "WO-098", "verified_by": "just runtime-stress"},
{"id": "RT-INV-STRESS-08", "rule": "ARCH3-R020", "text": "every failpoint RFP0-RFP10 is visited by its workload and its armed outcome matches the failpoints table", "test": "tests/integration/runtime/stress_failpoints_test.cpp", "wo": "WO-098", "verified_by": "just runtime-stress"},
{"id": "RT-INV-GATE-01", "rule": "ARCH3-R167", "text": "the scaled demo exits 0 printing runtime-demo: ok, writes each gate_evidence file with its format tag and nothing outside the evidence directory; a usage error exits 2", "test": "tests/integration/runtime/gate_demo_test.cpp", "wo": "WO-099", "verified_by": "just runtime-test gate"},
{"id": "RT-INV-GATE-02", "rule": "ARCH3-R029", "text": "the fault at tick 450 restores the tick-300 snapshot and the final hash equals the fault-free hash", "test": "tests/integration/runtime/gate_recovery_test.cpp", "wo": "WO-099", "verified_by": "just runtime-test gate"},
{"id": "RT-INV-GATE-03", "rule": "ARCH3-R051", "text": "the demo shuts down with zero leaks, zero upstream allocations and zero log events per steady frame", "test": "tests/integration/runtime/gate_memory_test.cpp", "wo": "WO-099", "verified_by": "just runtime-test gate"},
{"id": "RT-INV-GATE-04", "rule": "ARCH3-R161", "text": "observations are committed by the host through the sink, Fabric mutations equal host commits and F5 bypassing is 0", "test": "tests/integration/runtime/gate_fabric_test.cpp", "wo": "WO-099", "verified_by": "just runtime-test gate"},
{"id": "RT-INV-GATE-05", "rule": "ARCH3-R170", "text": "determinism_check accepts an all-equal hash matrix and rejects a single divergent cell naming workers, mode and tick", "test": "tools/runtime/test_determinism_check.py", "wo": "WO-099", "verified_by": "just runtime-determinism"},
{"id": "RT-INV-GATE-06", "rule": "ARCH3-R020", "text": "the contract dump equals the SL-ARCH-003 sl-data tables and schemas/runtime/contract.lock.json, and a drifted value is named", "test": "tools/runtime/test_contract_lock.py", "wo": "WO-100", "verified_by": "just runtime-contract-lock"},
{"id": "RT-INV-GATE-07", "rule": "ARCH3-R171", "text": "the scorecard rejects each planted bad input (leak, sanitizer report, divergence, F5 bypass, stress failure, coverage < 1.0) naming the measure", "test": "tools/runtime/test_scorecard.py", "wo": "WO-100", "verified_by": "just runtime-scorecard"}
],
"retired": []}
```

## Measures and enforcement points

- Invariant coverage (ARCH3-R014): invariants with a passing test over invariants, threshold 1.0; `just runtime-contracts` (pending-tolerant, WO-077), `just runtime-contracts-strict` (WO-100), `just neg-runtime-contracts`.
- Contract tables (every sl-data block): `just runtime-contract-lock` compares `sl_runtime_dump_contract` with this document and `schemas/runtime/contract.lock.json` (WO-100); `just neg-runtime-contract-lock`.
- Static rules (layout, Fabric includes, TXCALL, math, globals, layers, nothrow, fences, orders, platform-only calls, link, outputs): `just runtime-lint` and `just neg-runtime-lint` (WO-077); `just fabric-surface-check` keeps R-BND-INCLUDE (WO-061) over engine/runtime.
- Sanitizers (ARCH3-R163): `just runtime-san asan|ubsan|tsan <area>` per order, fail-closed; `just neg-runtime-race` (TSan), `just neg-runtime-poison` (ASan), `just neg-runtime-leak`, `just neg-runtime-stress`.
- Compilers and static analysis per order: `just runtime-gcc <area>` (gcc -Werror with -Wshadow and -Wsign-conversion over runtime targets only) and `just runtime-tidy <area>` (repo .clang-tidy over the area's files); full workflows at the wave gate and in check-full.
- Determinism D1 (ARCH3-R085): RT-INV-SCHED-06, RT-INV-STRESS-05, `just runtime-determinism` (workers {1, 2, 4, 8, 16}, serial versus parallel, resume, replay), `just neg-runtime-determinism`; clang versus gcc recorded in hash-matrix.json, not gated.
- F5 (ARCH3-R161): numerator Fabric mutations that bypass the transaction layer during a runtime workload, threshold 0; RT-INV-BND-07, RT-INV-GATE-04, `just runtime-scorecard`.
- Memory: leaks at shutdown 0 and upstream allocations per steady frame 0 (RT-INV-FRAME-04, RT-INV-GATE-03); `just runtime-memreport-check` validates sl-runtime-memreport/1.
- Performance (master plan section 6.1): work-bound counters only (archetypes visited per new archetype, chunk visits, steals, upstream allocations per steady frame, log events per steady frame); wall times are recorded in docs/episodes/081-100.md, never asserted; benchmarks are Volume 16.
- Gate 100 (ARCH3-R172): `just runtime-gate` composes all of the above and every neg-runtime-* test; `just acceptance-081-100` adds runtime-san x3, runtime-determinism and check-full (WO-101).
- Scorecard (ARCH3-R171): `just runtime-scorecard` writes build/runtime/scorecard.json (sl-runtime-scorecard/1) from the gate evidence; an absent optional input (sanitizer logs, hash-matrix.json outside check-full) is `not_run`, never a pass; `just neg-runtime-scorecard`.
- Wall times measured at reconciliation: `just runtime-demo` about 500 s in Debug and `just runtime-determinism` about 820 s, so both belong in check-full or the gate composition, never in `fast` (WO-101).
- Every work order: the acceptance commands of its file; wo-verify runs runtime orders with --ac-timeout 1800 and WO-101 with --timeout 9000 --ac-timeout 7200.

## Traceability

Master plan sections 1.5 (rows Runtime model, Determinism), 2.1, 2.2 (row 03), 2.3, 2.5, 3 (F5, F6), 3.3, 4.2, 4.3
(gate episode 100), 5.1 (rows Platform layer, Memory subsystem, Job system, Events and input), 6.1, 6.2, 6.3;
Volume 05 episodes SL-EP-081 to SL-EP-100; Volume 17 episodes 321-340 as seams only. SL-ARCH-001 (ARCH1-R08,
ARCH1-R40, ARCH1-R46 to R48, ARCH1-R60 to R62) and SL-ARCH-002 (the commit path the host uses). Decisions ADR-0001,
ADR-0002, ADR-0009, ADR-0015, ADR-0017, ADR-0021 to ADR-0028. Work orders WO-075 to WO-101 (waves in
build/design/vol05-draft/WAVES.md until merged). Plans: build/design/vol05-plan-0.json (boundary and determinism
sections), vol05-plan-1.json (limits, Chase-Lev orders, proposal_sink, pending_entity), vol05-plan-2.json (outline,
failpoints, stress matrix, gate) and the verdict build/design/vol05-judges.json.

## Open issues

- Record creation from observation: spawn and despawn are not written back; a Fabric reopen would have to let the boundary create records (ADR-0027).
- Cross-compiler and cross-platform determinism (D2 and above) are Volume 17 (SL-ARCH-012, not written); Volume 05 records clang versus gcc only.
- Windows and macOS platforms (ADR-0006, Volume 26), windowing and real input devices (Volumes 06 and 07) are absent; the runtime is headless.
- A shared job system across runtimes (editor plus preview) is a named Volume 06 seam; each runtime owns its job system.
- Wall-time performance is unmeasured as a gate (Volume 16); a shared 4-core machine makes timing floors flaky.
- Episodes 081-100 stay Demonstrated until 021-040 are Accepted (ADR-0021).
- Code reconciliation (WO-100): every entry of the WO-076 to WO-099 deviation files is recorded above; the code, test and tooling changes WO-100 listed here were made by WO-109 (docs/arch/deviations/vol05/WO-109.md), with the start-unwind limit recorded at ARCH3-R025.
- Wall time: `just runtime-demo` takes about 500 s and `just runtime-determinism` about 820 s in this environment; WO-101 places them in the gate composition and check-full, never in `fast`.

## Change log

| Version | Date | Change |
| :---- | :---- | :---- |
| 0.1.0 | 2026-10-20 | First draft before any Volume 05 code (WO-075), from the panel verdict build/design/vol05-judges.json. |
| 0.2.0 | 2026-10-22 | Reconciled with the Volume 05 code (WO-100): "Code reconciliation (WO-100)" records the 312 entries of the WO-076 to WO-099 deviation files; `limits` gains 20 rows and marks 14 `"bound": false`; `gate_evidence` gains hashes.json; error_codes, failpoints and thread_safety prose corrected; every contract table bound by `just runtime-contract-lock` and schemas/runtime/contract.lock.json; code still to change listed as open issues; nothing retired, rules unchanged except the R053 schema path. |
| 0.2.1 | 2026-10-22 | Volume 05 follow-ups (WO-109): the WO-100 open issues are fixed in code (layout digest fields, faulted-frame events, the baseline after the first host frame, a restore rewinds the logical tick and last runs, set-that-adds signals, last_run_ticks in the system scheduler, RFP5 and RFP8 site strings, sim_fixture warm-up, the runtime-tidy map, two cppcheck notes, the hashes.json schema and demo-hash golden) and removed from Open issues; the reconciliation lines they made stale are corrected; ARCH3-R025 records the start-unwind test limit; status unchanged. |
