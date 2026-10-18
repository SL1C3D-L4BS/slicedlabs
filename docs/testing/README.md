# Testing

Four test tiers, one ctest label each, one default timeout each. Source: master plan episode 051 (SL-EP-051), quality
model SL-GOV-009. The helpers live in `cmake/SlTesting.cmake`; the tiers are under `tests/`.

| tier | label | default TIMEOUT | what it is | where |
|---|---|---:|---|---|
| unit | `unit` | 10 s | doctest, one process, no spawning | `tests/unit` (`sl_tests`) |
| integration | `integration` | 60 s | doctest across several libraries in one process | `tests/integration` |
| system | `system` | 120 s | a built binary run as its own process, exit code or output regex | `tests/system` |
| e2e | `e2e` | 300 s | a `cmake -P` script that copies the tree to `build/e2e`, configures (debug preset, no network), builds and runs `sl_sandbox` | `tests/e2e` |

A test that outlives its TIMEOUT fails and ctest prints `Timeout`; `just neg-test-timeout` proves it.

## Run

- `just test [name]`: all tests on the debug preset, or the tests matching `name` (ctest `-R`).
- `just test-tiers [tier...]`: configure and build debug once, then `ctest -L <tier>` per tier with a count per tier. A tier with no tests fails.
- `just test-report`: all tests, JUnit in `build/debug/test-report.xml`, Markdown summary (tier counts, durations) in `build/debug/test-report.md`.
- `just neg-test-timeout`: the timeout gate fails a sleeping test and passes the control.

## Add a test

In the tier's `CMakeLists.txt` (the helpers are included by the root `CMakeLists.txt`):

```cmake
sl_add_test(NAME my_test TIER unit SOURCES my_test.cpp LIBS sl::core)            # doctest, generated main
sl_add_test(NAME my_test TIER unit SOURCES my_test.cpp HAS_MAIN)                 # your own main
sl_add_test(NAME slow TIER integration SOURCES slow.cpp TIMEOUT 120 WILL_FAIL)   # override timeout, expect failure
sl_add_system_test(NAME my_run COMMAND sl_sandbox ARGS --self-check PASS_REGEX "ok")
sl_add_script_test(NAME my_e2e TIER e2e SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/my_e2e.cmake ARGS -DSL_SOURCE_DIR=${CMAKE_SOURCE_DIR})
```

- Unit and integration sources include `<doctest/doctest.h>` and write `TEST_CASE`s; do not define `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`.
- Each test gets `LABELS <tier>`, the tier TIMEOUT (or `TIMEOUT s`) and, when `SL_SANITIZE` is set, `ASAN_OPTIONS`, `UBSAN_OPTIONS` or `TSAN_OPTIONS`.
- Every test is written to `build/<preset>/test-registry.tsv` (tier, name, timeout), which `just test-report` joins with the JUnit file.
- The test presets use `noTestsAction: error`, so a build with zero tests fails.
