---
# Factory contract for this repo. Hooks and wo-verify read the COMMITTED main-branch copy only.
# Commands must be `just <recipe> [args]`; anything else is refused.
gate:
  fast: just fast           # stop-gate: runs after a turn that changed files (60 s)
  full: just check          # wo-verify: runs once per verify round
governed: [registry/**, schemas/**, deps/**, third_party/**, docs/adr/**, docs/gov/SL-GOV-001*, docs/gov/SL-GOV-006*, cmake/features/required-cxx23.txt, tests/gates/**, scripts/ci-local.sh, scripts/evidence-record, evidence/**, work-orders/**]
---
# FACTORY

Sliced Labs monorepo (SL-PLAN-001). Build and test: `just check`. Fast gate: `just fast`. Run one test: `just test <name>`.

Work orders live in `work-orders/WO-NNN.md` (frozen after landing; evidence binds to their sha256). Rules a builder must follow are in `.claude/rules/`. Gate configs (justfile, FACTORY.md, lint configs, CMake presets) change only when the work order sets `touches_gates: true`. Governed paths above need the founder (approve.py escalates).

Manual mode (ADR-0007): the commander writes the work order, the founder approves, a builder implements in a worktree, the founder runs `wo-verify`, `approve.py`, `git merge --no-ff`, `scripts/evidence-record` and `ep-advance`. Nobody pushes before the slice's first push.
