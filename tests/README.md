# tests

`unit/` (doctest, `sl_tests`), `gates/` (negative tests `neg_*.sh` proving each gate can fail), and
`governance/` (fixtures for the governance tools). The Python tool tests live next to the tools in
`tools/governance/test_*.py` and run with `just tools-test`.

Tiers (WO-026, episode 051): `unit/`, `integration/`, `system/`, `e2e/` registered through `cmake/SlTesting.cmake`
(`sl_add_test`, per-tier timeouts, ctest labels); `just test-tiers`, `just test-report`. Fixtures and golden data: `fixtures/`, `golden/` (WO-027).
