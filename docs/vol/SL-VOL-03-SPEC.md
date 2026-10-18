---
id: SL-VOL-03-SPEC
kind: VOL
title: "Volume 03 Specification: Toolchain, repository, build infrastructure"
status: Draft
version: 0.1.0
owner: founder
episodes: [41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60]
depends_on: [SL-GOV-001]
supersedes: []
decisions: [ADR-0003, ADR-0004]
---
# SL-VOL-03-SPEC: Volume 03 Specification

## Purpose

Volume 03 (toolchain, repository, build infrastructure) gives a reproducible environment before significant engine
code (master plan, Volume 03 block). This document holds one record per episode 041-060 (master plan section 6.1).
Episodes 041-048 name the work orders WO-013 to WO-021 that build them; episodes 049-060 are described here with the
seams this slice leaves. It is a view: the registry SL-REG-002 stays authoritative (master plan section 6.3, rule 1).

## Status and decisions

Status is Draft (version 0.1.0). A record's `title` and `purpose` are the SL-REG-002 `title` and `summary` of that
episode, verbatim; `work_orders` repeats the `Traces:` lines of the work orders in `work-orders/`. ADR-0003 lets
041-048 start before Volume 01 is Accepted and caps them at Demonstrated until episode 020 is Accepted. Episodes
049-060 are not built by WO-013 to WO-021 except where a seam says otherwise.

## Normative text

- VOL3-R01: Each episode 041-060, and no other, MUST have one record with a purpose, work orders, an acceptance recipe, a Kohi reference and a seam (master plan section 6.1).
- VOL3-R02: A record's title and purpose MUST NOT diverge from the registry (master plan section 6.3, rules 1 and 2).
- VOL3-R03: A work order listed in a record MUST exist in `work-orders/` and, for 041-048, MUST trace that episode (master plan section 6.3, rule 4).
- VOL3-R04: Episodes 041-048 MAY reach Demonstrated and MUST NOT reach Accepted before episode 020 is Accepted (ADR-0003; master plan section 6.2).
- VOL3-R05: A seam MUST state what the slice leaves undone, so no later work reads absence as completion (master plan section 6.3, rule 3).

```sl-data
{"volume": "SL-VOL-03", "episodes": [
{"episode": "SL-EP-041", "title": "Select supported development environments", "purpose": "Arch Linux primary, Windows secondary", "work_orders": ["WO-013", "WO-021"], "acceptance": "just doctor-selftest", "kohi_ref": "Architecture overview and setup (#000–#002)", "seam": "none: delivered by the listed work orders"},
{"episode": "SL-EP-042", "title": "Establish the C++ toolchain", "purpose": "compiler, linker, standard library, debugger", "work_orders": ["WO-013", "WO-014", "WO-021"], "acceptance": "just cpp-smoke", "kohi_ref": "Architecture overview and setup (#000–#002)", "seam": "none: delivered by the listed work orders"},
{"episode": "SL-EP-043", "title": "Define the C++23 baseline", "purpose": "supported features proven by toolchain tests", "work_orders": ["WO-015", "WO-021"], "acceptance": "just features-check", "kohi_ref": "Architecture overview and setup (#000–#002)", "seam": "none: delivered by the listed work orders"},
{"episode": "SL-EP-044", "title": "Evaluate C++26 capabilities", "purpose": "feature tests and promotion criteria", "work_orders": ["WO-015", "WO-021"], "acceptance": "just cxx26-report", "kohi_ref": "Architecture overview and setup (#000–#002)", "seam": "none: delivered by the listed work orders"},
{"episode": "SL-EP-045", "title": "Configure Vulkan development", "purpose": "SDK, loader, validation layers, diagnostics", "work_orders": ["WO-016", "WO-021"], "acceptance": "just vk-check", "kohi_ref": "Architecture overview and setup (#000–#002)", "seam": "none: delivered by the listed work orders"},
{"episode": "SL-EP-046", "title": "Establish the repository architecture", "purpose": "engine, editor, tools, SDK, tests, docs, examples, third_party", "work_orders": ["WO-001", "WO-014", "WO-021"], "acceptance": "just presets-check", "kohi_ref": "Architecture overview and setup (#000–#002)", "seam": "none: delivered by the listed work orders"},
{"episode": "SL-EP-047", "title": "Design the build system", "purpose": "targets, dependency graph, configurations, generated metadata", "work_orders": ["WO-014", "WO-018", "WO-020", "WO-021"], "acceptance": "just cpp-smoke", "kohi_ref": "Architecture overview and setup (#000–#002)", "seam": "none: delivered by the listed work orders"},
{"episode": "SL-EP-048", "title": "Lock dependencies reproducibly", "purpose": "lockfiles, manifests, update process", "work_orders": ["WO-017", "WO-018", "WO-019", "WO-021"], "acceptance": "just deps-check", "kohi_ref": "Architecture overview and setup (#000–#002)", "seam": "none: delivered by the listed work orders"},
{"episode": "SL-EP-049", "title": "Automated formatting", "purpose": "style, include order, enforcement", "work_orders": ["WO-020"], "acceptance": "none in this slice", "kohi_ref": "none", "seam": "seam: format is enforced only through fmt-check inside ci-local v0; no include-order gate"},
{"episode": "SL-EP-050", "title": "Static analysis", "purpose": "warnings-as-errors, sanitizers, analyzers, quality gates", "work_orders": [], "acceptance": "none in this slice", "kohi_ref": "none", "seam": "seam: no static-analysis or tidy gate in this slice"},
{"episode": "SL-EP-051", "title": "Test infrastructure", "purpose": "unit, integration, system, end-to-end runners", "work_orders": ["WO-018"], "acceptance": "none in this slice", "kohi_ref": "none", "seam": "seam: one doctest smoke case under ctest (WO-018); no integration, system or end-to-end runners"},
{"episode": "SL-EP-052", "title": "Test fixtures and golden data", "purpose": "versioned assets, expected outputs, deterministic fixtures", "work_orders": [], "acceptance": "none in this slice", "kohi_ref": "none", "seam": "seam: no fixtures or golden data yet; WO-019 fixtures are provider-test inputs only"},
{"episode": "SL-EP-053", "title": "Local CI runner", "purpose": "build, test, format and contract checks in one command", "work_orders": ["WO-001", "WO-012", "WO-020"], "acceptance": "none in this slice", "kohi_ref": "none", "seam": "seam: ci-local v0 is a local script only; no contract-check composition beyond `just check`"},
{"episode": "SL-EP-054", "title": "Initial CI pipeline", "purpose": "clean builds, caching, artifacts, failure reports", "work_orders": [], "acceptance": "none in this slice", "kohi_ref": "none", "seam": "seam: no hosted CI pipeline; ci-local v0 is the only runner"},
{"episode": "SL-EP-055", "title": "Artifact provenance", "purpose": "source revision, compiler ID, parameters, output hashes (F3 seed)", "work_orders": [], "acceptance": "none in this slice", "kohi_ref": "none", "seam": "seam: provenance only in build-info.json (WO-014); no artifact signing or output-hash ledger"},
{"episode": "SL-EP-056", "title": "Developer diagnostics", "purpose": "structured logs, error codes, stack traces, machine-readable reports", "work_orders": [], "acceptance": "none in this slice", "kohi_ref": "Logging and assertions (#003)", "seam": "seam: no structured logs or error-code scheme yet"},
{"episode": "SL-EP-057", "title": "Documentation generation", "purpose": "API docs, dependency reports, architecture references", "work_orders": [], "acceptance": "none in this slice", "kohi_ref": "none", "seam": "seam: no documentation generation; generated views are limited to registry/views"},
{"episode": "SL-EP-058", "title": "Secure dependency ingestion", "purpose": "licenses, checksums, vulnerability findings", "work_orders": ["WO-021"], "acceptance": "none in this slice", "kohi_ref": "none", "seam": "seam: lock hash verification only (WO-018); no license or vulnerability findings"},
{"episode": "SL-EP-059", "title": "Reproducible development image", "purpose": "packaged toolchain and environment", "work_orders": [], "acceptance": "none in this slice", "kohi_ref": "none", "seam": "seam: no development image; the toolchain is recorded as data by WO-013"},
{"episode": "SL-EP-060", "title": "Gate the first infrastructure release", "purpose": "clean checkout builds, tests and emits traceable artifacts", "work_orders": [], "acceptance": "none in this slice", "kohi_ref": "none", "seam": "seam: release gate deferred; a green ci-local v0 is the nearest check"}
]}
```

## Measures and enforcement points

- VOL3-R01 and VOL3-R03: `just plan-check` parses the `sl-data` block, requires exactly episodes 041-060 and checks each work order file (R-PLAN-VOL03).
- VOL3-R02: `just plan-check` compares title and purpose with SL-REG-002; `just registry-check` validates the registry itself.
- VOL3-R04: `just registry-check` applies the ADR-0003 waiver (R-DEP-ACCEPTED); `just ep-advance` enforces the cap.
- VOL3-R05: review by the founder; the seams are listed under Open issues for the next slice.
- Every document: `just doc-check`; the register entry: `just registry-docs-check`.

## Traceability

Master plan Volume 03 block, sections 5.1 and 6.1; SL-REG-002 episodes 041-060; work orders WO-013 to WO-021
(WO-001 for episodes 046 and 053, WO-012 for episode 053); ADR-0003 and ADR-0004; SL-PLAN-005 for the Kohi references of 041-048 and 056.

## Open issues

- Episodes 050, 052, 054, 055, 056, 057, 059 and 060 have no work order in this slice; their seams above are what they leave.
- `ci-local` is v0: a local script, with no hosted CI and no tidy gate.
- Provenance lives only in `build-info.json` (WO-014); episode 055 owns the full F3 seed.
- Per-episode acceptance beyond the listed recipe (section 6.1 positive, negative and boundary cases) is held by the work orders.
- An `acceptance` recipe is a stub (exit 3) until the work order that owns it lands (`tools/governance/recipes.json`).

## Change log

| Version | Date | Change |
| :---- | :---- | :---- |
| 0.1.0 | 2026-10-17 | First draft (WO-011). |
