# Founder scripts

Run by the founder only, from a normal terminal in `~/Projects/slicedlabs`. Agents never run them (ADR-0020, PLAN3-R11/R12).

- `approve-001-080.sh`: the one-shot close-out for episodes 001-080 (evidence, ADRs, Constitution, lifecycle, signed commit). It refuses to start, and waits before each `wo-verify`, while the machine is busy (load average above `--max-load`, a live factory builder, or foreign cmake/ninja/ctest/compiler/linker processes); `--ignore-load` overrides, `--preflight-only` runs the checks and exits (WO-127). Summary and change list: `build/closeout/approval-summary.md`.
- `resign-history.sh`: re-sign every commit on main before the first push. Rewrites every hash; read its warning.
- `commit-wos`, `preflight.sh`: earlier founder helpers (commit grouping by work order; pre-flight).
