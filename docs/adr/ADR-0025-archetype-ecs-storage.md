---
id: ADR-0025
title: "Archetype ECS storage and structural change"
status: Proposed
date: 2026-10-20
decision_refs: []
supersedes: []
---
# ADR-0025: Archetype ECS storage and structural change

## Context

The runtime model is an archetype ECS (master plan section 1.5, row Runtime model). Episodes 091-095 need
generational handles, data-oriented storage, cached queries, deterministic structural change and change tracking
(master plan, Volume 05). The hot-reload protocol classes object layout and runtime ABI as restart-only changes
(master plan section 2.5). An ECS handle is never an identity and never appears in a schema (ARCH1-R08 of
SL-ARCH-001; master plan section 2.2, row 03).

## Decision

- An entity MUST be {index u32, generation u32}, generation from 1, the zero value null, a saturated slot retired forever; freed indices MUST be reused FIFO and only while at least 1024 are free (master plan, Volume 05, episode 091).
- Storage MUST be 16 KiB chunks aligned to 64 with 64-byte-aligned SoA columns and u32 added and changed tick columns, archetypes kept in creation order, at most 32 components per archetype (master plan, Volume 05, episode 092).
- Structural change MUST happen only on the owner thread outside phases or through per-job command buffers played back by ordered_key at sync boundaries, with `pending_entity` resolved at flush in key order (ADR-0026).
- A structural change MUST be atomic under allocation failure: destination space is reserved before any row moves (ADR-0023).
- Chunk size, handle layout and the component layout digest MUST be inputs of `runtime_abi_digest`, and a change to them is restart_required (master plan section 2.5).
- Change ticks MUST be u32 with a boundary wrap sweep, and queries filter changed<T> and added<T> against an explicit since tick (master plan, Volume 05, episode 095).

## Consequences

- WO-083 (entities), WO-088 (storage), WO-090 (queries), WO-091 (command buffers) and WO-093 (change tracking).
- Iteration order (archetype creation, chunk, row) is part of the determinism contract (ADR-0026).
- Snapshots capture the physical layout so restore reproduces iteration order (WO-095).

## Alternatives

- Sparse-set storage: rejected, the locked decision names archetypes.
- 64 KiB chunks: rejected, 16 KiB keeps small archetypes dense on a 4-core desktop and matches common L2 budgets.
- LIFO index reuse: rejected, it recycles handles immediately and weakens stale-handle detection.
