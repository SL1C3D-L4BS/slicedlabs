---
id: ADR-0024
title: "Concurrency model and sanitizer policy"
status: Proposed
date: 2026-10-20
decision_refs: []
supersedes: []
---
# ADR-0024: Concurrency model and sanitizer policy

## Context

The locked runtime model is an archetype ECS with a job system with declared read/write access (master plan section
1.5, row Runtime model). The job system and ECS must be TSan-clean at the release gate (master plan section 4.3).
ThreadSanitizer does not model standalone fences, and a planted race was reported with exit 66 both in the builder
sandbox and inside a nested bwrap with the wo-verify jail flags (machine fact, 2026-10-20). The sl::log logger
dispatches under one mutex and keeps the current correlation thread-local (machine fact:
engine/core/include/sl/core/log.hpp).

## Decision

- Runtime synchronization MUST use only std::atomic, std::mutex, std::condition_variable, std::atomic wait and notify, std::latch, std::barrier and std::counting_semaphore; standalone fences, volatile synchronization, inline assembly and raw futex calls MUST NOT appear (machine fact: TSan does not model fences).
- A memory order weaker than seq_cst MUST name its pairing operation in a `// pairs:` comment (master plan section 6.1, review).
- Job and system bodies MUST be noexcept by type and report errors through `job_context::fail` (ADR-0009).
- Waits MUST help (run other ready jobs) and spin at most spin_limit before parking (master plan, Volume 05, episode 087).
- `locked_deque` is the reference queue; the Chase-Lev `ws_deque` (seq_cst on contended operations) MUST pass the same contract suite and MAY become the default only after the steal invariants pass under TSan (master plan, Volume 05, episode 088).
- Background jobs MUST NOT touch world state; their results enter as recorded external inputs at begin_frame (ADR-0026).
- `runtime-san` MUST be fail-closed for builders and wo-verify: a sanitizer that cannot start, or any report, is a failure (machine fact: TSan runs in the jail).
- Steady-frame code MUST report through counters; log events only at boundaries, faults and lifecycle transitions (machine fact: the logger serializes under a mutex).

## Consequences

- `neg-runtime-race` (WO-082) proves TSan detection; `just runtime-san tsan flt` (WO-076) is the jail smoke test.
- runtime-lint rules R-RT-FENCE, R-RT-ORDER, R-RT-NOEXCEPT and R-RT-THROW (WO-077).
- Every builder order runs `runtime-san` for its area; full sanitizer workflows run at the wave gate and in check-full.

## Alternatives

- Fibers or coroutine jobs: rejected for Volume 05; a plain function keeps TSan and stack traces simple (named seam).
- Mark TSan "unproven here" in the jail: rejected, the machine fact shows it runs and the gate claim needs it.
- Lock-free deque as the only queue: rejected, the gate would bet on unproven code.
