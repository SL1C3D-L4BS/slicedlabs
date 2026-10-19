---
id: ADR-0017
title: "Ownership and authority: one owner per domain, twelve reserved domains, innermost-domain authority"
status: Accepted
date: 2026-10-18
decision_refs: []
supersedes: []
---
# ADR-0017: Ownership and authority: one owner per domain, twelve reserved domains, innermost-domain authority

## Context

F2 requires data domains with more than one registered owner over all domains to be 0, enforced by "identity registry
rejects a second ownership claim" (master plan section 3). Episode 066 asks which subsystem owns each record and who may
mutate it (master plan, Volume 04). The twelve subsystems and what each owns are fixed by master plan section 2.2.
Agents reach the transaction layer only through governed automation (master plan section 2.1; section 1.5, row AI).

## Decision

- A data domain MUST be a dotted name under one of twelve reserved prefixes, one per subsystem of master plan section 2.2: `shell` 01, `fabric` 02, `runtime` 03, `gfx` 04, `content` 05, `sim` 06, `code` 07, `asset` 08, `obs` 09, `collab` 10, `verify` 11, `gov` 12 (master plan section 2.2).
- A domain MUST have exactly one owner subsystem; a second claim with a different owner is rejected with `ownership_conflict` naming both claimants, and the rejection is counted (master plan section 3, F2).
- Every record type MUST belong to exactly one domain (`ownership_orphan` otherwise), and its domain, state class and record class never change (master plan section 3, F2).
- The registry MUST freeze when the first workspace opens; later claims fail with `registry_frozen` (master plan section 3, F2).
- State classes MUST be document, derived and observation only; `runtime.*` admits observation records (captures) and never document records, because live simulation state is transient (master plan section 2.2, row 03; episode 074).
- Editor layouts and the command registry MUST be document state of `shell.*` (subsystem 01), changed by transactions like any other fact (master plan section 2.2, row 01; section 2.1).
- Authority MUST be checked at the innermost typed value whose type declares a domain different from its parent, so a physics component inside a scene entity needs the `sim` grant as well as the `content` grant (master plan, Volume 04, episode 066).
- Actors MUST be human, tool, plugin, agent or service; agent and service actors are denied by default and need an explicit grant (master plan section 1.5, row AI; section 2.1).
- The F2 report MUST expose numerator, denominator and rejected claims; `just fabric-ownership-check` compares the C++ domain dump, the schema files and the SL-ARCH-001 domain table, and `just neg-fabric-f2` plants a second claim and requires both owners named (master plan section 3, F2).

## Consequences

- WO-048 builds the registry, the authority validator and both recipes; WO-062 adds the gate-1 measure (records with no
  owner, stable id or revision over records = 0).
- Game UI is authored content and lives under `content.ui`, not `shell`.
- Ownership transfer between subsystems is out of scope; it needs a new ADR.

## Alternatives

- A prefix table that omits subsystems 03 and 04 and maps `ui.` to the editor shell (D1 draft): rejected, it
  contradicts master plan section 2.2.
- Authority at the record level only: rejected, nested components owned by other subsystems would bypass their owner.
- Allow-by-default for agents: rejected, master plan section 1.5 row AI routes every agent edit through governance.
