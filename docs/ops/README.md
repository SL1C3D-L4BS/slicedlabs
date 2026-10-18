# docs/ops

Operations references `SL-OPS-001` to `SL-OPS-010` (master plan, Required documents and registries): CI and quality
gates, supply chain, security response, privacy, backup, release, benchmarks, agent permissions, support and live
operations. None is written yet: all ten are Planned in SL-REG-001 (`registry/SL-REG-001-documents.json`) and no
`SL-OPS-*.md` file exists here. Until one is written and registered, the operational facts live in the gate recipes
(`just --list`, `FACTORY.md`), `docs/build/README.md` and the governance documents under `docs/gov`.

| Id | Title | Target volume(s) | Status |
| :---- | :---- | :---- | :---- |
| SL-OPS-001 | CI and Quality Gates | 03 | Planned |
| SL-OPS-002 | Dependency and Supply-Chain Operations | 03, 24 | Planned |
| SL-OPS-003 | Security Response | 24 | Planned |
| SL-OPS-004 | Privacy and Telemetry | 24 | Planned |
| SL-OPS-005 | Backup and Disaster Recovery | 23 | Planned |
| SL-OPS-006 | Release Management | 22 | Planned |
| SL-OPS-007 | Performance and Benchmark Operations (hardware lab) | 26 | Planned |
| SL-OPS-008 | Agent Permission Policy | 20 | Planned |
| SL-OPS-009 | Support Operations | 25 | Planned |
| SL-OPS-010 | Live Operations Runbooks | 41 | Planned |

CI and quality gates (SL-OPS-001 seed, WO-028): [ci.md](ci.md) covers the gates, artifacts, re-runs and the Windows evidence route.

Documentation build and checks (WO-031): `just docs` writes `build/docs` (an index, the overview, the registries, the ADR
log, the dependency and licence table, the records and the API reference) and runs Doxygen on `engine/core/include` when
`doxygen` is installed, else writes a stub API page; `just docs-check` verifies every relative Markdown link and every cited
document, episode and ADR id (rules `R-DOCS-LINK`, `R-DOCS-ID`; sources in `tools/docs/`). Doxygen is not installed on every
machine; install it with `sudo pacman -Syu --needed doxygen`.
