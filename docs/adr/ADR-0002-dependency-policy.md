---
id: ADR-0002
title: "One dependency policy, one lockfile, four providers"
status: Accepted
date: 2026-10-17
decision_refs: []
supersedes: []
---
# ADR-0002: One dependency policy, one lockfile, four providers

## Context

The starter prompt requires dependencies pinned through vcpkg manifest mode with a committed baseline (master plan,
Starter prompt, constraints). The master plan names SL-GOV-008 Build-vs-Adopt and Dependency Policy and SL-REG-005
Dependency and License Register as required documents (master plan, Required documents and registries).
Machine fact (plan of record, 2026-10-17): doctest and vcpkg are not installed but are in the Arch `extra` repository.
The founder chose all three mechanisms (system and vendored hash-manifested, vcpkg manifest, CPM.cmake) under one
policy and one lockfile.

## Decision

- There MUST be one dependency policy, SL-GOV-008, and one lockfile `deps/deps.lock.json` (master plan, Required documents and registries; plan of record, Decisions table).
- Four providers (vendored, system, cpm, vcpkg) MUST resolve a package through `sl_require(<pkg>)` to `sl::<pkg>` (plan of record, Decisions table, ADR-0002 row).
- CI presets SHOULD resolve vendored first and verify hashes (plan of record, Decisions table); developer presets SHOULD resolve system first (plan of record).
- vcpkg is an optional provider the founder installs; the committed baseline stays `null` and `unpinned` until the founder pins it, and episode 048 records that honestly (master plan section 6.1, completion record).
- Agents MUST NOT fetch or install anything (machine fact, plan of record: "Agents never install or fetch").

## Consequences

- WO-010 writes SL-GOV-008; WO-017 and WO-018 write the lock and `sl_require`; WO-019 adds the cpm and vcpkg providers.
- A hash mismatch never falls through to the next provider.
- The unpinned vcpkg baseline is an open issue in the 041-048 episode notes, not a silent default.

## Alternatives

- vcpkg only, as the starter prompt reads literally: rejected, the baseline cannot be pinned without the founder
  installing vcpkg (machine fact above), and the offline gate needs a provider that works without it.
- A separate lock per provider: rejected, it creates a second source of truth (master plan section 1.3).
