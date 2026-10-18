# tests/unit/fabric

Fabric unit tests (tier `unit`, WO-037 wiring). Rule: each `<stem>.cpp` whose name ends in `_test.cpp` becomes the
doctest executable and ctest test `fabric_<stem>` with the labels `unit`, `fabric` and `fabric_<area>`, where
`<area>` is `<stem>` up to its first `_` (`id_text_test.cpp` -> `fabric_id`). `just fabric-test <area>` runs one
area. One file per invariant row; each TEST_CASE name starts with its FAB-INV id (SL-ARCH-001, SL-ARCH-002).
`SL_FABRIC_FIXTURES_DIR` points at `tests/fixtures/fabric`; sanitizer builds set `SL_FABRIC_SWEEP_SCALE=0.1`.
