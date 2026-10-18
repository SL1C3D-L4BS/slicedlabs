# engine/fabric

The Fabric: the canonical project model of master plan Volume 04 (SL-ARCH-001 data side, SL-ARCH-002 transactions,
undo and recovery). Target `sl_fabric` (alias `sl::fabric`), namespace `sl::fabric`. Every fallible call returns
`sl::result<T>` with a code of the fabric error range 5000-5999 (ADR-0009, ADR-0010); persisted-path code has no
`throw`, `try`, `.at()`, `std::sto*` or unordered container (FAB-INV-CF-04).

Layout (WO-037 fixed the build files; later work orders only add files):

| Path | Contents | Owner |
| :---- | :---- | :---- |
| `include/sl/fabric/` | public headers | each area's work order |
| `include/sl/fabric/internal/` | headers shared inside the library, not API | each area's work order |
| `src/` | sources, globbed into `sl_fabric` | each area's work order |
| `gen/` | schema-generated code (`gen/include`, `gen/src`) | WO-045 |
| `testing/` | `sl_fabric_testing`, helpers for the fabric tests | WO-037, then test work orders |
| `tools/` | one developer executable per `tools/<name>.cpp` | later work orders |

Areas (header and source stem, test label `fabric_<area>`, `just fabric-test <area>`):

| Area | Files | What | Owner |
| :---- | :---- | :---- | :---- |
| id | `id.hpp`, `src/id.cpp` | record ids, classes, mint schemes, id sources (ADR-0011) | WO-037 |
| digest | `digest.hpp`, `src/digest.cpp`, `src/sha256.cpp` | in-house SHA-256, object ids, frame header (ARCH1-R09) | WO-037 |
| bytes | `bytes.hpp`, `src/bytes.cpp` | byte writer and bounded reader, strict LEB128 | WO-037 |
| clock | `clock.hpp`, `src/clock.cpp` | injected clock, system and manual clocks (ADR-0019) | WO-037 |

Later areas (values, codec, schemas, store, transactions and the rest) are listed in SL-ARCH-001 "API surface" and
SL-ARCH-002 with their owning work orders WO-039 to WO-063.

Build: Debug and sanitizer builds define `SL_FABRIC_F5_INSTRUMENT=1` publicly (ADR-0016). Tests live in
`tests/unit/fabric` and `tests/integration/fabric`.
