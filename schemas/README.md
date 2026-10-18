# schemas

JSON Schema (2020-12) for every registry and data file: `sl-reg-001..006`, `sl-doc-frontmatter`, `sl-wo-trace`, `sl-deps`, `sl-deps-lock`, `sl-provenance` (artifact provenance records, WO-029). Validated by the stdlib subset validator in `tools/governance/` (WO-002). Governed path: changes need the founder.
Also `sl-log-event` (structured log events, WO-030).
`fabric/` holds the Fabric schema source (`*.sltype.json` modules, their JSON Schema `sl-type-def.schema.json` and `schema.lock.json`; WO-040, see `fabric/README.md`).
