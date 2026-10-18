# tools/fabric

Python tooling for the Fabric (Volume 04, SL-ARCH-001 and SL-ARCH-002): checkers such as the contract, surface and
golden checks that later Volume 04 work orders add (see `tools/governance/recipes.json`, owners WO-036 to WO-063).
Python 3 stdlib only, run as `python3 -I`, no subprocess, no network (ADR-0004). Tests are `test_*.py` files here;
`just tools-test` discovers them (this directory is a package so discovery reaches it).

The C++ Fabric tests run through `just fabric-test <area>` (CTest label `fabric_<area>`), not from this directory.
Created empty by WO-035.
