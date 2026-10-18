# tests/integration/fabric

Fabric integration tests (tier `integration`, WO-037 wiring). Rule: each `<stem>.cpp` whose name ends in
`_test.cpp` becomes the doctest executable and ctest test `fabric_<stem>` with the labels `integration`, `fabric`
and `fabric_<area>`, where `<area>` is `<stem>` up to its first `_` (`tx_recover_test.cpp` -> `fabric_tx`).
`SL_FABRIC_FIXTURES_DIR` points at `tests/fixtures/fabric`; sanitizer builds set `SL_FABRIC_SWEEP_SCALE=0.1`.
Empty until a later work order adds a test.
