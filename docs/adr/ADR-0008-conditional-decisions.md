---
id: ADR-0008
title: "Conditional decisions: ADR seeds from the Engine Constitution"
status: Proposed
date: 2026-10-17
decision_refs: []
supersedes: []
---
# ADR-0008: Conditional decisions: ADR seeds from the Engine Constitution

## Context

The heading of master plan section 1.5 reads "Locked, conditional and rejected decisions", but its table lists only
locked decisions; no conditional decision is listed (master plan section 1.5). SL-GOV-001 therefore holds
`conditional: []` and names three candidates under Open issues (SL-GOV-001 Open issues; master plan section 1.5).
Episode 020 approves and freezes SL-GOV-001 and records the conditional decisions (master plan, Volume 01, episode 020).

## Decision

This ADR records the candidates as seeds. A seed is not a decision: each needs its own ADR before it binds anyone.

- Seed 1, Engine language: "C++23 baseline; C++26 features promoted by toolchain feature tests" (master plan
  section 1.5, row Engine language). Open: the promotion rule, meaning which feature tests, which toolchains and who
  approves a promotion. Related: ADR-0005 and the C++26 report of WO-015.
- Seed 2, Graphics: "console APIs behind the same backend contract" (master plan section 1.5, row Graphics). Open:
  which console APIs, when each is conditional on a platform agreement, and what the backend contract must expose.
- Seed 3, Extension language: "Luau (typed, sandboxable, fast) for tools, mods and gameplay iteration; native C++ for
  engine and shipping hot paths" (master plan section 1.5, row Extension language). Open: the rule that decides which
  side of the Luau and native C++ split a given piece of code belongs to.
- The 19 locked rows of master plan section 1.5 are unchanged by this ADR; a change to a locked decision needs an
  ADR (master plan section 6.3, rule 3).
- The founder MAY replace these seeds with a list of conditional decisions; the plan lists none (master plan section 1.5).
- Episode 020 records the accepted conditional decisions in SL-GOV-001 (`conditional`) and freezes the document with
  `just constitution-frozen` (master plan, Volume 01, episode 020).

## Consequences

- WO-023 adds `constitution-frozen` (R-FROZEN-SHA, R-FROZEN-STATUS, R-FROZEN-LOCKED), which is inert until the
  founder approves SL-GOV-001 in `registry/SL-REG-001.approvals.json` (master plan section 6.2, Approved).
- Each seed that the founder accepts becomes its own ADR, numbered after ADR-0008, and is then listed in
  SL-GOV-001 (`conditional`); until then `conditional` stays empty (SL-GOV-001 Open issues).
- This ADR stays Proposed until the founder flips it (docs/templates/ADR.md).

## Alternatives

- Fix the three seeds as decisions now: rejected, the plan locks the rows but not the rules behind them (master plan section 1.5).
- Leave the seeds only in SL-GOV-001 Open issues: rejected, episode 020 records conditional decisions as ADRs (master plan section 6.3, rule 3).
