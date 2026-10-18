---
id: SL-PLAN-004
kind: PLAN
title: "Vertical Slice Definitions"
status: Draft
version: 0.1.0
owner: founder
episodes: [010]
depends_on: []
supersedes: []
decisions: [ADR-0004]
---
# Vertical Slice Definitions

## Purpose

This document defines the six implementation phases P0 to P5 with their gates, and the first vertical slice that
proves the canonical model end to end (master plan section 4.3, "First vertical slice"). It supports SL-EP-010, the
product lifecycle: create, author, execute, debug, collaborate, test, release.

## Status and decisions

Draft, owner founder, who approves it. Phase ranges follow the volume ranges of SL-REG-002 (`registry/views/volumes.md`).
Each gate is a running product with recorded evidence, not a document (master plan section 4.3). The step-to-episode
mapping of the first slice is a proposal of this work order (WO-011) and is listed under Open issues.

## Normative text

- PLAN4-R01: The work is organised in phases P0 to P5, each ending at a gate episode: SL-EP-100, SL-EP-200, SL-EP-400, SL-EP-500, SL-EP-640 and SL-EP-840 (master plan section 4.3).
- PLAN4-R02: A gate MUST be a running product with recorded evidence, not a document (master plan section 4.3).
- PLAN4-R03: Gate SL-EP-640 MUST measure the Unreal parity matrix on the hardware lab for the first time (master plan sections 4.3 and 2.7).
- PLAN4-R04: The first vertical slice MUST pass only if every step goes through the canonical model with correct invalidation, transactional undo, crash recovery and verifiable provenance (master plan, "First vertical slice").
- PLAN4-R05: The first vertical slice targets the end of Volume 10, at the gate SL-EP-200 (master plan, "First vertical slice").

| Phase | Volumes | Episodes | Gate episode | Gate |
| :---- | :---- | :---- | :---- | :---- |
| P0 Foundation | 01-05 | 001-100 | SL-EP-100 | runtime |
| P1 Core loop | 06-10 | 101-200 | SL-EP-200 | asset pipeline |
| P2 Production | 11-20 | 201-400 | SL-EP-400 | 2D + 3D games |
| P3 Early Access | 21-25 | 401-500 | SL-EP-500 | EA release |
| P4 Unreal-class depth | 26-32 | 501-640 | SL-EP-640 | SL-BENCH parity |
| P5 Platforms + web + ecosystem | 33-42 | 641-840 | SL-EP-840 | Sliced Labs 1.0 |

First vertical slice, with the episodes that deliver each step (mapping per Open issues):

| Step | Action | Episodes |
| :---- | :---- | :---- |
| A | Create project | SL-EP-064, SL-EP-080 |
| B | Author scene | SL-EP-091, SL-EP-113 |
| C | Import asset | SL-EP-184, SL-EP-185 |
| D | Build DAG | SL-EP-047, SL-EP-072 |
| E | Run in editor and headless | SL-EP-081, SL-EP-101 |
| F | Inspect runtime trace | SL-EP-098, SL-EP-199 |
| G | Change one property | SL-EP-070, SL-EP-113 |
| H | Rebuild only affected outputs | SL-EP-073, SL-EP-187 |
| I | Undo, save, reopen, recover | SL-EP-071, SL-EP-076, SL-EP-116 |
| J | Package and verify provenance | SL-EP-055, SL-EP-067, SL-EP-192 |
| K | Archive evidence | SL-EP-200 |

## Measures and enforcement points

- PLAN4-R01: `just plan-check` requires every gate episode to appear in this document and to be the last episode of the phase's final volume in SL-REG-002 (R-PLAN-GATE).
- PLAN4-R02: a gate episode reaches Demonstrated or Accepted only with evidence (master plan section 6.2); `just evidence-verify` and `just ep-advance` enforce it.
- PLAN4-R03 to PLAN4-R05: reviewed by the founder at each gate; no recipe measures them before the gate volumes exist.
- Every episode id cited here must exist: `just plan-check` (R-PLAN-EP); the document: `just doc-check`.

## Traceability

Master plan sections 2.7, 4.3 and 6.2, "First vertical slice"; SL-EP-010; SL-EP-100, 200, 400, 500, 640 and 840;
SL-REG-002 volume ranges; work order WO-011.

## Open issues

- The step-to-episode mapping of the first slice is proposed here from episode titles; the founder reviews it.
- Step F (runtime trace) precedes Volume 16 (Sliced Insights) in the graph, so the mapped episodes give snapshots and diagnostics, not the full trace.
- Gate SL-EP-100 is the last episode of Volume 05; this slice builds only episodes 041-048 of phase P0.

## Change log

| Version | Date | Change |
| :---- | :---- | :---- |
| 0.1.0 | 2026-10-17 | First draft (WO-011). |
