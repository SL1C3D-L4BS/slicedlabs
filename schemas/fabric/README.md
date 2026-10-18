# schemas/fabric

Fabric schema source (ADR-0013, SL-ARCH-001 Schemas and versioning, WO-040). Governed path: changes need the founder.
Nothing else defines a Fabric type (ARCH1-R19).

| File | Holds |
| :---- | :---- |
| `<module>.sltype.json` | one module: `{format "sl-type-def/1", module, domain, owner "01".."12", doc?, types[...]}` |
| `sl-type-def.schema.json` | JSON Schema of a module file (stays inside the `tools/governance/slib.py` subset) |
| `schema.lock.json` | `{"format": "sl-schema-lock/1", "released": {<type>: {"<major.minor>": {"hash", "definition"}}}}` |

There are no system modules yet, so the lock is empty. WO-045 adds `fabric.*.sltype.json` (owner 02) and WO-063 pins the
Volume 04 set as released 1.0. The fixture modules in `tests/fixtures/fabric/schema/modules/` exercise every type kind.

## Module and type

A module file is named `<module>.sltype.json`; its `domain` has at least two segments and its first segment is one of
the twelve reserved prefixes of the SL-ARCH-001 `domain_registry` (shell 01, fabric 02, runtime 03, gfx 04, content 05,
sim 06, code 07, asset 08, obs 09, collab 10, verify 11, gov 12). `owner` must be the registry owner of that prefix; a
second owner for one domain is R-OWN-DUP. Each type name is `<module>.<local>`.

A type: `name, kind (struct|enum|variant|list|map|oset|tagged), version "major.minor", record_class, state_class`,
optional `domain` (the module domain or a child of it; default the module domain), `document_root`, `doc`, and by kind:

| Kind | Members |
| :---- | :---- |
| struct | `fields[{no, name, type, presence required\|optional, default?, since_minor?, deprecated_in?, ref?, doc?}]`, `reserved[numbers]` |
| enum | `enumerators[{no, name, since_minor?, deprecated_in?, doc?}]` |
| variant | `cases[{no, name, type, since_minor?, deprecated_in?, doc?}]` |
| list, oset, tagged | `element` (a scalar or a type name); tagged is a named newtype of its base |
| map | `key` (string, id or an integer type) and `element` |

A field `type` is a scalar (bool, i8..i64, u8..u64, f32, f64, string, bytes, id, digest, blob_ref, external_ref), a type
name, or `ref` with `ref {strength weak|strong|owned, targets[record type names]}`. Containers are always named
types. `record_class` is `none` or an id class name (a record type is a struct); `state_class` is document, derived or
observation. A `default` is allowed on optional bool, integer, float, string and enum fields (an enum default is the
enumerator number). Field, enumerator and case numbers are u32; a removed field number goes to `reserved` for ever.

## Lock hash (the schema object id)

`lock_hash` is the object id of kind 9 (major 1, minor 0, flags 0) over the SLB/1 bytes of the SL-ARCH-001
`type_def_value` (`tools/fabric/schemalib.py`; the C++ codec must agree, vectors in
`tests/fixtures/fabric/schema/lock_vectors.json`). Encodings the ARCH document leaves open: strings for name, kind,
domain, record_class, state_class, presence and ref strength; `reserved` ascending; fields, enumerators and cases
sorted by number; optional keys (5, 7, 8, 13, 14) absent when unset; a missing `since_minor` is 0 and a missing `doc` is
the empty string; the type-level `doc` is not hashed; enumerator = `{1 no, 2 name, 3 doc, 4 since_minor, 5 deprecated_in?}`,
case = `{1 no, 2 name, 3 type, 4 doc, 5 since_minor, 6 deprecated_in?}`.

## Rules (`just fabric-schema-check`)

`R-SCH-FORMAT`, `R-SCH-NAME`, `R-SCH-WELLFORMED`, `R-SCH-LOCK`, `R-SCH-RELEASED` (a released type changed, fell or left),
`R-SCH-BUMP` (the class needs a bigger bump), `R-SCH-REUSE` (reserved number reused), `R-SCH-RESERVE` (removed field not
reserved), `R-SCH-FORBIDDEN` (domain, state class or record class changed), `R-SCH-TRANSIENT`, `R-OWN-DUP`,
`R-OWN-ORPHAN`, `R-OWN-STATE` (details in the `tools/fabric/schema_check.py` docstring). `just neg-fabric-schema` proves
R-SCH-RELEASED and R-SCH-REUSE fail naming the type.

Releasing: edit the source (bump the version by the class that `schema_compat.py` reports), then
`python3 -I tools/fabric/schema_check.py lock` pins every unreleased (type, version) after a clean check. A lock entry
also holds the normalized `definition`, because the bump check compares the next version with the last released one.
Any edit of a released definition changes its object id, so even a label or doc edit needs a new minor version.
