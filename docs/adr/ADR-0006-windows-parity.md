---
id: ADR-0006
title: "Windows parity is verified structurally on Linux"
status: Proposed
date: 2026-10-17
decision_refs: []
supersedes: []
---
# ADR-0006: Windows parity is verified structurally on Linux

## Context

The starter prompt requires that Windows (MSVC + Clang-cl) must also configure (master plan, Starter prompt,
constraints). The graphics policy puts Windows on Vulkan (master plan section 2.6). The workstation is Arch Linux and
has clang-cl 23 but no Windows host (machine fact, plan of record, 2026-10-17). The release gates require evidence
on every platform (master plan, Release gates).

## Decision

- On Linux, Windows parity MUST be verified only structurally: a presets check, `cmake -P` tests of the compiler flag table with mocked MSVC and clang-cl ids, a clang-cl flag-acceptance probe and a portability lint (plan of record, Decisions table, ADR-0006 row).
- These checks are not a Windows configure and MUST NOT be reported as one (master plan, Release gates: evidence on every platform).
- Real configure evidence SHOULD come from a `workflow_dispatch` windows-latest job run after the first push by the founder (plan of record, Decisions table).
- `.github/**` is a governed path, so changes to it need the founder (master plan section 6.3 rule 4; plan of record, FACTORY.md governed list).
- Until that job has run, the 041-048 episode notes MUST list Windows as unverified (master plan section 6.1, completion record).

## Consequences

- WO-014 builds the structural checks; WO-020 adds the CI scripts; WO-021 records the open issue.
- No claim of Windows support is made before the hosted job passes.

## Alternatives

- A local Windows VM: rejected for this slice, the plan takes hosted evidence after the first push.
- Skip Windows until later: rejected, the starter prompt requires it to configure (master plan, Starter prompt).
