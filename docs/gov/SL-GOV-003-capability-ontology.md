---
id: SL-GOV-003
kind: GOV
title: "Capability Ontology and Map"
status: Draft
version: 0.1.0
owner: founder
episodes: [005, 006, 017]
depends_on: [SL-GOV-001]
supersedes: []
decisions: []
---
# SL-GOV-003: Capability Ontology and Map

## Purpose

This document is the canonical inventory of Sliced Labs capabilities: the twelve subsystems of master plan section 2.2
with what each owns authoritatively and what it only derives, the five boundary classes of the product boundary
(episode 006) and the extensibility strategy (episode 017). The machine-readable inventory is the capability register
SL-REG-004 (`registry/SL-REG-004-capabilities.json`); this document is its prose and `just capability-check` compares them.

## Status and decisions

Status is Draft (version 0.1.0); approval is the founder's. The subsystem table is copied from master plan section 2.2.
The boundary class and owning interface document of each seeded capability are proposals for the founder to
confirm (see Open issues). Hot reload is a formal protocol only (master plan section 1.5), and a change to a locked
decision needs an ADR (master plan section 6.3, rule 3).

## Normative text

Ownership (master plan section 2.2, doctrine in section 1.3):

- GOV3-R01: Every capability MUST belong to exactly one of the twelve subsystems 01 to 12 (master plan section 2.2).
- GOV3-R02: A project fact MUST have one authoritative owning subsystem; every other subsystem that holds it MUST treat it as derived, so there is no competing authoritative representation (master plan section 1.3, zero 2).
- GOV3-R03: Each capability MUST name its interface document (an SL-ARCH id) and the episode that introduces it (master plan section 6.3, rule 2, and the SL-ARCH table in the required documents).

Boundary classes (master plan episode 006, "core, optional modules, plugins, external integrations, hosted services"):

- GOV3-R04: Each capability MUST carry exactly one boundary class: `core`, `module` (optional first-party), `plugin`, `external` or `hosted` (master plan episode 006).
- GOV3-R05: An `external` capability (compiler, debugger, GPU driver, platform SDK, middleware behind an adapter, interchange format) MAY be used, and MUST NOT create a second source of truth, an untracked transformation or a manual reconciliation step (master plan section 1.3).
- GOV3-R06: A `hosted` capability (Sliced Hub services) MUST stay optional for local work, except marketplace, cloud builds, web studio and online services, which require it (master plan section 2.8).

Extensibility (master plan episode 017, "first-party modules, plugins, native extensions, compatibility"):

- GOV3-R07: Extension MUST go through a declared surface: a first-party module, a plugin, or a native extension; Luau is the extension language for tools, mods and gameplay iteration, and native C++ is for engine and shipping hot paths (master plan section 1.5).
- GOV3-R08: Live update of extension code MUST use the hot-reload protocol with its ABI manifest and rollback; arbitrary C++ reload is rejected (master plan section 1.5 and section 2.5).
- GOV3-R09: Plugin ABI, security model and compatibility guarantees MUST be defined in SL-ARCH-017 before any plugin boundary is frozen (master plan, Volume 24 and the SL-ARCH table).

Subsystems of master plan section 2.2 ("authoritative" is the Owns column, "derived" the Derives column), and the
boundary classes and extensibility surfaces named by episodes 006 and 017:

```sl-data
{"subsystems": [
{"n": "01", "name": "Unified Editor Shell", "owns": "Workspace layouts, command registry, selection", "derives": "Views over every other subsystem", "capability": "CAP-01-SHELL"},
{"n": "02", "name": "Canonical Project Model (Fabric)", "owns": "IDs, schemas, revisions, transactions, provenance", "derives": "—", "capability": "CAP-02-FABRIC"},
{"n": "03", "name": "Runtime Core", "owns": "Live simulation state (transient)", "derives": "Component schemas from 02", "capability": "CAP-03-RUNTIME-CORE"},
{"n": "04", "name": "Graphics and Compute", "owns": "GPU resources, pipelines (derived)", "derives": "Materials, meshes, render settings", "capability": "CAP-04-GRAPHICS-COMPUTE"},
{"n": "05", "name": "Content Production", "owns": "Editable source docs: meshes, sprites, graphs, curves, audio edits", "derives": "—", "capability": "CAP-05-CONTENT-PRODUCTION"},
{"n": "06", "name": "Simulation and Gameplay", "owns": "Gameplay config, nav settings, net topology", "derives": "Physics/nav data as derived artifacts", "capability": "CAP-06-SIMULATION-GAMEPLAY"},
{"n": "07", "name": "Code and Graph Toolchain", "owns": "Source files, graph IR", "derives": "Compiled modules, debug symbols", "capability": "CAP-07-CODE-GRAPH-TOOLCHAIN"},
{"n": "08", "name": "Asset and Build Graph", "owns": "Content-addressed store, build DAG", "derives": "Cooked artifacts with provenance", "capability": "CAP-08-ASSET-BUILD-GRAPH"},
{"n": "09", "name": "Observability and Replay", "owns": "Traces, captures, checkpoints", "derives": "Correlation to IDs/ticks/revisions", "capability": "CAP-09-OBSERVABILITY-REPLAY"},
{"n": "10", "name": "Collaboration and History", "owns": "Accepted revisions, branches, locks", "derives": "Semantic diffs", "capability": "CAP-10-COLLABORATION-HISTORY"},
{"n": "11", "name": "Verification and Delivery", "owns": "Test results, release manifests", "derives": "Packages, signatures", "capability": "CAP-11-VERIFICATION-DELIVERY"},
{"n": "12", "name": "Governed Automation", "owns": "Agent permissions, approvals, audit log", "derives": "Proposed change sets", "capability": "CAP-12-GOVERNED-AUTOMATION"}
],
"boundaries": [
{"class": "core", "plan_words": "core"},
{"class": "module", "plan_words": "optional modules"},
{"class": "plugin", "plan_words": "plugins"},
{"class": "external", "plan_words": "external integrations"},
{"class": "hosted", "plan_words": "hosted services"}
],
"extensibility": ["first-party modules", "plugins", "native extensions", "compatibility"]}
```

## Measures and enforcement points

- GOV3-R01, R03: `just capability-check` rules R-CAP-SHAPE, R-CAP-SUBSYS, R-CAP-DOC and R-CAP-EP; numerator is capabilities that fail, denominator is all capabilities, threshold 0.
- GOV3-R02: R-CAP-SUBSYS compares the `subsystems` block above with the master plan section 2.2 table (12 rows, owns and derives text).
- GOV3-R04: R-CAP-SHAPE requires the boundary enum; R-CAP-ONTOLOGY requires the five classes of the `boundaries` block.
- GOV3-R05 to R09: no automated gate yet; they are reviewed at the plan episodes and listed under Open issues.

## Traceability

Master plan sections 1.3, 1.5, 2.2, 2.5, 2.8 and 6.3; episodes SL-EP-005 (capability ontology), SL-EP-006 (product
boundary) and SL-EP-017 (extensibility strategy). Register: SL-REG-004. Companion document: SL-GOV-010 (parity matrix).
Tests: `tools/governance/test_check_caps.py`.

## Open issues

- The plan names the five boundary classes but does not define them beyond episode 006; the definitions above are the plan's own words and the per-capability assignment is a seed to be confirmed in episode 006.
- No SL-ARCH document covers Volume 21 or 22 (verification and delivery); capability CAP-11-VERIFICATION-DELIVERY points at SL-ARCH-008 as the nearest interface document.
- The reference-games row (Volume 25, 41) has no SL-ARCH of its own; CAP-11-REFERENCE-GAMES points at SL-ARCH-023.
- schemas/sl-reg-004.schema.json (WO-002) does not describe this register: it requires ids `SL-CAP-NNN` and the keys name, description, depends_on, owner, episodes, status, and forbids all others. The register uses id, subsystem, boundary, interface_doc, introduced_in, unreal_counterpart, src, so `capability-check` validates the shape itself (R-CAP-SHAPE) until the founder patches the schema.
- GOV3-R05 to R09 have no automated enforcement point yet.

## Change log

| Version | Date | Change |
| :---- | :---- | :---- |
| 0.1.0 | 2026-10-17 | First draft: twelve subsystems, boundary classes, extensibility strategy (WO-008). |
