# tests/governance

Fixtures for the governance tools.

- `fixtures/good-minimal.json`: a clean bundle `{fixture, registry, documents, work_orders, plan_sha256, views}`.
- `fixtures/bad-<RULE>.json`: the good bundle with one planted defect; it must fail with exactly that rule id
  (`just neg-registry`, `just registry-schema-selftest`). One per rule in `slib.RULES`.
- `fixtures/validator-cases.json`: `{schema, instance, valid}` cases for the subset validator.

The real registry (`registry/SL-REG-002-episodes.json`, WO-004) is exercised by `just neg-registry` through
`cmd_check_episodes.py --mutation-selftest`: defects are planted in an in-memory copy and each must be named by its
rule id (R-SCHEMA, R-DUP-ID, R-GAP, R-VOLGRAPH, R-DEP-ORDER, R-LIFECYCLE, R-NO-EVIDENCE, R-SILENT-SCOPE, R-PLAN-SHA,
R-DEP-ACCEPTED) or golden assert (GOLDEN, LOCK). Nothing on disk is modified.
