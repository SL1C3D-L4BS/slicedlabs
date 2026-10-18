---
id: ADR-0016
title: "F5 enforcement: immutable snapshots, a passkey on the only mutator, a debug ledger and a surface lint"
status: Proposed
date: 2026-10-18
decision_refs: []
supersedes: []
---
# ADR-0016: F5 enforcement: immutable snapshots, a passkey on the only mutator, a debug ledger and a surface lint

## Context

F5 requires mutations bypassing the transaction manager over all mutations to be 0, instrumented in debug builds, with
the enforcement point "mutation APIs only reachable through the transaction manager" (master plan section 3). Every
write from a person, tool, plugin or agent enters through the transaction layer (master plan section 2.1). Release
builds cannot afford per-write counters, so the guarantee has to be structural and the counter a detector.

## Decision

- No mutable document API MUST exist: snapshots and values are immutable and public headers expose no setter and no non-const accessor into stored data (master plan section 3, F5).
- `store_backend::commit` MUST take a `const mutation_capability&`; the capability is non-copyable, non-movable, and constructible only by `transaction_manager` and by `testing::backdoor`, which is declared in `store.hpp` and defined only in the test-only library `sl_fabric_testing` (master plan section 3, F5).
- Every other storage write (ref creation, ref reset, pin, checkpoint, quarantine move) MUST either be a transaction itself or be a recovery action recorded in the recovery report; none is reachable from public headers without the capability (master plan section 2.1).
- Debug and sanitizer builds MUST compile the F5 ledger (`SL_FABRIC_F5_INSTRUMENT`): the manager opens `f5::authorized_scope` around a commit, every backend mutation calls `f5::note_mutation(site)`, and a call outside a scope increments `bypassing`, logs a fatal `fabric/f5` event and trips `SL_DEBUG_ASSERT` (master plan section 3, F5; ADR-0009).
- Schema and domain registries MUST be mutable only before `freeze()`, which the workspace calls when it opens (master plan section 3, F2 and F5).
- `just fabric-surface-check` (rules R-F5-FRIEND, R-F5-MUTABLE, R-F5-CAP, R-F5-SETTER, R-F5-INCLUDE) MUST pass, and `just neg-fabric-f5` MUST prove it fails twice: on a planted public setter, and on a copy of `transaction_manager.cpp` with the `authorized_scope` line removed, where the ledger test reports `bypassing > 0` and `mutations > 0` (master plan section 3, F5; governance rule: every gate has a neg test).
- Compile-fail checks (requires-expressions under `static_assert`) MUST prove that commit without a capability and capability construction outside the friends do not compile (master plan section 3, F5).

## Consequences

- WO-042 lands the capability and backdoor declaration; WO-050 the manager; WO-051 the ledger, the lint, the
  compile-fail tests and the negative test. The scorecard (WO-062) reports `mutations` and `bypassing` (master plan
  section 3.3, transaction coverage).
- The friend list is part of the lint; a new friend needs this ADR amended.
- Release builds have no ledger; the Release test asserts it is compiled out, and the structural barrier carries F5.

## Alternatives

- Library split into a read-side and a write-side target with compile-fail tests only (D1 draft): kept as the
  compile-fail checks, but a split alone leaves raw backend writes open.
- Private head advance plus counters at named choke points (D3 draft): rejected, a public free write function could
  still move a head with no barrier and no count.
- Workspace or transient state classes exempt from F5 (D2 draft): rejected, master plan section 3.1 allows six
  exemption classes only and section 2.2 makes workspace layouts authoritative data of subsystem 01.
