---
paths:
  - "docs/**"
  - "README.md"
  - "**/README.md"
---
# Documentation rules

- Cite the master plan (`docs/plan/SL-PLAN-001-master-plan.md`) by section for every normative sentence
  (for example "master plan 1.5", "6.2"); a sentence with no citation and no ADR is an Open issue, not a rule.
- `docs/plan/SL-PLAN-001-master-plan.md` is a byte copy pinned by sha256 (`.sha256` beside it). Never edit it;
  amendments go through the registry.
- Document ids are `SL-<KIND>-<NNN>` (GOV, PLAN, REG, VOL, ARCH, OPS). A doc keeps its id for life; titles may
  change, ids never do.
- ADRs live in `docs/adr/ADR-NNNN-<slug>.md` with front matter carrying at least `id` and `status`
  (`just adr-check`). Repo ADRs are separate from the dotfiles `D-NNN` decisions.
- Episode notes live in `docs/episodes/NNN.md`; templates in `docs/templates/`.
- Front matter uses the small YAML subset the tools parse: `key: value`, `[a, b]` lists, one nested level.
- Prose is plain: say what is true and what is unverified; no hype, no hedging filler.
- Every directory has a `README.md` stating its purpose and which work order or episode fills it.
