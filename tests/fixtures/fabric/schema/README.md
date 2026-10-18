# tests/fixtures/fabric/schema

Fixtures of WO-040 (schema source, lock and compatibility classifier). Text and JSON only; relock the manifest after a
change with `python3 -I tools/testing/fixtures.py lock`.

| File | Holds |
| :---- | :---- |
| `modules/*.sltype.json` | the seed set: three modules (content.shapes 05, sim.physics 06, runtime.capture 03), 12 types covering struct, enum, variant, list, map, oset, tagged, ref, defaults, a reserved number and a deprecated field. Not in `schemas/fabric/`, so the repository lock stays empty; tests and `neg-fabric-schema` copy them into a scratch tree |
| `compat_table.json` | `{format "sl-compat-table/1", rows[{case, change, old, new, expected}]}`: 32 rows over the 23 rows of the SL-ARCH-001 `compatibility` table (`change` is its text, `old` and `new` are types in module form, `expected` the class). WO-057 checks the C++ classifier against it |
| `lock_vectors.json` | per fixture type `{type, version, hex, hash}`: the SLB/1 bytes of its `type_def_value` and the `sha256:` lock hash. WO-041 and WO-045 check the C++ schema object id against it; `python3 -I tools/fabric/schemalib.py vectors write` regenerates |
