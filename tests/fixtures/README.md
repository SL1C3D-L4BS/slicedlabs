# tests/fixtures

Static test inputs (WO-019, WO-027). Small text and JSON files only; no binary assets yet, and no generated output (the cpm tarball is rebuilt under `build/` at test time by `deps/cpm/make_fixture.sh`).

- `MANIFEST.json` lists every file here (except itself) with its sha256 and size. `just fixtures-check` fails on drift (`R-FIX-DRIFT`), a missing file (`R-FIX-MISSING`), an unmanifested file (`R-FIX-UNLISTED`), a bad manifest (`R-FIX-SCHEMA`, `R-FIX-NOMANIFEST`) or a symlink (`R-FIX-SYMLINK`).
- Changing a fixture on purpose: edit it, run `python3 -I tools/testing/fixtures.py lock`, and review the manifest diff with the change.
- `deps/`: fixtures of the dependency provider tests (cpm offline archive recipe, fake vcpkg toolchain and port).
- Expected outputs live in `tests/golden/`.
