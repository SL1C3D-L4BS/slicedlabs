# tools/governance

Python 3 standard library only, run as `python3 -I`, no subprocess, no network (ADR-0004).

- `slgov.py`: shared parsers and checks. `cmd_<name>.py`: one thin command per gate.
- `recipes.json`: every justfile recipe with its owner WO and `stub|implemented` status.
- `layout.json`: the ADR-0001 directories that must exist with a README.
- `test_*.py`: unit tests, run by `just tools-test`.
- `slib.py` (WO-002): JSON loading (duplicate keys rejected), the JSON Schema subset validator (type, required,
  properties, additionalProperties, enum, pattern, items, const, local `$ref`, uniqueItems, plus minItems,
  minLength, minimum, maximum; any other keyword is refused), the lifecycle FSM (master plan 6.2), and the
  registry rule engine: 13 rule ids (`slib.RULES`), findings `{rule, path, message}`. `meaning_sha256(title,
  summary)` is the hash of `title\nsummary`. `cmd_check_fixture.py` runs the engine over fixture bundles;
  `cmd_registry_schema_selftest.py` is `just registry-schema-selftest`.
