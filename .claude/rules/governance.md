---
paths: ["registry/**", "schemas/**", "deps/**", "third_party/**", "tools/governance/**", "tests/gates/**", "work-orders/**", "evidence/**", "justfile", "FACTORY.md"]
---
# Governance rules

- The registry is authoritative: `registry/SL-REG-002-episodes.json` (master plan 6) beats any prose or view
  that disagrees. Generated views are never hand-edited; regenerate them.
- IDs never change and are never reused (episodes, documents, requirements, work orders, ADRs). Supersede
  with a new id; do not renumber.
- Episode status moves only along master plan 6.2, through `ep-advance`, with evidence.
- Governed paths (`FACTORY.md` `governed:`) need the founder; never widen a work order's `paths_allowed`.
- Gate configs (`justfile`, `FACTORY.md`, `.clang-*`, presets, CI) change only in a work order with
  `touches_gates: true`. The `check: fast` line stays; the gate ratchets by extending `fast`.
- An unimplemented recipe is a stub (`just stub <name>`, exit 3) owned by a WO in
  `tools/governance/recipes.json`; flip it to `implemented` in the change that implements it.
- Tooling is Python 3 stdlib only, run as `python3 -I`, no subprocess, no network (ADR-0004).
- Every gate has a `neg-*` test proving it can fail and names the culprit.
