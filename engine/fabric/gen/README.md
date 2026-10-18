# engine/fabric/gen

Schema-generated code and metadata (WO-045, SL-ARCH-001 ARCH1-R67 and ARCH1-R68, ADR-0013), written by
`tools/fabric/slgen.py` from `schemas/fabric/*.sltype.json`. Never edit these files by hand:
`python3 -I tools/fabric/slgen.py generate` rewrites them and `just fabric-gen-check` regenerates them in memory and
fails naming every stale, missing or extra file. Both C++ directories are on the `sl_fabric` include path and source
glob (WO-037). Generated C++ is clang-format clean, warning-free and passes the nothrow lint.

| Path | Holds |
| :---- | :---- |
| `include/sl/fabric/gen/<module>.gen.hpp` | typed C++ values of every type of the module (namespace `sl::fabric::gen::<module with _>`): `enum class` for enums, structs for structs, a `std::variant` holder (`alt`) for variants, aliases for list, map, oset and tagged; `<type>_to_value` / `<type>_from_value` returning `sl::result`; `register_types()`, `types()` |
| `src/<module>.gen.cpp` | the descriptor table of every type (registered into a `schema_registry`) and the conversions; from_value is the validator table: kind, integer range, enumerator, case, required and undeclared field checks |
| `include/sl/fabric/gen/support.gen.hpp`, `src/support.gen.cpp` | shared helpers: `oset_item`, `external_ref`, `type_entry`, `lock_hash()`, the conversion and descriptor builders |
| `include/sl/fabric/gen/system.gen.hpp`, `src/system.gen.cpp` | `register_system_types()`, `system_registry()` (frozen), `system_types()` over every module |
| `meta/<module>.meta.json` | inspector metadata: every type and field with doc, domain, owner, state class, ref targets, lock hash |
| `text/<module>.text.schema.json` | JSON Schema 2020-12 of the canonical text form (ARCH1-R29) of the module's types and the types they reach |

A descriptor keeps its state class only on record types: in `type.hpp` a state class marks a record type, while
sl-type-def/1 gives every type one. `type_entry::declared_state` keeps the declared class and `gen::lock_hash()`
restores it, so the C++ schema object id equals the Python lock hash (FAB-INV-GEN-03). Fixtures of every type live in
`tests/fixtures/fabric/types/<module>/<type>.json` (two or more canonical text values each, checked by
`fabric-gen-check`).
