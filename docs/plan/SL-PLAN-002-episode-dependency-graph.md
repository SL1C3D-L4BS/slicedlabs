---
id: SL-PLAN-002
kind: PLAN
title: "Episode Dependency Graph"
status: Draft
version: 0.1.0
owner: founder
episodes: [018]
depends_on: []
supersedes: []
decisions: [ADR-0004]
---
# Episode Dependency Graph

## Purpose

This document fixes the hard build order between the 42 volumes and records why later volumes still shape early
decisions (master plan section 4.2). It supports the public roadmap of SL-EP-018: capability milestones and
evidence-based release gates. The graph itself is generated from the registry, not drawn by hand.

## Status and decisions

Draft, owner founder, who approves it. The graph lives in `registry/views/volume-graph.mmd`, written by
`just registry-views` from `depends_on_volumes` in `registry/SL-REG-002-episodes.json` (WO-011). Tooling is
stdlib-only (ADR-0004). The companion views are `registry/views/volumes.md` (one row per volume) and
`registry/views/episodes.csv` (one row per episode with status and Kohi reference).

## Normative text

- PLAN2-R01: The volume graph MUST be the Mermaid flowchart in `registry/views/volume-graph.mmd`, generated from SL-REG-002 and never edited by hand (master plan section 6.3, rule 1).
- PLAN2-R02: An episode MUST NOT reach Accepted before the episodes it depends on, including those of the volumes it depends on in section 4.2, are Accepted, except under a recorded waiver (master plan sections 4.2 and 6.2; ADR-0003 for SL-EP-041 to SL-EP-048).
- PLAN2-R03: The graph is the minimum hard order, so a later volume MAY constrain an earlier one: collaboration (volume 18) constrains identity design (volume 04), and replay (volume 17) constrains scheduling and clocks (volume 05) (master plan section 4.2).
- PLAN2-R04: Constraints from later volumes MUST be written into SL-ARCH documents before the early volumes freeze (master plan section 4.2).
- PLAN2-R05: A change to a volume dependency MUST go through the registry and a recorded decision, not through this document (master plan section 6.3, rule 3).

The hard order, in the words of master plan section 4.2:

- Part 1 foundations: volume 01 feeds 03, 03 feeds 04, 04 and 02 feed 05.
- Part 2 visible engine: 04 and 05 feed 06; 05 feeds 07; 07 feeds 08; 08 feeds 09; 04 and 09 feed 10.
- Part 3 content: 10 feeds 11, 12, 13 and 14; 05 feeds 15.
- Part 4 intelligence and Part 5 Early Access: 05 feeds 16, 16 feeds 17 and 21; 04 feeds 18, 18 feeds 23; 06 feeds 19; 19 feeds 20 and 24; 21 feeds 22; 22 and 24 feed 25.
- Parts 6 and 7 follow the same rule: every edge is in the generated graph and in the registry.

The generated graph is `registry/views/volume-graph.mmd` (embedded by reference so that it cannot drift from the registry).

## Measures and enforcement points

- PLAN2-R01: `just registry-views-check` regenerates all views in memory and fails with R-VIEW-DRIFT on any difference.
- PLAN2-R01: `just plan-check` requires this document to reference `registry/views/volume-graph.mmd`, the file to exist and to equal the registry-generated graph (R-PLAN-GRAPH).
- PLAN2-R02: `just registry-check` enforces the graph against section 4.2 (R-VOLGRAPH) and the dependency order of episodes (R-DEP-ORDER).
- PLAN2-R03 and PLAN2-R04: review by the founder; the SL-ARCH documents that carry the constraints do not exist yet (Open issues).

## Traceability

Master plan sections 4.1, 4.2, 6.2 and 6.3; SL-EP-018 (public roadmap); SL-EP-041 and SL-EP-048 (waiver, ADR-0003);
SL-REG-002 `volumes[].depends_on_volumes`; work order WO-011.

## Open issues

- The SL-ARCH documents that record the "later volumes shape early decisions" constraints are not written in this slice.
- `registry/views/episodes.csv` carries lifecycle status, so every `ep-advance` makes the views stale until `just registry-views` is run again.

## Change log

| Version | Date | Change |
| :---- | :---- | :---- |
| 0.1.0 | 2026-10-17 | First draft (WO-011). |
