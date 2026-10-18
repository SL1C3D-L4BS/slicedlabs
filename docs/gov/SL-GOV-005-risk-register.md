---
id: SL-GOV-005
kind: GOV
title: "Risk and Feasibility Register"
status: Draft
version: 0.1.0
owner: founder
episodes: [013]
depends_on: [SL-GOV-001]
supersedes: []
decisions: [ADR-0002, ADR-0005, ADR-0006, ADR-0007]
---
# SL-GOV-005: Risk and Feasibility Register

## Purpose

This document records the technical uncertainty, dependency and delivery-constraint risks of the program, one row
per risk R-nnn with likelihood, impact, trigger, mitigation and owner episode. Episode 013 ("technical uncertainty,
dependencies, delivery constraints", master plan Volume 01) writes it. It is the risk side of the release gates in
SL-GOV-007 (master plan, Release gates).

## Status and decisions

Status is Draft (version 0.1.0); the founder approves. The rows are bound by ADR-0002 (dependencies), ADR-0005 (CMake
floor, `import std`), ADR-0006 (Windows parity) and ADR-0007 (factory manual mode). Machine facts are those of the
plan of record, 2026-10-17. The register lists risks; it grants no exemption (those live in SL-REG-006, master plan
section 3.1).

## Normative text

- GOV5-R01: Every risk MUST have an id R-nnn, a likelihood, an impact, a trigger, a mitigation and an owner episode (master plan, Volume 01 episode 013).
- GOV5-R02: Likelihood and impact MUST use the labels low, medium or high; the master plan gives no numeric scale (see Open issues, master plan, Volume 01 episode 013).
- GOV5-R03: A risk about a capability the development machine lacks MUST be mitigated by a capability-gated path and not by assuming the capability (master plan section 2.6).
- GOV5-R04: Windows MUST NOT be reported as verified before a hosted Windows configure has run (ADR-0006).
- GOV5-R05: Work that rests on the CMake 3.28 floor, `import std` or an unpinned vcpkg baseline MUST list it as an open issue in its episode notes (ADR-0005, ADR-0002, master plan section 6.1).
- GOV5-R06: Completion of work MUST rest on recorded evidence, not on a report of done, while the factory runs in manual mode (ADR-0007, master plan section 6.3 rule 4).
- GOV5-R07: A risk MUST be reviewed at every milestone of master plan section 1.4 and when its trigger fires (master plan section 1.4).
- GOV5-R08: A new risk SHOULD cite its source, a master plan section, an ADR or a machine fact (master plan section 6.3).

```sl-data
{"scale": ["low", "medium", "high"],
"risks": [
{"id": "R-001", "risk": "Development GPU (RX 580 8 GB, RADV 1.4.354) has no mesh shaders and no hardware ray tracing", "likelihood": "high", "impact": "high", "trigger": "An episode in Volumes 27-28 needs a mesh-shader or hardware-RT path to run or be measured", "mitigation": "Capability-gated paths with a fallback; the profiler shows which path ran; hardware-lab evidence for the parity gate", "owner_episode": "SL-EP-008", "source": "machine fact; master plan sections 2.6 and 4.3"},
{"id": "R-002", "risk": "Windows (MSVC and clang-cl) parity is unverified locally", "likelihood": "high", "impact": "medium", "trigger": "A claim of Windows support, or a change to presets or the flag table, before the hosted configure job has run", "mitigation": "Structural checks on Linux only; windows-latest workflow_dispatch job after the first push; list Windows as unverified until then", "owner_episode": "SL-EP-041", "source": "ADR-0006"},
{"id": "R-003", "risk": "The CMake 3.28 floor is untested; only CMake 4.4.4 is tested", "likelihood": "medium", "impact": "medium", "trigger": "A CMake feature newer than 3.28 is used without a CMAKE_VERSION guard", "mitigation": "Guard features above 3.28 by CMAKE_VERSION; keep the floor an open issue in the episode notes", "owner_episode": "SL-EP-047", "source": "ADR-0005"},
{"id": "R-004", "risk": "Factory runs in manual mode; there is no wo-pipeline or land, and commit signing is not defined", "likelihood": "high", "impact": "medium", "trigger": "A work order lands without founder verification, evidence or a merge by the founder; or a commit lacks a verifiable author", "mitigation": "The founder runs wo-verify, approve.py, the merge and evidence-record; agents never commit or push", "owner_episode": "SL-EP-012", "source": "ADR-0007"},
{"id": "R-005", "risk": "One-person program against 840 episodes in 42 volumes, all in 1.0 scope", "likelihood": "high", "impact": "high", "trigger": "A milestone slips, or an episode is started before its dependencies are Demonstrated", "mitigation": "Dependency order with a shipped milestone each (Foundation to 1.0); one episode per work order; Claude Code builds each episode in order", "owner_episode": "SL-EP-018", "source": "master plan section 1.4"},
{"id": "R-006", "risk": "import std is unverified (CMake gate UUID unconfirmed)", "likelihood": "medium", "impact": "low", "trigger": "Code or a preset relies on import std", "mitigation": "CMAKE_CXX_SCAN_FOR_MODULES OFF; import std is a report-only probe that never fails the build", "owner_episode": "SL-EP-044", "source": "ADR-0005"},
{"id": "R-007", "risk": "The vcpkg baseline is unpinned (vcpkg is not installed here)", "likelihood": "high", "impact": "medium", "trigger": "A build resolves a package through vcpkg without a pinned baseline", "mitigation": "Baseline stays null and unpinned until the founder pins it; the lockfile holds the other providers; recorded in episode 048", "owner_episode": "SL-EP-048", "source": "ADR-0002"},
{"id": "R-008", "risk": "Console SDKs are under NDA and not available to the program", "likelihood": "medium", "impact": "high", "trigger": "Gate 4 needs a console conformance or certification pre-check", "mitigation": "Platform APIs behind the backend contract; certification is a declared exemption class once registered in SL-REG-006", "owner_episode": "SL-EP-009", "source": "master plan sections 2.6 and 3.1"},
{"id": "R-009", "risk": "Unreal parity cannot be measured on the minimum machine", "likelihood": "high", "impact": "high", "trigger": "Gate 7 or the episode 640 gate needs SL-BENCH on hardware above the minimum row", "mitigation": "Recommended and high-end rows set by episode 008; benchmarks run on the hardware lab, never extrapolated", "owner_episode": "SL-EP-008", "source": "master plan section 4.3 and Release gates"},
{"id": "R-010", "risk": "The plan copy drifts from the pinned master plan", "likelihood": "low", "impact": "high", "trigger": "registry-gen-check or plan-sha fails", "mitigation": "SHA-pinned byte copy; the generator re-parses it and fails on drift", "owner_episode": "SL-EP-012", "source": "plan of record, WO-004"}
]}
```

## Measures and enforcement points

- Completeness: numerator = rows in the `risks` list lacking any GOV5-R01 field; denominator = all rows; threshold 0. No gate reads this block yet; it is listed under Open issues.
- Labels: numerator = rows whose likelihood or impact is outside `scale`; denominator = all rows; threshold 0. Same status.
- R-002, R-003, R-006 and R-007 are carried into the 041-048 episode notes as open issues (WO-021, ADR-0005, ADR-0006).
- `just doc-check` enforces the template and citations of this document; `just registry-docs-check` its registration.

## Traceability

Master plan sections 1.4, 2.6, 3.1, 4.3 and Release gates; episodes SL-EP-008, SL-EP-013, SL-EP-018 and SL-EP-041 to
SL-EP-048; ADR-0002, ADR-0005, ADR-0006, ADR-0007; SL-GOV-007 (gates and profiles); work order WO-009.

## Open issues

- The master plan gives no likelihood or impact scale or review period; the labels are this document's own.
- The owner episodes are this document's assignment; the master plan names only episode 013 as the register's owner.
- ADR-0007 does not define commit signing; R-004 records the gap and the founder decides the rule.
- No gate checks the `risks` block yet; a `risk-check` recipe is not planned by any work order.

## Change log

| Version | Date | Change |
| :---- | :---- | :---- |
| 0.1.0 | 2026-10-17 | First draft (WO-009). |
