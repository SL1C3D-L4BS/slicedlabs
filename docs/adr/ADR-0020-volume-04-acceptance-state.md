---
id: ADR-0020
title: "Volume 04 acceptance state: Demonstrated cap, intra-volume order, founder-only status changes"
status: Proposed
date: 2026-10-18
decision_refs: []
supersedes: []
---
# ADR-0020: Volume 04 acceptance state: Demonstrated cap, intra-volume order, founder-only status changes

## Context

An episode is accepted only on evidence; Specified moves to InProgress when dependencies are accepted (master plan
sections 6 and 6.2). Volume 04 depends on Volume 03 (master plan section 4.2), and the registry records it as
`depends_on_volumes: [SL-VOL-03]` while every episode 061-080 has `depends_on: []` (machine fact:
`registry/SL-REG-002-episodes.json`). ADR-0003 caps 041-048 at Demonstrated until episode 020 is Accepted and does not
cover later volumes. So `ep-advance` would neither order nor cap Volume 04 today, and "accepted on evidence" is
ambiguous for it. The authoring order puts SL-ARCH-001, then SL-ARCH-002, before code (master plan, Authoring order).

## Decision

- Episodes 061-080 MAY reach Demonstrated on recorded evidence, and MUST NOT reach Accepted before Volume 03 episodes 041-060 are Accepted and SL-ARCH-001 and SL-ARCH-002 are Approved (master plan sections 4.2 and 6.2).
- SL-ARCH-001 and SL-ARCH-002 MUST become Approved only through `registry/SL-REG-001.approvals.json` with a matching sha256, written by the founder (master plan section 6.3 rule 1).
- No Volume 04 code work order MAY start before both documents are Draft and registered and the founder has flipped ADR-0010, ADR-0011, ADR-0012, ADR-0014, ADR-0015, ADR-0016 and ADR-0017 to Accepted (master plan, Authoring order; ADR-0003 does not cover this volume).
- After the document work order merges, the founder runs `ep-advance` Planned to Specified for all twenty episodes; each later move needs the episode's primary work order merged and its evidence recorded (master plan section 6.2).
- Builders MUST NOT change episode status, document status above InReview or ADR status (master plan section 6.3 rule 4).
- Proposed registry amendment (governed, applied by the founder with an impact note under master plan section 6.3 rule 3): 070 depends on 061, 062 and 068; 071 on 070; 073 on 072; 076 on 070; 077 on 076; 079 on 061-078; 080 on 079.

## Consequences

- `ep-advance` and `registry-check` need the cap rule for 061-080 (a governed change to the registry tooling,
  owned by the founder, not by a Volume 04 work order).
- SL-VOL-04-SPEC names exactly one primary work order per episode; `just plan-check` (R-PLAN-VOL04) verifies it.

## Alternatives

- Accept 061-080 as soon as their own tests pass: rejected, master plan section 6.2 needs accepted dependencies and
  Volume 03 is itself Demonstrated-capped.
- Extend ADR-0003 to Volume 04: rejected, ADR-0003 is a one-slice waiver and says so.
