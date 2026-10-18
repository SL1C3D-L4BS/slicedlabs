# tests/golden

Golden (expected-output) files for the test suite (WO-027, SL-EP-052). Each `<name>.golden` is compared with the text a test produces through `SL_GOLDEN_CHECK("<name>", text)` from `tests/unit/sl_golden.hpp`.

- Location: `SL_GOLDEN_DIR` is a compile definition that `tests/CMakeLists.txt` sets to this directory; a test never builds the path itself.
- Mismatch: the doctest `CHECK` fails and the output carries a unified-style line diff (`--- golden/<name>.golden`, `+++ actual`, `@@ -old +new @@` hunks, `-` expected, `+` actual).
- Update: `SL_UPDATE_GOLDEN=1 ctest --test-dir build/debug -R golden` rewrites the files from the actual text and passes. Review the file diff in the same change that explains it; never edit a golden file to make a failure go away without understanding it.
- Determinism: volatile content (compiler and versions, revision, build type, sanitizer, preset, linker, dates, absolute paths) is masked before comparison, so a golden is identical on clang and gcc and on every preset. A masked field that is empty renders as `<unset>`.
- `build_info.golden`: the sandbox banner (`examples/sandbox/main.cpp`) rendered from `sl::core::build_info()` and masked by `tests/unit/golden_smoke.cpp`.
- Recipes: `just golden-check` (debug build, `ctest -R golden`), `just neg-golden` (edits a golden file in a copy of the tree and asserts the failure and the diff).
- Golden files are LF text, small, and not hash-manifested; binary assets and static inputs belong in `tests/fixtures/` (hash manifest, `just fixtures-check`).
