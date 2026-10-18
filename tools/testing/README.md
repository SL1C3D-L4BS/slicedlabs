# tools/testing

Test-data tooling (WO-027, episode 052). Python 3 stdlib only, `python3 -I`, no subprocess, no network (ADR-0004).

- `fixtures.py lock|check [--root DIR]`: the `tests/fixtures/MANIFEST.json` gate. `lock` rewrites the manifest from disk; `check` fails on `R-FIX-NOMANIFEST`, `R-FIX-SCHEMA`, `R-FIX-MISSING`, `R-FIX-DRIFT`, `R-FIX-UNLISTED` and `R-FIX-SYMLINK`.
- `test_fixtures.py`: unit tests (`just tools-test`).
- Recipes: `just fixtures-check`, `just golden-check`, `just neg-golden` (`tests/gates/neg_golden.sh`).
