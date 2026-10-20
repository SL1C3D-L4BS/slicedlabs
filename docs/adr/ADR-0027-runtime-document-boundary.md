---
id: ADR-0027
title: "Runtime side of the document boundary"
status: Proposed
date: 2026-10-20
decision_refs: []
supersedes: []
---
# ADR-0027: Runtime side of the document boundary

## Context

Nothing in Execution mutates authoritative data directly; every write enters through the transaction layer (master
plan sections 2.1 and 2.2). SL-ARCH-001 gives the runtime a read-only `document_projection` and
`propose_from_observation`, which sets paths of live records only (ARCH1-R60 to R62; machine fact:
engine/fabric/include/sl/fabric/boundary.hpp). `tx_options` defaults to durability fsync and commit_policy
reject_stale, and `rebase_if_commutes` exists (machine fact: engine/fabric/include/sl/fabric/transaction.hpp).
`fabric-surface-check` R-BND-INCLUDE checks direct includes under engine/runtime (machine fact). A projection copies
its records in make() and its snapshot's store must outlive it (machine fact).

## Decision

- The runtime MUST read only a host-supplied `document_projection` and MUST NOT name `transaction_manager` or `workspace` in engine/runtime/include or src (master plan section 2.1).
- Write-back MUST leave only as one coalesced patch per frame from `propose_from_observation` with epistemic class observed, handed at end_frame to an injected `link::proposal_sink`; the host commits with `rebase_if_commutes` and its own durability, so no fsync runs on the frame thread (master plan section 2.4; ADR-0017).
- The runtime actor MUST be a service actor with host-supplied write:<domain> grants (ADR-0017).
- Echoes MUST be suppressed: values equal to the projection are not proposed and rebase writes do not stamp change ticks (master plan section 2.1).
- A new revision MUST apply as a diff rebase at end_frame keeping the handles of unchanged records; at most two projections are held and their bytes are reported as external memory (master plan section 2.5; ADR-0023).
- Spawn and despawn MUST NOT be observed back; component types are document-bound (one binding to a frozen schema type) or transient (master plan section 2.2, row 03).
- Direct sl/fabric includes MUST follow the two scoped allow-lists of SL-ARCH-003 (library versus testing and tools) (master plan section 3, F5).

## Consequences

- WO-097 builds the link and testing::fabric_host; WO-099 commits gate observations through the host.
- Volume 05 adds no schema module; the gate registers content.scene types in code through public schema.hpp.
- Record creation from observation is an open issue for a later Fabric reopen.

## Alternatives

- Commit inside the observe phase through the transaction manager: rejected, fsync and head snapshots on the frame thread.
- Full re-hydration on every revision: rejected, it destroys handles and transient state after each observation commit.
- Let the runtime include workspace.hpp: rejected, it puts the mutation capability one call away.
