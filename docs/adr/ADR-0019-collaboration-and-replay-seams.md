---
id: ADR-0019
title: "Collaboration and replay seams frozen into patch v1 and the revision format"
status: Proposed
date: 2026-10-18
decision_refs: []
supersedes: []
---
# ADR-0019: Collaboration and replay seams frozen into patch v1 and the revision format

## Context

Collaboration (Volume 18) constrains identity and replay (Volume 17) constrains clocks; both constraints are written
into SL-ARCH documents before the early volumes freeze (master plan section 4.2). Volume 18 needs structured diffs,
three-way and scene-aware merges, locks, concurrent validation, real-time operation sync and offline reconciliation
(episodes 344-354). Volume 17 needs state hashing, schema-aware snapshot restore and branching histories (episodes
327, 328, 333). Remote pieces add scale, never authority (master plan section 2.3).

## Decision

- Patch v1 MUST carry a version, a base revision, ordered ops, and meta {label, undo group, coalesce key, idempotency key}; ops address values by field number, map key or element id, never by list position (master plan section 4.2).
- Identity-bearing collections MUST be `oset` (element id plus fractional order key); a plain list is atomic and replaced whole; concurrent inserts at an equal order key converge by element id (master plan section 4.2; episodes 345 and 347).
- Every patch MUST expose a footprint (record, path, read, write, create or remove), and `commutes(a, b)` MUST be symmetric and sound: when it is true both application orders give the same root (master plan section 4.2; episode 350).
- The validation pipeline MUST call a `lock_authority` at validation and again under the commit mutex; `null_lock_authority` allows all until Volume 18 (master plan section 4.2; episode 349).
- A `merge_driver` registry keyed by type id MUST exist with a default field-level three-way driver over footprints and `expect`; other drivers are Volume 18 work (master plan section 4.2; episode 345).
- A retried commit with the same (actor, idempotency key) MUST return the original receipt and create no revision (master plan section 4.2; episode 352).
- Wall clock and id minting MUST be injected (`clock`, `id_source`); same base, same patch and same injected values give an identical revision oid, and replaying tx records from a base reproduces every revision oid (master plan section 4.2; episode 327).
- Revision pins and per-revision schema_set MUST exist in Volume 04 (master plan section 4.2; episode 328).
- The collaboration service is not an authority by assumption: whether it runs this transaction manager or a Rust port is undecided (master plan section 2.3; section 1.5, row Web services).

## Consequences

- WO-043 (patch algebra, commutes, rebase, oset convergence), WO-050 (idempotency, lock hook, observers), WO-062
  (replay tests RPL-01..03). The named seams S1-S16 in SL-ARCH-002 Open issues list what stays for Volumes 17 and 18.
- A new op kind needs a patch minor version; the decoder rejects unknown op kinds under the same major.

## Alternatives

- Positional list ops (D1 draft): rejected, a positional patch frozen into history cannot be three-way merged
  without index transformation.
- Defer every hook to Volume 18: rejected, master plan section 4.2 requires the constraint written before Volume 04
  freezes, and the patch format is stored in history.
