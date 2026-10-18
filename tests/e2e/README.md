# tests/e2e

E2E tier (ctest label `e2e`, default timeout 300 s). `sandbox_build.cmake` (run as `cmake -P` by `sl_add_script_test`) copies the tree to `build/e2e/tree`, configures it with the debug preset (no network), builds `sl_sandbox` and runs it. See `docs/testing/README.md`.
