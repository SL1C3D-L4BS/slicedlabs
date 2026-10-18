# tests/fixtures/fabric/types

Fixtures of the schema-generated types (WO-045, ADR-0013, SL-ARCH-001 ARCH1-R69). Text and JSON only; relock the
manifest after a change with `python3 -I tools/testing/fixtures.py lock`.

| File | Holds |
| :---- | :---- |
| `<module>/<type>.json` | `{format "sl-type-fixtures/1", type, values[...]}`: two or more values of the type in canonical text form (ARCH1-R29), checked by `just fabric-gen-check` (R-GEN-FIXTURE) and round tripped through the generated typed structs by FAB-INV-GEN-02 |
| `lock_hashes.json` | `{format "sl-gen-lock-hashes/1", hashes {type: "sha256:..."}}`: `schemalib.lock_hash` of every type of `schemas/fabric`, written by `tools/fabric/slgen.py generate`; FAB-INV-GEN-03 checks the C++ schema object ids against it |
| `text_corpus.json` | `{format "sl-text-corpus/1", cases[{type, value, accept, why}]}`: values the C++ validator (from_text, then check) and the generated text JSON Schema must judge alike (FAB-INV-GEN-05) |
