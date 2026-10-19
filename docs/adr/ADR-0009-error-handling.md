---
id: ADR-0009
title: "Errors are values: std::expected in engine code, assertions abort"
status: Accepted
date: 2026-10-17
decision_refs: []
supersedes: []
---
# ADR-0009: Errors are values: std::expected in engine code, assertions abort

## Context

Episode 056 delivers developer diagnostics: structured logs, error codes, stack traces and machine-readable
reports (master plan, Volume 03, episode 056). The Kohi mapping row for #003 asks for structured logs with typed
fields and correlation IDs, not formatted strings (master plan section 5.1, row Logging and assertions). F6 requires
every trace event to carry correlation fields, with process-wide events carrying a scope tag (master plan section 3,
F6). The runtime model is an archetype ECS with a job system that declares read/write access (master plan section
1.5, row Runtime model): failures are produced inside jobs on worker threads, where an unwinding exception crosses
job and thread boundaries the scheduler does not own. The language baseline is C++23 (master plan section 1.5, row
Engine language; ADR-0005), which provides `std::expected` (cmake/features/required-cxx23.txt, `expected`).

## Decision

- Engine code MUST report a recoverable failure as a value, `sl::result<T>` = `std::expected<T, sl::error>`, not by throwing (master plan section 1.5, rows Runtime model and Engine language; episode 056).
- `sl::error` MUST carry an `sl::errc` code, a message and a `std::source_location`; codes MUST stay in their domain ranges (core 1000-1999, platform 2000-2999, graphics 3000-3999, asset 4000-4999) and are never renumbered (episode 056 "error codes"; master plan section 6.3 rule 2 applied to codes).
- The logging, error and assertion paths MUST NOT throw by design; a failure inside them is contained where it happens, not propagated (episode 056; master plan section 1.5, row Runtime model; containment in Consequences).
- A broken invariant is not an error value: `SL_ASSERT` (every build) and `SL_DEBUG_ASSERT` (Debug only) MUST log a fatal event, write the JSON report when `SL_ASSERT_REPORT_JSON` is set, and abort (episode 056 "machine-readable reports", master plan Volume 03).
- Tests MAY override the assert handler to capture the report without aborting (master plan section 5.1, row Logging and assertions).
- Every log event MUST carry `correlation {id, scope, tick}`; outside a `correlation_scope` the scope is the process-wide tag (master plan section 3, F6).
- The JSON form is `schemas/sl-log-event.schema.json`, checked by `just log-schema-check`, the F6 enforcement point "trace schema validator" (master plan section 3, F6).
- `-fno-exceptions` is NOT set by this ADR: third-party code and the standard library still use exceptions; dropping them is a separate decision (open issue below).

## Consequences

- WO-030 lands `sl/core/error.hpp`, `log.hpp`, `assert.hpp`, their unit tests, the schema, `log-schema-check` and
  `neg-log-schema`. Stack traces appear in reports only where `std::stacktrace` links (stdc++exp on libstdc++).
- Functions that can fail return `sl::result<T>`; callers propagate with `and_then`, `transform` or an early
  return. Code that may still throw (dependencies, `std::format`, allocation) is wrapped at the boundary that
  calls it, so `just tidy` (bugprone-exception-escape) passes without waivers.
- Logging catches at the sink boundary: a `write()` whose formatting or allocation throws drops that one event
  and increments the sink's `failures()` counter; the caller and the other sinks are unaffected. Chosen over
  terminating because a lost log line must not take down a running game; the cost is a silent drop, visible
  only through `failures()`. JSON output for events that do get written is unchanged.
- Assertions still abort: a failed stack-trace capture yields an empty trace, and a handler that throws falls
  back to the default handler, which writes a fixed stderr line if its own formatting fails, then aborts.
- Open issue: whether to build the engine with `-fno-exceptions`, and how dependencies are isolated if so.
- Open issue: the logger takes a mutex per event; a lock-free path is for the job system episodes (master plan
  section 1.5, row Runtime model).

## Alternatives

- Exceptions for recoverable errors: rejected for engine hot paths, because unwinding across job and worker
  boundaries is hard to bound and to test (master plan section 1.5, row Runtime model).
- Error codes as plain integers with an out-parameter: rejected, `std::expected` is in the C++23 baseline
  (ADR-0005) and keeps the value and the error in one return type.
- Assertions compiled out of Release: rejected for `SL_ASSERT`; only `SL_DEBUG_ASSERT` disappears, so a broken
  invariant in a shipped build still produces a report (episode 056).
