---
id: SL-GOV-007
kind: GOV
title: "Release Gates and Capability Profiles"
status: Draft
version: 0.1.0
owner: founder
episodes: [008, 009]
depends_on: [SL-GOV-001, SL-GOV-006]
supersedes: []
decisions: [ADR-0006]
---
# SL-GOV-007: Release Gates and Capability Profiles

## Purpose

This document holds the seven release gates, the delivery milestones, the platform and backend table, the hardware
support matrix and the capability profiles per platform. It implements master plan sections 1.4, 2.6 and 4.3 and the
"Release gates" table. Episode 008 (hardware support matrix) and episode 009 (target platforms and capability
profiles) write it.

## Status and decisions

Status is Draft (version 0.1.0); the founder approves. The minimum hardware row is the development workstation
(machine fact, plan of record, 2026-10-17); the recommended and high-end rows are targets that episode 008 sets and
are not set here. ADR-0006 binds the Windows row: no platform evidence exists until its own run.

## Normative text

- GOV7-R01: Sliced Labs 1.0 MUST ship only when all seven gates in `gates` pass with recorded evidence on every platform in `platforms` (master plan, Release gates).
- GOV7-R02: Each gate MUST be judged by its passes-when condition and its named evidence, and by nothing else (master plan, Release gates).
- GOV7-R03: Each milestone in `milestones` MUST ship to real users so the next is built on evidence (master plan section 1.4).
- GOV7-R04: Each phase gate in `phase_gates` MUST be a running product with recorded evidence, not a document (master plan section 4.3).
- GOV7-R05: Each platform MUST use the backend listed for it in `platforms` (master plan section 2.6).
- GOV7-R06: Mesh shaders, ray tracing and bindless MUST be capability-gated paths, and the profiler MUST show which path ran (master plan section 2.6).
- GOV7-R07: The minimum hardware row MUST be this workstation, and the rows `recommended` and `high_end` MUST stay marked as targets until episode 008 sets them (master plan, Volume 01 episode 008; machine fact).
- GOV7-R08: No gate result for a platform MAY be reported without a run on that platform (master plan, Release gates; ADR-0006).
- GOV7-R09: A gate failure MUST NOT be waived except by a registered exemption of the six classes (master plan section 3.1, SL-GOV-006 GOV6-R04).

```sl-data
{"gates": [
{"gate": 1, "name": "Architectural integrity", "passes_when": "Every authoritative record has owner, stable ID, revisions; F1-F8 at threshold", "evidence": "Conformance suite, fragmentation scorecard"},
{"gate": 2, "name": "Production capability", "passes_when": "Reference games (2D, 3D multiplayer, mobile, XR, console, web) and a digital human are made entirely in Sliced Labs", "evidence": "Reference projects + F8 workflow tests"},
{"gate": 3, "name": "Engineering reliability", "passes_when": "Reproducible builds, migrations, crash recovery, soak and security tests pass", "evidence": "Qualification report"},
{"gate": 4, "name": "Platform readiness", "passes_when": "Every target passes conformance, store and certification pre-checks", "evidence": "Platform reports"},
{"gate": 5, "name": "Web and services", "passes_when": "Hub, Web Studio, Stream, marketplace and online services meet SLOs", "evidence": "SLO dashboards, pen test"},
{"gate": 6, "name": "Operational readiness", "passes_when": "Install, update, support, incident and rollback procedures drilled", "evidence": "Drill records"},
{"gate": 7, "name": "Competitive validation", "passes_when": "Every parity-matrix row matches or beats Unreal Engine 5 on SL-BENCH", "evidence": "Audited benchmark register"}
],
"milestones": [
{"milestone": "Foundation", "episodes": "001-100", "ships": "Canonical model, toolchain, runtime, ECS"},
{"milestone": "Core loop", "episodes": "101-200", "ships": "Editor, Vulkan renderer, materials, asset pipeline"},
{"milestone": "Production", "episodes": "201-400", "ships": "2D, 3D, animation, audio, VFX, physics, networking, profiling, replay, collaboration, code, governed AI"},
{"milestone": "Early Access", "episodes": "401-500", "ships": "Verification, build farm, services, SDK, Linux + Windows desktop release"},
{"milestone": "Sliced Labs 1.0", "episodes": "501-840", "ships": "Unreal-class rendering, large worlds, digital humans, destruction, all platforms, web runtime, Sliced Hub website, marketplace, virtual production, enterprise"}
],
"phase_gates": [
{"phase": "P0 Foundation", "volumes": "01-05", "gate_episode": 100, "gate": "runtime"},
{"phase": "P1 Core loop", "volumes": "06-10", "gate_episode": 200, "gate": "asset pipeline"},
{"phase": "P2 Production", "volumes": "11-20", "gate_episode": 400, "gate": "2D + 3D games"},
{"phase": "P3 Early Access", "volumes": "21-25", "gate_episode": 500, "gate": "EA release"},
{"phase": "P4 Unreal-class depth", "volumes": "26-32", "gate_episode": 640, "gate": "SL-BENCH parity"},
{"phase": "P5 Platforms + web + ecosystem", "volumes": "33-42", "gate_episode": 840, "gate": "Sliced Labs 1.0"}
],
"platforms": [
{"platform": "Linux", "backend": "Vulkan", "volumes": "07"},
{"platform": "Windows", "backend": "Vulkan (DirectX 12 adapter for Xbox shared code)", "volumes": "07, 35"},
{"platform": "macOS", "backend": "Metal 3 native", "volumes": "26"},
{"platform": "iOS, iPadOS", "backend": "Metal 3 native", "volumes": "33"},
{"platform": "Android", "backend": "Vulkan", "volumes": "33"},
{"platform": "Meta Quest, Android XR", "backend": "Vulkan + OpenXR", "volumes": "34"},
{"platform": "Apple Vision Pro", "backend": "Metal + visionOS", "volumes": "34"},
{"platform": "PlayStation, Xbox, Nintendo Switch 2", "backend": "Platform APIs behind the backend contract (NDA SDKs)", "volumes": "35"},
{"platform": "Browser", "backend": "WebGPU via WebAssembly", "volumes": "36"}
],
"hardware_matrix": [
{"tier": "minimum", "status": "fact", "gpu": "AMD Radeon RX 580, 8 GB", "cpu": "Intel Core i7-6700", "ram": "31 GiB", "mesh_shaders": false, "hardware_rt": false, "source": "machine fact, development workstation"},
{"tier": "recommended", "status": "target, set by SL-EP-008", "gpu": null, "cpu": null, "ram": null, "mesh_shaders": null, "hardware_rt": null, "source": "master plan, Volume 01 episode 008"},
{"tier": "high_end", "status": "target, set by SL-EP-008", "gpu": null, "cpu": null, "ram": null, "mesh_shaders": null, "hardware_rt": null, "source": "master plan, Volume 01 episode 008"}
],
"capability_profiles": [
{"platform": "Linux", "profile": "desktop", "gated_paths": ["mesh shaders", "ray tracing", "bindless"], "baseline": "hardware_matrix minimum; mesh shaders and hardware RT absent, bindless unverified", "set_by": "SL-EP-009"},
{"platform": "Windows", "profile": "desktop", "gated_paths": ["mesh shaders", "ray tracing", "bindless"], "baseline": "same rows as Linux; unverified until a Windows run (ADR-0006)", "set_by": "SL-EP-009"},
{"platform": "macOS", "profile": "desktop", "gated_paths": ["mesh shaders", "ray tracing", "bindless"], "baseline": "to be set", "set_by": "SL-EP-009"},
{"platform": "iOS, iPadOS, Android", "profile": "mobile", "gated_paths": ["mesh shaders", "ray tracing", "bindless"], "baseline": "to be set (GPU tiers, memory and thermal, episode 641)", "set_by": "SL-EP-009"},
{"platform": "Meta Quest, Android XR, Apple Vision Pro", "profile": "XR", "gated_paths": ["mesh shaders", "ray tracing", "bindless"], "baseline": "to be set", "set_by": "SL-EP-009"},
{"platform": "PlayStation, Xbox, Nintendo Switch 2", "profile": "console", "gated_paths": ["mesh shaders", "ray tracing", "bindless"], "baseline": "to be set (NDA SDKs)", "set_by": "SL-EP-009"},
{"platform": "Browser", "profile": "browser", "gated_paths": ["mesh shaders", "ray tracing", "bindless"], "baseline": "to be set", "set_by": "SL-EP-009"}
]}
```

## Measures and enforcement points

- Gate pass rate: numerator = gates (1-7) with their evidence recorded for every platform in `platforms`; denominator = 7; threshold 7 of 7 for Sliced Labs 1.0 (master plan, Release gates). Today 0 of 7; no gate has a build.
- Plan agreement: numerator = rows of `gates`, `milestones`, `phase_gates` and `platforms` differing from the master plan tables; denominator = all rows; threshold 0. No recipe compares them yet (Open issues).
- Capability profile completeness: numerator = profiles with `baseline` set; denominator = 7; threshold 7 of 7 by episode 009. Today 2 of 7.
- `just doc-check` enforces this document's template and citations; `just registry-docs-check` its registration.

## Traceability

Master plan sections 1.4, 2.6, 2.7, 3.1 and 4.3 and the "Release gates" table; episodes SL-EP-008, SL-EP-009, SL-EP-018;
SL-GOV-005 (risks R-001, R-002, R-008, R-009), SL-GOV-006 (exemptions), SL-GOV-010 (parity matrix, gate 7); ADR-0006;
work order WO-009.

## Open issues

- The master plan section 1.4 table has five milestones; this document lists those five and the six phase gates of section 4.3 separately.
- The recommended and high-end hardware rows have no values; episode 008 sets them. The minimum row has CPU, GPU and RAM only, not OS, storage or display.
- Bindless support on the minimum row is unverified; the master plan names bindless as capability-gated without a per-platform result.
- Per-platform capability baselines other than the minimum row are not in the master plan; episode 009 and the volumes in `platforms` set them.
- No recipe compares the blocks with the master plan tables; the plan-agreement measure has no enforcement point.
- Episode 018 (public roadmap, evidence-based release gates) may amend the gate wording; this document follows the plan until then.

## Change log

| Version | Date | Change |
| :---- | :---- | :---- |
| 0.1.0 | 2026-10-17 | First draft (WO-009). |
