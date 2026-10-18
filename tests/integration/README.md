# tests/integration

Integration tier (ctest label `integration`, default timeout 60 s): doctest tests that link several libraries in one process. `sandbox_selfcheck.cpp` repeats the `sl_sandbox --self-check` checks against `sl::core` without spawning a process. See `docs/testing/README.md`.
