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
Episodes 041-048 name the work orders WO-013 to WO-021 that build them; episodes 049-060 name WO-020 to WO-034, with the
seams each leaves. It is a view: the registry SL-REG-002 stays authoritative (master plan section 6.3, rule 1).

## Status and decisions

Status is Draft (version 0.1.0). A record's `title` and `purpose` are the SL-REG-002 `title` and `summary` of that
episode, verbatim; `work_orders` repeats the `Traces:` lines of the work orders in `work-orders/`. ADR-0003 lets
041-048 start before Volume 01 is Accepted and caps them at Demonstrated until episode 020 is Accepted. Episodes
049-060 are built by WO-024 to WO-034 and are Demonstrated-ready once the founder records evidence; no status is
changed here.

## Normative text

- VOL3-R01: Each episode 041-060, and no other, MUST have one record with a purpose, work orders, an acceptance recipe, a Kohi reference and a seam (master plan section 6.1).
- VOL3-R02: A record's title and purpose MUST NOT diverge from the registry (master plan section 6.3, rules 1 and 2).
- VOL3-R03: A work order listed in a record MUST exist in `work-orders/` and, for 041-048, MUST trace that episode (master plan section 6.3, rule 4).
- VOL3-R04: Episodes 041-048 MAY reach Demonstrated and MUST NOT reach Accepted before episode 020 is Accepted (ADR-0003; master plan section 6.2).
- VOL3-R05: A seam MUST state what the slice leaves undone, so no later work reads absence as completion (master plan section 6.3, rule 3).

```sl-data
{
 "volume": "SL-VOL-03",
 "episodes": [
  {"episode": "SL-EP-041", "title": "Select supported development environments", "purpose": "Arch Linux primary, Windows secondary", "work_orders": ["WO-013", "WO-021", "WO-033"], "acceptance": "just doctor-selftest", "kohi_ref": "Architecture overview and setup (#000–#002)", "seam": "none: delivered by the listed work orders"},
  {"episode": "SL-EP-042", "title": "Establish the C++ toolchain", "purpose": "compiler, linker, standard library, debugger", "work_orders": ["WO-013", "WO-014", "WO-021"], "acceptance": "just cpp-smoke", "kohi_ref": "Architecture overview and setup (#000–#002)", "seam": "none: delivered by the listed work orders"},
  {"episode": "SL-EP-043", "title": "Define the C++23 baseline", "purpose": "supported features proven by toolchain tests", "work_orders": ["WO-015", "WO-021"], "acceptance": "just features-check", "kohi_ref": "Architecture overview and setup (#000–#002)", "seam": "none: delivered by the listed work orders"},
  {"episode": "SL-EP-044", "title": "Evaluate C++26 capabilities", "purpose": "feature tests and promotion criteria", "work_orders": ["WO-015", "WO-021"], "acceptance": "just cxx26-report", "kohi_ref": "Architecture overview and setup (#000–#002)", "seam": "none: delivered by the listed work orders"},
  {"episode": "SL-EP-045", "title": "Configure Vulkan development", "purpose": "SDK, loader, validation layers, diagnostics", "work_orders": ["WO-016", "WO-021"], "acceptance": "just vk-check", "kohi_ref": "Architecture overview and setup (#000–#002)", "seam": "none: delivered by the listed work orders"},
  {"episode": "SL-EP-046", "title": "Establish the repository architecture", "purpose": "engine, editor, tools, SDK, tests, docs, examples, third_party", "work_orders": ["WO-001", "WO-014", "WO-021"], "acceptance": "just presets-check", "kohi_ref": "Architecture overview and setup (#000–#002)", "seam": "none: delivered by the listed work orders"},
  {"episode": "SL-EP-047", "title": "Design the build system", "purpose": "targets, dependency graph, configurations, generated metadata", "work_orders": ["WO-014", "WO-018", "WO-020", "WO-021", "WO-029"], "acceptance": "just cpp-smoke", "kohi_ref": "Architecture overview and setup (#000–#002)", "seam": "none: delivered by the listed work orders"},
  {"episode": "SL-EP-048", "title": "Lock dependencies reproducibly", "purpose": "lockfiles, manifests, update process", "work_orders": ["WO-017", "WO-018", "WO-019", "WO-021"], "acceptance": "just deps-check", "kohi_ref": "Architecture overview and setup (#000–#002)", "seam": "none: delivered by the listed work orders"},
  {"episode": "SL-EP-049", "title": "Automated formatting", "purpose": "style, include order, enforcement", "work_orders": ["WO-020", "WO-024", "WO-034"], "acceptance": "just fmt-check-all", "kohi_ref": "none", "seam": "none: delivered by the listed work orders; 37 long Python lines stay allow-listed in tools/format/allow.txt and shfmt is not installed here"},
  {"episode": "SL-EP-050", "title": "Static analysis", "purpose": "warnings-as-errors, sanitizers, analyzers, quality gates", "work_orders": ["WO-025", "WO-034"], "acceptance": "just sanitize-proof", "kohi_ref": "none", "seam": "none: delivered by the listed work orders; tidy allow list empty since 2026-10-18"},
  {"episode": "SL-EP-051", "title": "Test infrastructure", "purpose": "unit, integration, system, end-to-end runners", "work_orders": ["WO-018", "WO-026", "WO-034"], "acceptance": "just test-tiers", "kohi_ref": "none", "seam": "none: delivered by the listed work orders; each upper tier holds one or two smoke tests and nothing runs on a GPU device"},
  {"episode": "SL-EP-052", "title": "Test fixtures and golden data", "purpose": "versioned assets, expected outputs, deterministic fixtures", "work_orders": ["WO-027", "WO-034"], "acceptance": "just golden-check", "kohi_ref": "none", "seam": "none: delivered by the listed work orders; six small text fixtures, no binary assets"},
  {"episode": "SL-EP-053", "title": "Local CI runner", "purpose": "build, test, format and contract checks in one command", "work_orders": ["WO-001", "WO-012", "WO-020", "WO-028", "WO-034"], "acceptance": "just ci-local-full", "kohi_ref": "none", "seam": "none: delivered by the listed work orders; ci-local is a local script and the Windows job has never run"},
  {"episode": "SL-EP-054", "title": "Initial CI pipeline", "purpose": "clean builds, caching, artifacts, failure reports", "work_orders": ["WO-028", "WO-034"], "acceptance": "just ci-yaml-check", "kohi_ref": "none", "seam": "workflows not yet executed on GitHub: validated structurally and by pin hashes only, no hosted run exists"},
  {"episode": "SL-EP-055", "title": "Artifact provenance", "purpose": "source revision, compiler ID, parameters, output hashes (F3 seed)", "work_orders": ["WO-029", "WO-034"], "acceptance": "just provenance-check", "kohi_ref": "none", "seam": "none: delivered by the listed work orders; records live under build/, no content-addressed store or signing"},
  {"episode": "SL-EP-056", "title": "Developer diagnostics", "purpose": "structured logs, error codes, stack traces, machine-readable reports", "work_orders": ["WO-030", "WO-034"], "acceptance": "just log-schema-check", "kohi_ref": "Logging and assertions (#003)", "seam": "none: delivered by the listed work orders; sl_sandbox emits a build-info event; ADR-0009 is Proposed"},
  {"episode": "SL-EP-057", "title": "Documentation generation", "purpose": "API docs, dependency reports, architecture references", "work_orders": ["WO-031", "WO-034"], "acceptance": "just docs-check", "kohi_ref": "none", "seam": "Doxygen not installed on the reference machine: the API page is a stub"},
  {"episode": "SL-EP-058", "title": "Secure dependency ingestion", "purpose": "licenses, checksums, vulnerability findings", "work_orders": ["WO-021", "WO-032", "WO-034"], "acceptance": "just deps-licenses", "kohi_ref": "none", "seam": "none: delivered by the listed work orders; vulnerability findings are an empty, manually maintained register"},
  {"episode": "SL-EP-059", "title": "Reproducible development image", "purpose": "packaged toolchain and environment", "work_orders": ["WO-033", "WO-034"], "acceptance": "just devimage-check", "kohi_ref": "none", "seam": "image unbuilt: the Containerfile has never been built, only the toolchain lock and package list are checked"},
  {"episode": "SL-EP-060", "title": "Gate the first infrastructure release", "purpose": "clean checkout builds, tests and emits traceable artifacts", "work_orders": ["WO-034"], "acceptance": "just acceptance-049-060", "kohi_ref": "none", "seam": "none: delivered by the listed work orders; the gate has run locally only, no GitHub run and no evidence record yet"}
 ]
}
```

## Measures and enforcement points

- VOL3-R01 and VOL3-R03: `just plan-check` parses the `sl-data` block, requires exactly episodes 041-060 and checks each work order file (R-PLAN-VOL03).
- VOL3-R02: `just plan-check` compares title and purpose with SL-REG-002; `just registry-check` validates the registry itself.
- VOL3-R04: `just registry-check` applies the ADR-0003 waiver (R-DEP-ACCEPTED); `just ep-advance` enforces the cap.
- VOL3-R05: review by the founder; the seams are listed under Open issues for the next slice.
- Every document: `just doc-check`; the register entry: `just registry-docs-check`.

## Traceability

Master plan Volume 03 block, sections 5.1 and 6.1; SL-REG-002 episodes 041-060; work orders WO-013 to WO-021
(WO-001 for episodes 046 and 053, WO-012 for episode 053) and WO-024 to WO-034 for episodes 049-060; ADR-0003 and ADR-0004; SL-PLAN-005 for the Kohi references of 041-048 and 056.

## Open issues

- Episodes 049-060 are built by WO-020 and WO-024 to WO-034; their records are in `docs/episodes/049-060.md`. Seams that stay open: 054 (workflows never executed on GitHub), 057 (Doxygen absent here), 059 (image unbuilt), and the allow-lists named in the 049 and 050 seams.
- Windows (ADR-0006) and GPU behaviour are unverified for the whole volume.
- Provenance (episode 055) lives under `build/`; a content-addressed store is a later volume.
- Per-episode acceptance beyond the listed recipe (section 6.1 positive, negative and boundary cases) is held by the work orders.
- An `acceptance` recipe is a stub (exit 3) until the work order that owns it lands (`tools/governance/recipes.json`).

## Change log

| Version | Date | Change |
| :---- | :---- | :---- |
| 0.1.0 | 2026-10-17 | First draft (WO-011). |
| 0.1.0 | 2026-10-18 | WO-034: records 049-060 name their acceptance recipe and real seam. |
