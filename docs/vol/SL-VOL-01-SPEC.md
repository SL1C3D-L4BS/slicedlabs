---
id: SL-VOL-01-SPEC
kind: VOL
title: "Volume 01 Specification"
status: Draft
version: 0.1.0
owner: founder
episodes: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20]
depends_on: [SL-GOV-001]
supersedes: []
decisions: []
---
# SL-VOL-01-SPEC: Volume 01 Specification

## Purpose

Volume 01 (product vision, requirements, competitive architecture) defines what Sliced Labs is, what it must do
and how completion is measured (master plan, Volume 01 block). This document holds one record per episode
001-020 as the plan states them (master plan section 6.1). It is a view: the registry SL-REG-002 stays authoritative
(master plan section 6.3, rule 1).

## Status and decisions

Status is Draft (version 0.1.0). Each record's `title` and `purpose` are the SL-REG-002 `title` and `summary`
of that episode, verbatim. The primary document and work order come from `registry/vol01-documents.json`
(the approved episode to document map). Approval of the volume is episode 020 (WO-023).

## Normative text

- VOL1-R01: Each episode 001-020 MUST have one record with a purpose, a primary document, a closing work order, an acceptance recipe and a Kohi reference (master plan section 6.1).
- VOL1-R02: A record's purpose MUST NOT diverge from the registry summary of that episode (master plan section 6.3, rules 1 and 2).
- VOL1-R03: An episode becomes Accepted only on evidence: passing acceptance tests and a completion record, not on code existing (master plan section 6).

```sl-data
{"volume": "SL-VOL-01", "episodes": [
{"episode": "SL-EP-001", "title": "Define the Sliced Labs product", "purpose": "engine, editor, SDK, runtime, content suite, automation layer and delivery platform as one product family", "document": "SL-GOV-001", "work_order": "WO-006", "acceptance": "just constitution-check", "kohi_ref": "none"},
{"episode": "SL-EP-002", "title": "Establish the competitive position", "purpose": "compare Unreal, Unity, Godot, proprietary engines, DCC tools and standalone dev tools", "document": "SL-GOV-010", "work_order": "WO-008", "acceptance": "just capability-check", "kohi_ref": "none"},
{"episode": "SL-EP-003", "title": "Define the zero-manual-fragmentation doctrine", "purpose": "native workflows, permitted integrations, F1–F8 measurement", "document": "SL-GOV-006", "work_order": "WO-006", "acceptance": "just constitution-check", "kohi_ref": "none"},
{"episode": "SL-EP-004", "title": "Define users and production scenarios", "purpose": "solo devs, studios, technical artists, simulation engineers, platform developers", "document": "SL-GOV-002", "work_order": "WO-007", "acceptance": "just rtm-check", "kohi_ref": "none"},
{"episode": "SL-EP-005", "title": "Define the capability ontology", "purpose": "canonical inventory of capabilities, dependencies, interfaces, ownership", "document": "SL-GOV-003", "work_order": "WO-008", "acceptance": "just capability-check", "kohi_ref": "none"},
{"episode": "SL-EP-006", "title": "Define the product boundary", "purpose": "core, optional modules, plugins, external integrations, hosted services", "document": "SL-GOV-003", "work_order": "WO-008", "acceptance": "just capability-check", "kohi_ref": "none"},
{"episode": "SL-EP-007", "title": "Establish the quality model", "purpose": "reliability, performance, usability, accessibility, security, compatibility", "document": "SL-GOV-009", "work_order": "WO-010", "acceptance": "just policy-contracts-check", "kohi_ref": "none"},
{"episode": "SL-EP-008", "title": "Define the hardware support matrix", "purpose": "minimum, recommended and high-end workstation configurations", "document": "SL-GOV-007", "work_order": "WO-009", "acceptance": "just exception-check", "kohi_ref": "none"},
{"episode": "SL-EP-009", "title": "Define the target platforms and capability profiles", "purpose": "desktop, mobile, XR, console and browser, all in 1.0 scope", "document": "SL-GOV-007", "work_order": "WO-009", "acceptance": "just exception-check", "kohi_ref": "none"},
{"episode": "SL-EP-010", "title": "Define the product lifecycle", "purpose": "create, author, execute, debug, collaborate, test, release", "document": "SL-PLAN-004", "work_order": "WO-011", "acceptance": "just plan-check", "kohi_ref": "none"},
{"episode": "SL-EP-011", "title": "Define measurable acceptance criteria", "purpose": "product promises converted to tests and evidence", "document": "SL-GOV-002", "work_order": "WO-007", "acceptance": "just rtm-check", "kohi_ref": "none"},
{"episode": "SL-EP-012", "title": "Establish the engineering decision system", "purpose": "ADRs, status rules, supersession, change control", "document": "SL-GOV-004", "work_order": "WO-003", "acceptance": "just adr-check", "kohi_ref": "none"},
{"episode": "SL-EP-013", "title": "Establish risk and feasibility registers", "purpose": "technical uncertainty, dependencies, delivery constraints", "document": "SL-GOV-005", "work_order": "WO-009", "acceptance": "just exception-check", "kohi_ref": "none"},
{"episode": "SL-EP-014", "title": "Define the build-versus-adopt policy", "purpose": "proprietary advantages vs mature infrastructure", "document": "SL-GOV-008", "work_order": "WO-010", "acceptance": "just policy-contracts-check", "kohi_ref": "none"},
{"episode": "SL-EP-015", "title": "Define the dependency policy", "purpose": "licensing, security, support, reproducibility, supply chain", "document": "SL-GOV-008", "work_order": "WO-010", "acceptance": "just policy-contracts-check", "kohi_ref": "none"},
{"episode": "SL-EP-016", "title": "Define the development economics", "purpose": "effort, hardware, distribution, support, hosting, maintenance", "document": "SL-GOV-009", "work_order": "WO-010", "acceptance": "just policy-contracts-check", "kohi_ref": "none"},
{"episode": "SL-EP-017", "title": "Define the extensibility strategy", "purpose": "first-party modules, plugins, native extensions, compatibility", "document": "SL-GOV-003", "work_order": "WO-008", "acceptance": "just capability-check", "kohi_ref": "none"},
{"episode": "SL-EP-018", "title": "Define the public roadmap", "purpose": "capability milestones and evidence-based release gates", "document": "SL-PLAN-002", "work_order": "WO-011", "acceptance": "just plan-check", "kohi_ref": "none"},
{"episode": "SL-EP-019", "title": "Create the master requirements traceability matrix", "purpose": "requirement → architecture → implementation → test", "document": "SL-REG-003", "work_order": "WO-007", "acceptance": "just rtm-check", "kohi_ref": "none"},
{"episode": "SL-EP-020", "title": "Approve the Engine Constitution", "purpose": "freeze locked commitments; record conditional decisions", "document": "SL-GOV-001", "work_order": "WO-023", "acceptance": "just constitution-frozen", "kohi_ref": "none"}
]}
```

## Measures and enforcement points

- `acceptance` is the `just` recipe that gates the primary document: `doc-check` additionally gates every document, and `registry-docs-check` the register entry.
- The map covers the 20 Volume 01 documents and episodes 001-020: `just vol01-coverage` (master plan, Volume 01 block).
- Episode 020 closes with `just constitution-frozen` and `just vol01-gate` (WO-023), both stubs until then.

## Traceability

Master plan Volume 01 block and section 6.1; SL-REG-002 episodes 001-020; `registry/vol01-documents.json`;
SL-GOV-001 and SL-GOV-006 (this work order, WO-006).

## Open issues

- `kohi_ref` is `none` for every record: the Kohi arc table of master plan section 5.1 maps no arc to episodes 001-020.
- Per-episode acceptance beyond the document gate (positive, negative, boundary, recovery and performance cases of section 6.1) is not written; the owning work orders add it.
- Episodes 002, 005, 006, 008, 009, 013 to 019 are closed by other work orders; their recipes are stubs until those land.
- Dependencies, architecture and release fields of section 6.1 are held by SL-REG-002, not repeated here.

## Change log

| Version | Date | Change |
| :---- | :---- | :---- |
| 0.1.0 | 2026-10-17 | First draft (WO-006). |
