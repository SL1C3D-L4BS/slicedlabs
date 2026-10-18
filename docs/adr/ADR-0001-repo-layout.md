---
id: ADR-0001
title: "Repository layout follows the master plan starter prompt"
status: Proposed
date: 2026-10-17
decision_refs: [D-126, D-135, D-141, D-144]
supersedes: []
---
# ADR-0001: Repository layout follows the master plan starter prompt

## Context

The master plan fixes the layout in its Starter prompt for Claude Code (episodes 041-048): engine/{core,runtime,render,fabric},
editor/, tools/, sdk/, tests/, docs/, third_party/, examples/ (master plan, Starter prompt, Layout constraint).
Dotfiles decisions D-126 (the narrow 2D-RPG engine scope) and D-135 (the monorepo layout with `sliced-engine/`)
predate the master plan. D-141 retired and wiped the first engine on 2026-10-17 (machine fact, plan of record).
D-144, filed by the founder in the dotfiles repository, records that the master plan replaces D-126 and D-135.

## Decision

- The repository layout MUST be the master plan's: engine/core, engine/runtime, engine/render, engine/fabric, editor/, tools/, sdk/, tests/, docs/, third_party/, examples/ (master plan, Starter prompt, Layout constraint).
- `platform/` is reserved and holds a README only; the web platform is specified in master plan section 2.8.
- Governance code lives in tools/governance, schemas in schemas/, registries in registry/, and controlled documents in docs/gov, docs/plan and docs/vol (plan of record, Repo layout; document kinds from master plan, Required documents and registries).
- Repository ADRs `docs/adr/ADR-NNNN-<slug>.md` are separate from the dotfiles decisions D-NNN; an ADR MAY name a D-NNN in `decision_refs` (plan of record, Decisions table, ADR-0001 row).
- Gates MUST NOT read the dotfiles decisions file (plan of record, Decisions table; the gate stays inside the repository).
- This ADR answers D-126 and D-135 as the master plan does (D-144); it does not edit them.

## Consequences

- `tools/governance/layout.json` lists the directories that must exist with a README; `just layout-check` enforces it.
- The founder files D-144 in the dotfiles repository and updates its layout line; builders do not touch the dotfiles.
- `engine/{runtime,render,fabric}` stay README-only until their volumes (master plan section 4.1).
- Old `sliced-engine/` paths are not reused; nothing from the wiped engine is imported (D-141).

## Alternatives

- Keep D-135's `sliced-engine/`, `sliced-studio/` layout: rejected, the master plan names one platform with one layout
  (master plan section 1.1).
- A flat `src/` tree: rejected, the starter prompt names the directories (master plan, Starter prompt).
