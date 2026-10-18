# tests

`unit/` (doctest, `sl_tests`), `gates/` (negative tests `neg_*.sh` proving each gate can fail), and
`governance/` (fixtures for the governance tools). The Python tool tests live next to the tools in
`tools/governance/test_*.py` and run with `just tools-test`.
