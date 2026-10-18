# engine/core

`sl_core`: the foundation library (static, plus the generated `build_info.hpp`). Landed with WO-014. Depends on
nothing but the standard library (and `stdc++exp` when `std::stacktrace` is linked).

| Header | What | WO |
| :---- | :---- | :---- |
| `sl/core/version.hpp` | `build_info()` baked in at configure time; `log_build_info()` emits it as one info event | WO-014, WO-030 |
| `sl/core/log.hpp` | structured logging: levels trace..fatal, typed fields, correlation `{id, scope, tick}` (F6), sinks, logger | WO-030 |
| `sl/core/error.hpp` | `sl::errc` by domain (core 1000-, platform 2000-, graphics 3000-, asset 4000-), `sl::error`, `sl::result<T>` | WO-030 |
| `sl/core/assert.hpp` | `SL_ASSERT`, `SL_VERIFY`, `SL_DEBUG_ASSERT`: fatal event, JSON report, stack trace, abort | WO-030 |

Diagnostics (WO-030, SL-EP-056, ADR-0009):

- Logging: `sl::log::emit(level, system, message, {{"key", value}...})` or `SL_LOG_INFO(system, message, {"k", v})`
  to `sl::log::default_logger()` (a `stderr_sink` at level info). Sinks: `stderr_sink` (text), `json_sink` (one JSON
  object per line, `schemas/sl-log-event.schema.json`), `memory_sink` (ring of the last N events). A
  `correlation_scope` sets the thread's correlation; outside one, events carry the process scope tag (`"process"`,
  `set_process_scope`). The logger serialises sinks with a mutex.
- Errors: functions that can fail return `sl::result<T>`; build failures with `sl::make_error(errc, message)`;
  `sl::describe(error)` gives `core.not_found (1002): message [file:line]`. Nothing on these paths throws.
- Assertions: on failure a fatal event (system `assert`) with the expression, `file:line:function` and the stack
  trace goes to the default logger; `SL_ASSERT_REPORT_JSON=<path>` (or `1` for `./assert-report.json`) writes the
  JSON report; then `std::abort()`. Tests capture failures with `sl::scoped_assert_handler`.
- Stack traces: configure checks that `std::stacktrace` links (without a library, then with `stdc++exp`) and prints
  `sliced: std::stacktrace in assertion reports: on|off`; `-DSL_STACKTRACE=OFF` disables it.
- Gates: `just test` (unit tests in `tests/unit/`), `just log-schema-check`, `just neg-log-schema`.
