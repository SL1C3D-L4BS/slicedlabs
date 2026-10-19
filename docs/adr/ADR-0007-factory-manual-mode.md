---
id: ADR-0007
title: "Factory manual mode until wo-pipeline and land exist"
status: Accepted
date: 2026-10-17
decision_refs: []
supersedes: []
---
# ADR-0007: Factory manual mode until wo-pipeline and land exist

## Context

Claude Code works through bounded work orders; its report of done is not evidence, the test run is (master plan
section 6.3 rule 4). Machine fact (plan of record, 2026-10-17): the factory has `wo-lint`, `wo-verify`, `approve.py`,
`worktree.sh`, agents and hooks; `wo-new`, `wo-run`, `wo-pipeline`, `run-wo` and `land` do not exist.
SL-PLAN-003 Work-Order Protocol is a required document (master plan, Required documents and registries).

## Decision

Until `wo-pipeline` and `land` exist, this slice runs in manual mode. The commander writes `work-orders/WO-NNN.md`
and runs `wo-lint`; the founder approves; a builder works in a worktree (master plan section 6.3 rule 4: one episode,
declared files, declared tests). The founder runs `wo-verify`, `approve.py`, `git merge --no-ff`, `evidence-record`
and `ep-advance`. Nobody pushes mid-slice; agents never push (machine fact: plan of record, founder answer 3).
Completion needs recorded evidence (master plan section 6.2, Demonstrated to Accepted).
SL-PLAN-003 (WO-012) defines manual mode in full.

## Consequences

- WO-012 writes `scripts/close-out` and `scripts/evidence-record` for the founder.
- Manual mode ends when the missing factory commands exist; a later ADR supersedes this one.

## Alternatives

- Build `wo-pipeline` and `land` first: rejected, it delays the first governed work order; the plan takes manual mode.
- Agents merge their own work: rejected, an agent's report of done is not evidence (master plan section 6.3 rule 4).
