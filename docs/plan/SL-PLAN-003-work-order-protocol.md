---
id: SL-PLAN-003
kind: PLAN
title: "Claude Code Work-Order Protocol and prompt templates"
status: Draft
version: 0.2.0
owner: founder
episodes: [012, 014, 053]
depends_on: []
supersedes: []
decisions: [ADR-0007, ADR-0003, ADR-0004]
---
# Claude Code Work-Order Protocol and prompt templates

## Purpose

This document defines how Claude Code builds the Sliced Labs monorepo through bounded work orders in manual mode,
so that an agent's report of "done" is never the evidence and the test run is (master plan section 6.3 rule 4).
It implements the SL-PLAN-003 entry of the master plan's required documents and records ADR-0007.

## Status and decisions

Draft, owner founder, who approves it. Bound by ADR-0007 (manual mode until `wo-pipeline` and `land` exist),
ADR-0003 (Volume 03 work starts before Volume 01 is Accepted; its episodes may reach Demonstrated, never Accepted)
and ADR-0004 (stdlib-only tooling, no subprocess or network in gates). Manual mode ends when a later ADR supersedes
ADR-0007. Roles: the founder owns goals, spec approval for class L or any risk flag, spend and configs; the commander
(main session) writes and classifies work orders and never writes product code; builders and reviewers are workers.

## Normative text

Rule ids are `PLAN3-Rnn`. Manual-mode steps, in order: 1 commander writes `work-orders/WO-NNN.md` and lints it;
2 founder approves when R05 requires it; 3 builder implements; 4 reviewer reads (optional for class S);
5 founder runs `scripts/close-out WO-NNN` (verify, approve, merge, record, advance). Nobody pushes (R11).

- PLAN3-R01: Until `wo-pipeline` and `land` exist, every work order MUST follow the manual-mode steps above (ADR-0007).
- PLAN3-R02: A work order is one file `work-orders/WO-NNN.md`: front matter in the factory template
  (`~/.dotfiles/templates/factory/WO.md`: id, class, repo, risk, goal, non_goals, acceptance, paths_allowed,
  touches_gates, budget, approver), then a body. Per ADR-0007 and factory design section 8, the commander MUST run
  `wo-lint` and `just wo-check` before asking for approval.
- PLAN3-R03: Work-order ids are `WO-` and three digits, assigned in order by the commander, `repo: slicedlabs`.
  Per master plan section 6.3 rules 2 and 4, an id MUST NOT be reused or renumbered, and a landed work order is
  frozen (evidence binds to its sha256, R12), so a change is a new work order.
- PLAN3-R04: Per master plan section 6.3 rule 3 and schema `sl-wo-trace`, the body MUST carry exactly one line
  `Traces: SL-EP-nnn [SL-EP-nnn ...]` naming every episode the order serves, each id well formed and, once
  `registry/SL-REG-002-episodes.json` exists, present in it. One order serves one coherent change; never several unrelated episodes.
- PLAN3-R05: Class and approver. Class is S, M or L. Approver `auto` is allowed only for class S or M with risk
  `[none]`. Per factory design section 8, G1, class L or any risk flag (gpu, governed, deps, ci, security, data,
  deploy) MUST be approved by the founder before a builder starts. The `fable` approver runs only through the fable gate (factory design section 8, G1).
- PLAN3-R06: Budget caps are S 300000 tokens and $3, M 1000000 and $8, L 1500000 and $20, rounds at most 2, and at
  most 3 builders live at once. A work order above its class cap is invalid (factory design section 2; `just wo-check`).
- PLAN3-R07: Per factory design section 8, invariant 4, every acceptance entry MUST be `just <recipe> [args]` with
  `expect_exit`, naming a recipe that exists in the justfile and carrying no shell syntax; an unimplemented recipe
  is a stub that exits 3 (master plan section 6.3 rule 4).
- PLAN3-R08: A builder works one work order, only in `paths_allowed`, from `docs/templates/prompt-builder.md`. It runs
  in a worktree (`~/.local/state/worktrees/slicedlabs/<slice>`) or, while worktrees are unavailable, in the main
  checkout (direct mode). It MUST NOT merge, push or widen scope, and it reports in 40 lines or fewer (ADR-0007).
- PLAN3-R09: A reviewer is a fresh, read-only session from `docs/templates/prompt-reviewer.md` that returns
  `VERDICT: PASS|CHANGES|ESCALATE`. Per master plan section 6.3 rule 4 a review is never evidence of completion, and
  factory design section 2 allows at most 2 rounds.
- PLAN3-R10: The founder closes out with `scripts/close-out WO-NNN [--branch B --worktree D | --direct]`, which prints
  and, after each confirmation, runs: `wo-verify`, `approve.py`, `git merge --no-ff` (skipped with `--direct`),
  `scripts/evidence-record`, then `ep-advance` for each `--advance SL-EP-nnn=State`. Per ADR-0007 a failed step
  stops the sequence, and an approve.py error is never an approval.
- PLAN3-R11: Per ADR-0007 and the machine fact in the plan of record (founder answer 3), agents MUST NOT push,
  force-push or merge; nobody pushes before the slice's first push, and then only after the gates pass.
- PLAN3-R12: The evidence record is `evidence/WO-NNN.json`, written by the founder through `scripts/evidence-record`
  and never by an agent or by hand: `{wo, wo_sha256, merge_commit, branch_tip_tree, gate_log_sha256|null, pass, date}`.
  `wo_sha256` is sha256 of the committed work-order file, `pass` comes from the `wo-verify` evidence, and an existing
  record MUST NOT be overwritten (master plan section 6.2, Demonstrated to Accepted; ADR-0007).
- PLAN3-R13: Episode status moves only through `ep-advance <id> <state> <wo>`, along master plan section 6.2 and
  appending a history entry `{from, to, date, note}`. Per rule R-NO-EVIDENCE and master plan section 6.2,
  the states Demonstrated and Accepted need an evidence record with `pass` true whose `wo_sha256` equals the current hash.
- PLAN3-R14: Entering InProgress or later requires every dependency Accepted (R-DEP-ACCEPTED, master plan section 6.2).
  The only exception is `--adr ADR-nnnn` naming a waiver in the registry that covers the episode and the target state,
  for example ADR-0003, which stops at Demonstrated (ADR-0003).
- PLAN3-R15: An episode record follows `docs/templates/EPISODE.md`, the twelve fields of master plan section 6.1,
  and agrees with the registry, which is authoritative (master plan section 6.3 rule 1).
- PLAN3-R16: A blocked hook means stop and ask the founder. Guard files and gate configs change only by a patch that
  the founder applies, or by a work order with `touches_gates: true` (factory design section 8, invariant 5).
- PLAN3-R17: Per ADR-0007 and the founder's standing rules of 2026-10-19, a wave of work orders MUST pass, besides
  each order's own acceptance commands, `just fast`, `just tidy`, `just test`, `just workflow ci-gcc` and
  `just workflow ci-release` on the merged tree before it is committed (ADR-0007); a builder PASS without those five
  is not a PASS (ADR-0007). Any commander change, however small, MUST have its own fast-lane work order (class S)
  before it is committed (ADR-0007; factory design section 8).

## Measures and enforcement points

| Rule | Measure | Enforced by |
| :---- | :---- | :---- |
| R02, R03, R05, R06, R07 | work orders passing / all `work-orders/WO-*.md`, threshold 100% | `just wo-check` (R-WO-FRONT, -CLASS, -BUDGET, -ACCEPT) |
| R04 | work orders with well-formed `Traces:` ids / all, threshold 100% | `just wo-check` (R-WO-TRACES, R-WO-REGISTRY) |
| R10, R12 | evidence records whose objects exist / all records | `just evidence-verify` (`git cat-file -e`, founder run) |
| R13, R14 | refused illegal transitions / attempted, threshold 100% | `just ep-advance`, `tools-test` (test_ep_advance) |
| R08, R09, R11, R16 | builder and reviewer prompts used; no push by an agent | prompt templates, hooks, review at close-out |

`just ep-advance <id> <state> <wo> [--adr ADR-nnnn] [--date YYYY-MM-DD]` runs `cmd_ep_advance.py`;
`just evidence-record WO-NNN [gate-log] [--direct]` runs `scripts/evidence-record`.

## Traceability

Master plan sections 6.1 to 6.3. Episodes SL-EP-012, SL-EP-014 and SL-EP-053 (WO-012). ADR-0003, ADR-0004, ADR-0007.
Templates: `docs/templates/EPISODE.md`, `prompt-builder.md`, `prompt-reviewer.md`. Tools: `cmd_wo_check.py`,
`cmd_ep_advance.py`, `scripts/close-out`, `scripts/evidence-record`, `scripts/evidence-verify`.

## Open issues

- The factory `wo-lint`, `wo-verify` and `approve.py` live in `~/.dotfiles/tools/factory` and are guard files; this
  document describes their use, not their internals, and the factory design sections cited are unverified here.
- `ep-advance` writes the registry in the layout of `cmd_gen_episodes.py` (indent 1, sorted keys); a generator change breaks that match.
- Direct mode (no worktree) records `branch_tip_tree` as the tree of HEAD, not of a merged branch tip.
- Class L reviewer and Fable-gate use are not specified here; the founder sets the Max plan allowance first.

## Change log

| Version | Date | Change |
| :---- | :---- | :---- |
| 0.1.0 | 2026-10-17 | First draft (WO-012). |
| 0.2.0 | 2026-10-19 | PLAN3-R17: the per-wave gate (fast, tidy, test, ci-gcc, ci-release) and fast-lane work orders for commander changes (WO-067). |
