---
id: ADR-0003
title: "Volume 03 code starts before its authoring-order prerequisites"
status: Proposed
date: 2026-10-17
decision_refs: []
supersedes: []
---
# ADR-0003: Volume 03 code starts before its authoring-order prerequisites

## Context

The master plan's authoring order requires SL-ARCH-001, 002, 003, 005 and 008 and SL-PLAN-003 before Volume 03 code
starts (master plan, Authoring order flowchart). Episodes 041-048 are Volume 03 code and are the first thing the
founder asked to build. Lifecycle: an episode reaches Accepted only on recorded evidence, after dependencies
are accepted (master plan section 6.2). Rule R-DEP-ACCEPTED, defined by the registry checks in WO-002, requires an
episode's dependencies to be Accepted before it is.

## Decision

Two waivers apply to this slice only.

- Waiver 1 (authoring-order waiver): Volume 03 code for episodes 041-048 MAY start before SL-ARCH-001, SL-ARCH-002, SL-ARCH-003, SL-ARCH-005 and SL-ARCH-008 exist (master plan, Authoring order flowchart).
- Waiver 2 (`R-DEP-ACCEPTED` waiver): episodes 041-048 MAY start before Volume 01 is Accepted (master plan section 6.2).
- Episodes 041-048 MAY reach Demonstrated but MUST NOT reach Accepted before episode 020 is Accepted (master plan section 6.2; plan of record, ADR-0003 row).
- SL-PLAN-003 and the Volume 01 documents SHOULD be written in this slice before the code is Accepted (master plan, Authoring order flowchart).

## Consequences

- The plan authors the Volume 01 governance documents and the registries in earlier waves of this slice, so the
  authoring order is satisfied for documents before the code is Accepted.
- `ep-advance` and the registry check treat 041-048 as Demonstrated-capped until episode 020 is Accepted (WO-012, WO-023).
- Master plan section 6.3 rule 3 forbids silent scope changes, so both waivers are stated here and the founder
  approves this ADR before they apply.

## Alternatives

- Wait for Volume 01 and the SL-ARCH documents before any code: rejected, the founder asked for the toolchain
  episodes first (plan of record, founder answers).
- Mark 041-048 Accepted early: rejected, evidence is required for Accepted (master plan section 6).
