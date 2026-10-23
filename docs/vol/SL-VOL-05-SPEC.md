---
id: SL-VOL-05-SPEC
kind: VOL
title: "Volume 05 Specification: Native runtime, memory, jobs, ECS"
status: Draft
version: 0.2.0
owner: founder
episodes: [81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100]
depends_on: [SL-GOV-001, SL-ARCH-001, SL-ARCH-003]
supersedes: []
decisions: [ADR-0021, ADR-0022, ADR-0023, ADR-0024, ADR-0025, ADR-0026, ADR-0027, ADR-0028]
max_lines: 120
---
# SL-VOL-05-SPEC: Volume 05 Specification

## Purpose

Volume 05 builds the native runtime that runs games, simulations, editor previews and headless jobs: runtime core,
memory system, job scheduler and the archetype ECS (master plan, Volume 05 block; Kohi #004-#009 and #067 as a guide
only). This document holds one record per episode 081-100 (master plan section 6.1): purpose, the work orders that
trace it, exactly one primary work order whose acceptance recipe is the episode's acceptance, the seam it leaves, a
work-bound performance case and release applicability. It is a view: the registry SL-REG-002 stays authoritative
(master plan section 6.3, rule 1). The architecture is SL-ARCH-003, with the document side of the boundary in
SL-ARCH-001.

## Status and decisions

Status is Draft (version 0.2.0), written with SL-ARCH-003 by WO-075 before any Volume 05 code (master plan,
Authoring order) and given the seams as shipped by WO-101. A record's `title` and `purpose` are the SL-REG-002 `title` and `summary`, verbatim; `work_orders`
repeats the `Traces:` lines of `work-orders/WO-075.md` to `WO-101.md` and of WO-072 (ADR-0021), which traces 081 and 100. ADR-0021 lets Volume 05 start before Volume 02
and caps 081-100 at Demonstrated until 021-040 are Accepted; builders change no status. Binding decisions: ADR-0021
to ADR-0028. Gate 100 is the P0 gate "runtime" of master plan section 4.3.

## Normative text

- VOL5-R01: Each episode 081-100, and no other, MUST have one record with purpose, work orders, primary work order, acceptance recipe, Kohi reference, seam, performance case and release applicability (master plan section 6.1).
- VOL5-R02: A record's title and purpose MUST NOT diverge from the registry (master plan section 6.3, rules 1 and 2).
- VOL5-R03: `work_orders` MUST equal the set of work orders whose `Traces:` line names the episode, in both directions (master plan section 6.3, rule 4).
- VOL5-R04: Each record MUST name exactly one `primary_wo`, a member of `work_orders` whose acceptance list contains the record's `acceptance` command; a work order MUST be primary for at most one episode (master plan section 6.3, rule 4; ADR-0020 pattern).
- VOL5-R05: The supporting work orders (WO-072 lifecycle gates, WO-075 documents, WO-076 scaffold, WO-077 guardrails, WO-091 command buffers, WO-097 Fabric link, WO-099 gate demo, WO-100 conformance) MAY trace episodes and MUST NOT be primary (master plan section 6.3, rule 4).
- VOL5-R06: Episodes 081-100 MAY reach Demonstrated on recorded evidence and MUST NOT reach Accepted before episodes 021-040 are Accepted (master plan section 6.2; ADR-0021).
- VOL5-R07: A seam MUST state what the slice leaves undone, so no later work reads absence as completion (master plan section 6.3, rule 3).
- VOL5-R08: The performance case MUST be work-bound (allocations, visits, steals, log events), not wall time, until Volume 16 provides benchmarks (master plan section 6.1, acceptance suite).
- VOL5-R09: Every acceptance recipe MUST be a `just` recipe whose output stays under build/ (master plan section 6.1; ADR-0028).

```sl-data
{
 "volume": "SL-VOL-05",
 "episodes": [
  {"episode": "SL-EP-081", "title": "Runtime lifecycle states", "purpose": "startup, init, run, suspend, shutdown, recovery", "work_orders": ["WO-072", "WO-075", "WO-076", "WO-096", "WO-109"], "primary_wo": "WO-096", "acceptance": "just runtime-test life", "kohi_ref": "none", "seam": "V06-1 host-pumped frames, V19-1 swap protocol, V19-2 runtime_abi_digest, V19-3 isolated dev runtime named only (in_process); production recovery default is stop; the first recovery baseline is taken at the end of the first host frame, so a fault in that frame ends in recovery_failed; a restore rewinds the logical tick and last runs while wall time goes on; the startup RFP0 sweep covers visits 1 and 3, visit 2 (the std::pmr page list) aborts by design (ARCH3-R025)", "performance": "startup and shutdown proxies: threads joined = workers, tracked bytes at shutdown = 0, one log event per transition", "release": "Foundation; Linux desktop, headless; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-082", "title": "Platform abstraction boundary", "purpose": "windowing, timing, input, files, threads", "work_orders": ["WO-075", "WO-078"], "primary_wo": "WO-078", "acceptance": "just runtime-test plat", "kohi_ref": "Platform layer (Windows, Linux) (#004–#005)", "seam": "headless only: no window or input device (V06-4 input routing, V26-1 other OSes); file_access over the faulty vfs, durability simulated; threads are pthreads behind a pimpl, Linux only (Windows unverified, ADR-0006); atomic_write visits no RFP8", "performance": "one clock sample per frame; no OS call outside platform_*.cpp (runtime-lint R-RT-PLATFORM)", "release": "Foundation; Linux desktop, headless; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-083", "title": "Memory allocation contracts", "purpose": "alignment, lifetime, ownership, instrumentation", "work_orders": ["WO-075", "WO-079"], "primary_wo": "WO-079", "acceptance": "just runtime-test mem", "kohi_ref": "Memory subsystem (#007)", "seam": "global operator new untracked (V16-3); external memory reported through report_external, not tracked (fabric.projection, gpu.*); tracked blocks carry a 16-byte header and are filled, not poisoned, by tracking_resource", "performance": "Release tracking costs at most one relaxed atomic add per counter per call", "release": "Foundation; Linux desktop, headless; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-084", "title": "Foundational allocators", "purpose": "arena, pool, stack, general-purpose adapters", "work_orders": ["WO-075", "WO-081"], "primary_wo": "WO-081", "acceptance": "just runtime-test alloc", "kohi_ref": "Memory subsystem (#007)", "seam": "no NUMA or huge-page arenas; frame arenas per worker only; the allocators visit no failpoint and check their owner thread in Debug", "performance": "upstream allocations per steady frame = 0 after 10 warm-up frames (RT-INV-FRAME-04)", "release": "Foundation; Linux desktop, headless; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-085", "title": "Allocation diagnostics", "purpose": "allocation sites, leaks, fragmentation, peaks", "work_orders": ["WO-075", "WO-085"], "primary_wo": "WO-085", "acceptance": "just runtime-test diag", "kohi_ref": "Memory subsystem (#007)", "seam": "profiler capture and UI are Volume 16 (V16-1); sites only in Debug and sanitizer builds; leaks are taken at shutdown only", "performance": "a report read never blocks allocation (RT-INV-DIAG-04); one runtime/mem event per 600 frames", "release": "Foundation; Linux desktop, headless; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-086", "title": "Job-system prototype", "purpose": "submission, worker queues, dependencies, completion", "work_orders": ["WO-075", "WO-082"], "primary_wo": "WO-082", "acceptance": "just runtime-test job", "kohi_ref": "Job system (#067)", "seam": "no fibers or continuations; no io job class; one job system per runtime (shared mode is a Volume 06 seam); background results arrive behind earlier background jobs; only workers and the owner help in wait", "performance": "jobs run = jobs submitted; inline runs only for jobs without predecessors", "release": "Foundation; Linux desktop, headless; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-087", "title": "Synchronization primitives", "purpose": "fences, semaphores, atomic coordination", "work_orders": ["WO-075", "WO-080"], "primary_wo": "WO-080", "acceptance": "just runtime-test sync", "kohi_ref": "Job system (#067)", "seam": "gpu_fence sits beside cpu_fence in Volume 07 (V07-4)", "performance": "spin iterations bounded by spin_limit (64) before park", "release": "Foundation; Linux desktop, headless; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-088", "title": "Work stealing and scheduling", "purpose": "throughput, latency, locality, starvation", "work_orders": ["WO-075", "WO-086"], "primary_wo": "WO-086", "acceptance": "just runtime-test steal", "kohi_ref": "Job system (#067)", "seam": "no core pinning or thread priorities; work_stealing is the runtime default since WO-096 (locked_deque stays the reference) and its STEAL invariants run under TSan in check-full via runtime-san tsan", "performance": "scheduler counters: steals per job, failed steals, parks, wakes, injection polls (recorded)", "release": "Foundation; Linux desktop, headless; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-089", "title": "Frame and simulation clocks", "purpose": "wall, frame, fixed-step and logical time", "work_orders": ["WO-075", "WO-084", "WO-109"], "primary_wo": "WO-084", "acceptance": "just runtime-test clock", "kohi_ref": "Job system (#067)", "seam": "fixed-step contract and rng streams are the Volume 17 seams V17-2 and V17-4; wall clock only for log stamps", "performance": "zero drift: exactly 216000 ticks over 3600 s at 60 Hz; dropped ticks and dropped_ns counted", "release": "Foundation; Linux desktop, headless; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-090", "title": "Frame scheduler", "purpose": "ordered phases, task dependencies, sync boundaries", "work_orders": ["WO-075", "WO-089", "WO-109"], "primary_wo": "WO-089", "acceptance": "just runtime-test frame", "kohi_ref": "Job system (#067)", "seam": "present is a no-op slot (V07-1); retire_after for GPU retirement (V07-2)", "performance": "phase graphs built once; steady frames allocate 0 upstream; critical path recorded", "release": "Foundation; Linux desktop, headless; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-091", "title": "ECS entity lifecycle", "purpose": "creation, destruction, generations, stale handles, ID mapping", "work_orders": ["WO-075", "WO-083", "WO-091", "WO-097", "WO-109"], "primary_wo": "WO-083", "acceptance": "just runtime-test ent", "kohi_ref": "none", "seam": "no hierarchy or relations (entity fields inside components); spawn and despawn never written back to the document; a pending_entity handle is not itself a deterministic value, only the entities it resolves to are", "performance": "O(1) create, destroy and is_alive; FIFO reuse only past 1024 free indices", "release": "Foundation; Linux desktop, headless; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-092", "title": "Archetype component storage", "purpose": "data-oriented layouts", "work_orders": ["WO-075", "WO-088", "WO-091", "WO-109"], "primary_wo": "WO-088", "acceptance": "just runtime-test store", "kohi_ref": "none", "seam": "transforms and vectors are opaque payloads until Volume 02 (ADR-0021, V02-1); sizes are runtime ABI (restart class); the layout digest walks reflected fields", "performance": "rows moved per structural change and chunk occupancy recorded; RFP0 sweep leaves the world unchanged", "release": "Foundation; Linux desktop, headless; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-093", "title": "ECS queries", "purpose": "filters, iteration, caching, structural changes", "work_orders": ["WO-075", "WO-090"], "primary_wo": "WO-090", "acceptance": "just runtime-test query", "kohi_ref": "none", "seam": "no query language or scripting front end", "performance": "archetypes visited per new archetype = new archetypes only (RT-INV-QUERY-02)", "release": "Foundation; Linux desktop, headless; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-094", "title": "System scheduling contracts", "purpose": "declared reads/writes, ordering, parallelism", "work_orders": ["WO-075", "WO-094", "WO-109"], "primary_wo": "WO-094", "acceptance": "just runtime-test sched", "kohi_ref": "none", "seam": "systems are C++ callables; Luau and graph systems are Volume 19", "performance": "observed overlap > 1 for disjoint systems (RT-INV-SCHED-02); serial_canonical is the oracle", "release": "Foundation; Linux desktop, headless; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-095", "title": "Change tracking", "purpose": "modified components, structural changes, invalidation signals", "work_orders": ["WO-075", "WO-093", "WO-097", "WO-109"], "primary_wo": "WO-093", "acceptance": "just runtime-test chg", "kohi_ref": "none", "seam": "no persistent change journal; ticks are u32 with a wrap sweep", "performance": "changed<T> visits only rows with a later tick (counter)", "release": "Foundation; Linux desktop, headless; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-096", "title": "Runtime reflection", "purpose": "type, field, method and serialization metadata for tools", "work_orders": ["WO-075", "WO-087", "WO-109"], "primary_wo": "WO-087", "acceptance": "just runtime-test refl", "kohi_ref": "none", "seam": "schema-aware migration of runtime types is Volume 17 (V17-8); the inspector consumes the dump in Volume 06 (V06-5)", "performance": "encode cost linear in fields (visit counter)", "release": "Foundation; Linux desktop, headless; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-097", "title": "Typed runtime events", "purpose": "validated dispatch, ordering, tracing", "work_orders": ["WO-075", "WO-092", "WO-109"], "primary_wo": "WO-092", "acceptance": "just runtime-test evt", "kohi_ref": "Events and input (#008–#009)", "seam": "event recording and causality for replay are Volume 17 (V17-6); in-process export only", "performance": "events per frame and overflow counted; delivery O(events)", "release": "Foundation; Linux desktop, headless; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-098", "title": "Runtime snapshots", "purpose": "selected state for tests, recovery, replay", "work_orders": ["WO-075", "WO-095", "WO-109"], "primary_wo": "WO-095", "acceptance": "just runtime-test snap", "kohi_ref": "none", "seam": "same-ABI, same-schema restore only; deltas and schema-aware restore are Volume 17 (V17-8)", "performance": "snapshot bytes and take/restore visits recorded", "release": "Foundation; Linux desktop, headless; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-099", "title": "Concurrency and memory stress tests", "purpose": "scheduler, allocator and ECS invariants", "work_orders": ["WO-075", "WO-076", "WO-077", "WO-098", "WO-100", "WO-109"], "primary_wo": "WO-098", "acceptance": "just runtime-stress", "kohi_ref": "none", "seam": "runtime-soak runs random seeds outside every gate; wall time is recorded, not asserted; S5 sweeps RFP0 over a 2000-operation batch and RFP0 is armed at the memory root or around world operations only", "performance": "every stress file within 20 s Debug at scale 1; sanitizers at scale 0.1", "release": "Foundation; Linux desktop, headless; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-100", "title": "Gate the runtime foundation", "purpose": "multithreaded ECS simulation with diagnostics and reproducible tests", "work_orders": ["WO-072", "WO-075", "WO-097", "WO-099", "WO-100", "WO-101", "WO-109"], "primary_wo": "WO-101", "acceptance": "just acceptance-081-100", "kohi_ref": "none", "seam": "Demonstrated-capped until 021-040 are Accepted (ADR-0021); SL-ARCH-003 is InReview 0.3.0, not Approved; D1 only, same binary (clang vs gcc recorded, not gated); Linux only; headless, no window; wall time recorded, not a gate; runtime-demo and runtime-determinism are minutes long (see docs/episodes/081-100.md)", "performance": "runtime-gate and demo wall times recorded in docs/episodes/081-100.md", "release": "Foundation; Linux desktop, headless; Windows unverified (ADR-0006)"}
 ]
}
```

## Measures and enforcement points

- VOL5-R01 to VOL5-R05: `just plan-check` parses the `sl-data` block and applies R-PLAN-VOL05 (added by WO-075, the same checks as R-PLAN-VOL04): exactly episodes 081-100 in order, title and purpose against SL-REG-002, work orders against the `Traces:` lines both ways, one primary per record whose acceptance list holds the record's command, at most one episode per primary; `just neg-plan` proves each failure.
- VOL5-R06: `just ep-advance` (R-VOL-GATE) and `just registry-check` enforce the ADR-0021 cap.
- VOL5-R07 and VOL5-R08: founder review at WO-101, where `docs/episodes/081-100.md` records the observed numbers and wall times.
- VOL5-R09: `just wo-check` (acceptance grammar) and runtime-lint R-RT-OUTPUT.
- Every document: `just doc-check`; the register entry: `just registry-docs-check`.

## Traceability

Master plan Volume 05 block, sections 4.2, 4.3 (gate episode 100), 6.1, 6.2 and 6.3; SL-REG-002 episodes 081-100;
SL-ARCH-003 and SL-ARCH-001 (boundary); work orders WO-075 to WO-101 (waves in `build/design/vol05-draft/WAVES.md`
until merged); ADR-0021 to ADR-0028; the panel verdict build/design/vol05-judges.json.

## Open issues

- Volume 05 depends on Volume 02 in the registry (`depends_on_volumes: [SL-VOL-02, SL-VOL-04]`); ADR-0021 allows the early start and caps acceptance, enforced by ep-advance R-VOL-GATE.
- The episodes have no intra-volume `depends_on` edges yet; the wave order of WAVES.md is a proposal the founder may apply as a registry amendment with an impact note (master plan section 6.3 rule 3).
- Windows (ADR-0006), windowing, real input devices and cross-compiler determinism are outside Volume 05 (seams of SL-ARCH-003).
- Wall time is not a gate (VOL5-R08); WO-101 recorded the numbers in `docs/episodes/081-100.md`.
- The SL-REG-003 rows for episodes 081-100 are a draft in `docs/episodes/081-100.md`; the founder close-out writes them (WO-101 edits no registry).

## Change log

| Version | Date | Change |
| :---- | :---- | :---- |
| 0.1.0 | 2026-10-20 | First draft with SL-ARCH-003 (WO-075). |
| 0.2.0 | 2026-10-22 | Seams as shipped (WO-101): the records of 081-086, 088, 091, 092, 099 and 100 gain what the code does and leaves undone; `work_orders` and `primary_wo` unchanged (WO-101 was already primary for SL-EP-100). |
