---
id: ADR-0018
title: "Undo is a new transaction applying the stored inverse; per-actor stacks; barriers and compensations"
status: Accepted
date: 2026-10-18
decision_refs: []
supersedes: []
---
# ADR-0018: Undo is a new transaction applying the stored inverse; per-actor stacks; barriers and compensations

## Context

Episode 071 asks for reversible operations, grouping, and compensation for irreversible actions (master plan, Volume
04). History is one per fact (master plan section 1.1) and every edit is transactional (master plan section 3, F5), so
undo cannot rewind a head. Collaboration adds intervening edits by other actors (master plan section 4.2; episode 358).

## Decision

- Undo MUST be a new committed transaction that applies the inverse of the acting actor's last group; it carries `undo_of`, so history stays append-only (master plan section 3, F5).
- A normalised patch MUST carry `expect` (old value or order key) or `captured` (deleted record) on every mutating op, so its inverse is exact and needs no lookup (master plan section 2.4).
- Undo stacks MUST be per actor and rebuilt from tx records after reopen (master plan, Volume 04, episode 071).
- A group MUST undo as one transaction or not at all (master plan, Volume 04, episode 071 "grouping").
- When an intervening edit changed a value the inverse expects, undo MUST be refused with `undo_conflict` and a report; selective undo by transform is Volume 18 work (master plan section 4.2).
- Each op MUST declare reversible, compensable or irreversible; irreversible is a barrier that undo refuses to cross and names; compensable registers a compensation command run by a registered compensator as its own transaction bracketed by intent frames (master plan, Volume 04, episode 071 "compensation").
- Redo MUST be cleared by the actor's next non-undo commit; the undone revisions stay in history (master plan section 1.1).
- Coalescing MUST happen inside one open transaction: repeated sets of the same path keep the first `expect` and the last value, and a drag commits once on release (master plan section 2.4).

## Consequences

- WO-043 (normalize, invert, compose), WO-052 (undo manager). Delete-then-undo restores the same id and record oid,
  which the identity lifecycle (WO-058) relies on for the only permitted revival of a retired id.
- No revision per mouse move; an interrupted drag loses its uncommitted part (open issue: draft spill).

## Alternatives

- A global linear stack: rejected for multi-actor sessions (agents and users edit concurrently).
- Rewinding the head: rejected, it erases history and breaks F5.
- One revision per coalesced step with one undo entry (D1 draft): rejected for write amplification; history keeps
  the user-visible commits only.
