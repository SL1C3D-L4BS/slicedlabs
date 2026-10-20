# docs/adr

Architecture decision records `ADR-NNNN-<slug>.md` (template: `docs/templates/ADR.md`), at most 80 lines each,
checked by `just adr-check`. ADR-0001 to ADR-0007 are written by WO-003 as Proposed; the founder flips them to
Accepted. `docs/gov/SL-GOV-004-adr-log.md` is generated from this directory: after changing an ADR run
`python3 -I tools/governance/cmd_gen_adr_log.py` (`adr-check` fails while the log is stale). Governed path
(FACTORY.md). ADR-0022 to ADR-0028 (Volume 05: runtime error domain and fault injection, memory model, concurrency and
sanitizer policy, archetype ECS storage, determinism contract D1, runtime side of the document boundary, lifecycle,
recovery, swap protocol and the P0 gate) are written by WO-075 as Proposed; the founder flips them to Accepted before
WO-076 starts.
