---
id: ADR-0028
title: "Runtime lifecycle, recovery, swap protocol and the P0 gate"
status: Proposed
date: 2026-10-20
decision_refs: []
supersedes: []
---
# ADR-0028: Runtime lifecycle, recovery, swap protocol and the P0 gate

## Context

Episode 081 asks for startup, init, run, suspend, shutdown and recovery; the runtime runs games, editor previews and
headless jobs, in-process or sandboxed beside the editor (master plan section 2.3). Live updates swap only at a
safe frame or tick boundary, and allocator, scheduler and runtime ABI changes need an isolated runtime or a restart
(master plan section 2.5). Gate 100 is a multithreaded ECS simulation with diagnostics and reproducible tests (master
plan section 4.3). ADR-0021 caps Volume 05 at Demonstrated until episodes 021-040 are Accepted.

## Decision

- The runtime MUST follow the SL-ARCH-003 `rt_states` table; lifecycle calls run on the owner thread and request_* calls from any thread take effect at end_frame (master plan section 2.5).
- Startup MUST be all-or-nothing, and the host MUST pump frames through `run_frame` (no internal loop) (master plan section 2.3).
- Suspend MUST freeze the clocks: no tick accrues and the first delta after resume is 0 (master plan, Volume 05, episode 081).
- A faulted frame MUST NOT be played back; recovery is `stop` by default and `restore_snapshot` bounded by max_recoveries in tests and the gate (master plan, Volume 05, episodes 081 and 098).
- `request_swap` MUST apply only at end_frame with a pre-swap snapshot and a host health check that rolls back on failure; allocator, scheduler, component layout and runtime ABI classes return restart_required, keyed by `runtime_abi_digest` (master plan section 2.5).
- Two runtimes in one process MUST be independent, each with its own job system and instance id (master plan section 2.3).
- Gate 100 MUST be headless: `sl_runtime_demo` with a fixed CLI, evidence under --evidence-dir only, D1 proven, observations committed by the host (master plan section 4.3; ADR-0026; ADR-0027).
- Episodes 081-100 MAY reach Demonstrated and MUST NOT reach Accepted before 021-040 are Accepted (ADR-0021).
- `runtime-gate` MUST join `check` after fabric-gate and the sanitizer and determinism runs MUST join `check-full`, ratcheted only by WO-101 (master plan section 4.3).

## Consequences

- WO-096 (lifecycle), WO-099 (demo), WO-100 (conformance), WO-101 (release and ratchet).
- Seams for Volume 19 (swap, ABI fingerprint, isolated dev runtime) and Volume 06 (host-pumped frames, preview sink).
- No SDL3 or window in Volume 05 (ADR-0002).

## Alternatives

- Silent restore as the production default: rejected, it hides defects in shipped games.
- An internal frame loop: rejected, Volume 06 must own the event loop and the Wayland main thread.
- A windowed gate: rejected, a headless gate is reproducible under sanitizers and adds no dependency.
