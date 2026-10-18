---
id: SL-GOV-009
kind: GOV
title: "Quality Model"
status: Draft
version: 0.1.0
owner: founder
episodes: [007, 016]
depends_on: []
supersedes: []
decisions: [ADR-0004]
---
# Quality Model

## Purpose

SL-GOV-009 defines the six qualities of Sliced Labs (reliability, performance, usability, accessibility, security,
compatibility) and, for each, the measure, the source of its threshold and the episode that gates it (master plan
episode 007; section 3 for the F1-F8 measures; section 6 and the release gates table for the seven gates).

## Status and decisions

Draft, owner founder, approver founder. ADR-0004 binds the stdlib-only tooling that checks it. This document sets no
number the master plan does not set; a threshold the plan leaves open is listed under Open issues (master plan
section 6.3, rule 3, on silent scope changes).

## Normative text

- GOV9-R01: Every quality MUST name a measure, a threshold source and a gate episode (master plan episode 007).
- GOV9-R02: A threshold MUST cite the master plan section or episode that sets it and MUST NOT be invented here (master plan section 3, "named enforcement point").
- GOV9-R03: A release MUST pass only when every applicable F1-F8 invariant meets its threshold (master plan section 3).
- GOV9-R04: Release gate 3 (engineering reliability) MUST pass with a qualification report before 1.0 (master plan, release gates table).
- GOV9-R05: A parity or speed claim MUST carry benchmark evidence from Volume 41 (master plan section 6.3, rule 5).
- GOV9-R06: A violation outside a registered exemption SHOULD follow the phase handling of section 3.2 (warning while authoring, configurable fail locally, release blocked).

The qualities, as data (master plan episode 007, sections 3.3 and 6):

```sl-data
{
  "qualities": [
    {"id": "reliability", "measure": "Reproducible builds, migrations, crash recovery, soak and security tests pass; stale artifacts served / artifacts served (F4)", "threshold_source": "Release gate 3 (Engineering reliability); F4 threshold 0 (section 3)", "gate_episodes": [417, 436]},
    {"id": "performance", "measure": "Rebuilt nodes / dependency-closure nodes (F7); SL-BENCH rows against Unreal Engine 5", "threshold_source": "F7 <= 1.05, baseline set in episode 198 (section 3); release gate 7 (Competitive validation)", "gate_episodes": [315, 316, 814]},
    {"id": "usability", "measure": "Workflow-test steps requiring an external app / total steps (F1); core disciplines needing an external app / core disciplines (F8)", "threshold_source": "F1 and F8 threshold 0 (section 3); release gate 2 (Production capability)", "gate_episodes": [200, 220, 408]},
    {"id": "accessibility", "measure": "Accessibility conformance tests: semantic metadata, keyboard access, scaling", "threshold_source": "Episode 110 and 477 acceptance suites; no numeric threshold in the master plan", "gate_episodes": [110, 477]},
    {"id": "security", "measure": "Security tests, pen test, security-report handling and patch policy", "threshold_source": "Release gates 3 and 5 (security tests, pen test); no numeric threshold in the master plan", "gate_episodes": [475, 479, 480, 739]},
    {"id": "compatibility", "measure": "Platform conformance, hardware matrix, schema and API compatibility checks", "threshold_source": "Release gate 4 (Platform readiness); platform list in section 2.6", "gate_episodes": [418, 463, 520]}
  ]
}
```

## Measures and enforcement points

- Reliability: gate 3 evidence is a qualification report; soak testing is episode 417, crash reporting 436. Not enforceable today (no engine code yet).
- Performance: F7 is reported by the build graph report (episodes 186, 198); budgets are episode 315, harnesses 316, audit 814.
- Usability: F1 is enforced by the workflow acceptance suite (Vol 21) and by the build graph rejecting undeclared inputs; F8 by discipline acceptance suites.
- Accessibility: episode 110 builds the foundations, 477 tests conformance; web accessibility is 717 and certification is 818.
- Security: episode 475 (crash and security reports), 479 (response and patch policy), 480 (gate), 739 (web pen test).
- Compatibility: hardware tests 418, API compatibility 463, project migration 478, portability gate 520.
- Editor scorecard (section 3.3) surfaces provenance completeness, over-invalidation ratio, stale-artifact incidents and transaction coverage.
- Document checks: `just doc-check` and `just registry-docs-check`.

## Traceability

Master plan episode 007 (quality model), episode 016 (development economics, context only), sections 2.6, 3, 3.2,
3.3 and 6, and the release gates table. Gate episodes above are the episodes whose descriptions name the matching
test or budget in the master plan Part lists.

## Open issues

- The master plan sets no numeric threshold for reliability, accessibility or security; gate evidence is "pass with recorded evidence".
- No release gate row names accessibility; it is gated through episodes 110, 477 and 818 only.
- Performance thresholds per target are set later by the budget episodes (315, 519, 579, 598, 676, 693, 737), not here.
- Episode 016 (development economics) is cited as context only; the master plan gives no cost measure for the six qualities.

## Change log

| Version | Date | Change |
| :---- | :---- | :---- |
| 0.1.0 | 2026-10-17 | First draft: six qualities with measure, threshold source and gate episodes (WO-010). |
