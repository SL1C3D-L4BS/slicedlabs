---
id: ADR-0022
title: "Runtime error domain and fault injection"
status: Proposed
date: 2026-10-20
decision_refs: []
supersedes: []
---
# ADR-0022: Runtime error domain and fault injection

## Context

Errors are values and no exception crosses a module (ADR-0009). The Fabric reserved 5000-5999 with its own domain
and a code table bound by a contract lock (ADR-0010); its storage faults are injected through named failpoints
(ADR-0015). `sl::errc` is one enum in engine/core with `error_domain {none, core, platform, graphics, asset, fabric,
unknown}` and `domain()` mapping by thousands (machine fact: engine/core/include/sl/core/error.hpp). Volume 05 needs
typed failures for lifecycle, memory, jobs, ECS, events, snapshots and the link, and a way to make each failure
happen on demand under the sanitizers (master plan, Volume 05, episodes 081 and 099).

## Decision

- Runtime codes MUST be `sl::errc` values in 6000-6999, grouped by area as the SL-ARCH-003 `error_codes` table lists them, with `error_domain::runtime` appended after `unknown` so no existing domain value moves (ADR-0009; ADR-0010 pattern).
- Every code of the table MUST be added in one edit by WO-076, with to_string names, and MUST NOT be renumbered or reused; a golden test pins every pre-existing number (ADR-0010 pattern).
- A runtime API MUST NOT return a runtime code outside the table, and core codes are reused, never shadowed (ADR-0009).
- Failpoints RFP0-RFP10 MUST be the SL-ARCH-003 `failpoints` table: compiled only when SL_RUNTIME_FAULTS=1 (Debug and sanitizer presets), compiled out of Release, armed only through `sl::runtime::testing`, with the outcome after each fixed by the table (ADR-0015 pattern).
- The failpoint registry MUST be the only mutable namespace-scope state of the runtime, allow-listed by runtime-lint (master plan section 2.3).
- The error and failpoint tables MUST be bound to the code by `just runtime-contract-lock` (WO-100) (ADR-0010 pattern).

## Consequences

- engine/core changes once (WO-076) under `just fabric-contract-lock` and `just fabric-surface-check`; later
  orders never touch engine/core.
- RFP3 is `job.fatal` (a job reports a fatal error through `job_context::fail_fatal`), not a throwing body.
- The stress matrix proves every failpoint outcome (RT-INV-STRESS-08).

## Alternatives

- A separate runtime error enum: rejected, `sl::result` carries `sl::errc` and a second enum splits the error model.
- Failpoints in Release: rejected, a shipped binary must not carry test hooks.
- Codes added per work order: rejected, it would reopen engine/core in a dozen orders.
