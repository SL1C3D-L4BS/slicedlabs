---
id: ADR-0026
title: "Volume 05 determinism contract (D1) and replay seams"
status: Proposed
date: 2026-10-20
decision_refs: []
supersedes: []
---
# ADR-0026: Volume 05 determinism contract (D1) and replay seams

## Context

Determinism is a contract per subsystem proven by tests (master plan section 1.5, row Determinism), and replay
constrains scheduling and clocks (master plan section 4.2). Volume 17 (episodes 321-340) owns determinism classes,
recording, replay and divergence tooling, and SL-ARCH-012 is not written. Compilers may fold libm calls differently,
and floating-point contraction changes results between translation units (machine fact: -ffp-contract defaults
differ between clang and gcc). Correlation ids come from a process-wide counter (machine fact:
engine/core/include/sl/core/log.hpp).

## Decision

- In deterministic mode the per-tick state hash, event order and observation patch bytes MUST be pure functions of the initial projection, seed, recorded external inputs and configuration, on the same binary, for any worker count: class D1 (master plan section 4.2).
- Every merge of per-job buffers MUST go through one `det::merge_ordered` over `ordered_key {tick, phase, system_index, chunk_index, local_seq}` (master plan section 1.5, row Runtime model).
- `serial_canonical` execution MUST exist and be the oracle that parallel deterministic runs equal (master plan section 4.2).
- Time MUST be integer nanoseconds with a rational fixed step; randomness MUST come from `rng_stream` over std::mt19937_64 with engine-defined mappings and no std distributions (ADR-0021).
- State hashing MUST use the canonical encoding of SL-ARCH-003 (little-endian, width-tagged, -0.0 as +0.0, one quiet NaN) over SHA-256 (ADR-0012 pattern).
- Correlation ids MUST stay diagnostic: never in hashed or snapshotted state, an ordered_key or an event order (machine fact: process-wide counter).
- `sl_runtime` MUST carry -ffp-contract=off as INTERFACE and PRIVATE option (machine fact: contraction differs by compiler).
- Cross-compiler equality MUST be recorded, not gated, in Volume 05; Volume 17 (SL-ARCH-012) owns stronger classes (master plan, Volume 17).

## Consequences

- Seams left for Volume 17: determinism_mode, ordered_key, tick and fixed step, rng_stream, state hash with breakdown,
  snapshot_contributor and SLRS/1, record_sink for external inputs (SL-ARCH-003 `seams`).
- Gate systems use + - * / and sqrt only; hash-matrix.json records clang versus gcc.
- RT-INV-SCHED-06, RT-INV-STRESS-05 and `just runtime-determinism` prove D1.

## Alternatives

- A committed cross-compiler golden hash in check-full: rejected, libm folding would block the gate on a Volume 17 question.
- Lockstep-style fixed-point simulation: rejected, ADR-0021 forbids fixed-point types before Volume 02.
- Order merges by job completion: rejected, it is the nondeterminism the contract removes.
