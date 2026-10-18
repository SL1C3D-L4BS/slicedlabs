# third_party

Vendored, unmodified upstream sources, each directory with a `MANIFEST.json` generated from `deps/deps.lock.json` (`just deps-lock`; per-file sha256 and tree hash). A file here that no manifest covers fails `just deps-check`. Licences must be in the SL-GOV-008 allowlist. Governed path.

Now: `cpm/CPM.cmake` 0.43.2 (MIT), copied from the local CPM cache; `verified_upstream` is false until the founder compares it with the upstream release. `doctest/doctest/doctest.h` 2.5.3 (MIT), vendored by WO-018 from the Arch `doctest` package with `just deps-vendor doctest` (provenance `arch-repackage`, sha256 cfd518a3…; `verified_upstream` false until the founder compares it with the upstream v2.5.3 release).
