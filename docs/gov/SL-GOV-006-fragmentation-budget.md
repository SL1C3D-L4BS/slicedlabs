---
id: SL-GOV-006
kind: GOV
title: "Fragmentation Budget"
status: Draft
version: 0.1.0
owner: founder
episodes: [003]
depends_on: [SL-GOV-001]
supersedes: []
decisions: []
---
# SL-GOV-006: Fragmentation Budget

## Purpose

This document turns the doctrine "zero manual fragmentation" (SL-GOV-001, master plan section 1.3) into eight
invariants F1-F8. Each has a numerator, a denominator, a threshold and a named enforcement point, copied from
master plan section 3. It also records the declared exemptions (section 3.1), the violation flow (section 3.2) and
the scorecard (section 3.3). Episode 003 writes it.

## Status and decisions

Status is Draft (version 0.1.0); approval is the founder's. SL-GOV-001 is its parent. A release passes only
when every applicable invariant meets its threshold (master plan section 3). `just constitution-check` compares the
`sl-data` block with the plan table and fails with R-CONST-FRAG when they differ.

## Normative text

- GOV6-R01: A release MUST pass only when every applicable invariant F1-F8 meets its threshold (master plan section 3).
- GOV6-R02: Each invariant MUST have a numerator, a denominator, a threshold and a named enforcement point (master plan section 3).
- GOV6-R03: Each exemption MUST be registered in SL-REG-006 (Exception Register), shown in the editor and reviewed every release (master plan section 3.1).
- GOV6-R04: An exemption MUST belong to one of the six allowed classes in the `exemptions` list below (master plan section 3.1).
- GOV6-R05: A metric over its threshold MUST be handled by the flow in `flow` below: exempt, then by phase (master plan section 3.2).
- GOV6-R06: The editor MUST surface the scorecard items in `scorecard` below (master plan section 3.3).

```sl-data
{"fragmentation": [
{"id": "F1", "invariant": "No manual export/import in native workflows", "numerator": "Workflow-test steps requiring an external app", "denominator": "total steps, per discipline acceptance workflow", "threshold": "0", "enforcement": "Workflow acceptance suite (Vol 21) + build graph rejects undeclared inputs", "episodes": ["SL-EP-003", "SL-EP-200", "SL-EP-220", "SL-EP-408"], "verified_by": "pending"},
{"id": "F2", "invariant": "No duplicate authoritative representations", "numerator": "Data domains with >1 registered owner", "denominator": "all domains", "threshold": "0", "enforcement": "Identity registry rejects a second ownership claim", "episodes": ["SL-EP-066", "SL-EP-079"], "verified_by": "pending"},
{"id": "F3", "invariant": "Complete provenance", "numerator": "Derived artifacts missing any of: input IDs, input revisions, toolchain ID, parameters, environment hash", "denominator": "all derived artifacts", "threshold": "0", "enforcement": "Artifact store refuses writes without the provenance record", "episodes": ["SL-EP-055", "SL-EP-182", "SL-EP-434"], "verified_by": "just provenance-check"},
{"id": "F4", "invariant": "No stale artifact shown as current", "numerator": "Artifacts served while an input revision changed or is unverifiable", "denominator": "artifacts served", "threshold": "0", "enforcement": "Currency check on every artifact read", "episodes": ["SL-EP-187", "SL-EP-200"], "verified_by": "pending"},
{"id": "F5", "invariant": "Every edit is transactional", "numerator": "Mutations bypassing the transaction manager", "denominator": "all mutations (instrumented in debug builds)", "threshold": "0", "enforcement": "Mutation APIs only reachable through the transaction manager", "episodes": ["SL-EP-070", "SL-EP-116", "SL-EP-390"], "verified_by": "pending"},
{"id": "F6", "invariant": "Observations are traceable", "numerator": "Trace events missing correlation fields", "denominator": "all events; process-wide events carry a scope tag instead of an entity", "threshold": "0 missing required fields", "enforcement": "Trace schema validator", "episodes": ["SL-EP-301", "SL-EP-319"], "verified_by": "pending"},
{"id": "F7", "invariant": "Minimal rebuild scope", "numerator": "Rebuilt nodes", "denominator": "dependency-closure nodes on the reference workload set", "threshold": "≤ 1.05 (baseline set in ep. 198)", "enforcement": "Build graph report", "episodes": ["SL-EP-186", "SL-EP-198"], "verified_by": "pending"},
{"id": "F8", "invariant": "No external tool needed for final content in core disciplines", "numerator": "Core disciplines whose acceptance workflow needs an external app", "denominator": "core disciplines", "threshold": "0 at 1.0 for every core discipline", "enforcement": "Discipline acceptance suites", "episodes": ["SL-EP-220", "SL-EP-240", "SL-EP-260", "SL-EP-280"], "verified_by": "pending"}
],
"exemptions": ["console certification", "platform signing", "specialist simulation solvers", "legal or regulatory validation", "interchange import/export", "AI model training"],
"flow": [
{"step": "metric", "text": "Metric computed in CI or editor"},
{"step": "threshold", "text": "Over threshold? no: scorecard green"},
{"step": "exemption", "text": "Registered exemption? yes: shown as declared exception"},
{"step": "authoring", "text": "No exemption, phase authoring: editor warning + diagnostic record"},
{"step": "local-validation", "text": "No exemption, phase local validation: configurable fail"},
{"step": "release", "text": "No exemption, phase release: release blocked"}
],
"scorecard": [
"Native-authored content coverage per discipline",
"Provenance completeness (F3)",
"Over-invalidation ratio (F7)",
"Stale-artifact incidents (F4)",
"Transaction coverage (F5)",
"Cross-process boundary count and latency cost",
"External-tool dependency surface, with each exemption listed"
]}
```

## Measures and enforcement points

- Each F row above is a measure: numerator and denominator as in master plan section 3, threshold as listed. `verified_by: pending` means its enforcement point is not built yet; `just contracts-pending` lists these.
- Plan agreement: numerator = F rows differing from the master plan section 3 table in id, invariant, measurement, threshold, enforcement or episodes; denominator = 8; threshold 0; gate `just constitution-check`, rule R-CONST-FRAG.
- The exemption register SL-REG-006 is written in WO-009; the exemption gate is `just exception-check` (stub until then).
- F1-F8 enforcement points run in later volumes (Vol 21 workflow suites, identity registry, artifact store, build graph report); none exists in Volume 01.

## Traceability

Master plan sections 1.3, 3, 3.1, 3.2 and 3.3; episode SL-EP-003 (doctrine and F1-F8 measurement); the F-row episodes
above; SL-GOV-001 (doctrine), SL-REG-006 (exceptions); work order WO-006.

## Open issues

- The master plan section 3.2 flow is a flowchart; the `flow` list is a linear reading of it. Its node labels are shortened, not changed in meaning.
- The plan does not define how "total steps", "all domains" or the "core disciplines" are counted; the owning episodes (200, 220, 066, 079) decide.
- The `verified_by` field is this document's own marker (master plan section 3 names enforcement points, not recipes); it stays pending until each point exists.
- The master plan lists six exemption classes without defining how a class is proven; SL-REG-006 (WO-009) decides.

## Change log

| Version | Date | Change |
| :---- | :---- | :---- |
| 0.1.0 | 2026-10-17 | First draft (WO-006). |
