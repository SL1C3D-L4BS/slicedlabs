# tests/system

System tier (ctest label `system`, default timeout 120 s): a built binary runs as its own process. `sl_add_system_test` takes the executable target, its arguments and an optional `PASS_REGEX`. Now: `sl_sandbox --self-check` (exit 0) and the sandbox banner (regex). See `docs/testing/README.md`.
