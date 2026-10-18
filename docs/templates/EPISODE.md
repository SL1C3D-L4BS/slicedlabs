<!--
Episode record template (master plan section 6.1). One file per episode: docs/episodes/NNN.md (NNN = the episode
number, three digits). Copy this file, replace every <...>, and delete this comment. The registry
(registry/SL-REG-002-episodes.json) is authoritative (master plan section 6.3 rule 1): this file adds the prose the
registry holds only as pointers, and the status, dependencies and work orders below MUST agree with the registry.
An episode becomes Accepted only on evidence (section 6): passing acceptance tests, an integrated demo in the running
product and a completion record. Code existing is not completion. "None" is a valid answer; an empty field is not.
-->
# SL-EP-<nnn>: <title as in the registry>

| Field | Value |
| :---- | :---- |
| ID / volume | SL-EP-<nnn>, SL-VOL-<nn> (stable forever; the meaning never changes, section 6.3 rule 2) |
| Status | <Planned, Specified, InProgress, Verifying, Demonstrated, Accepted, Blocked or Reopened> (as in the registry) |
| Decision status | <Locked, Conditional, Experimental or Unresolved> |

## Purpose

The capability this episode establishes, the user outcome, and the non-goals (what it must not do).

## Dependencies

Episodes (SL-EP-nnn) and documents (SL-<KIND>-nnn) that must be Accepted first. A waiver (for example ADR-0003)
is named here with its ceiling.

## Architecture

Boundaries, ownership, data flow and failure modes. Name the module that owns each piece of state.

## Executable contract

Schemas, API signatures, invariants and error behavior, as text a test can check: file paths, function
signatures, JSON shapes, exit codes.

## Work orders

Bounded Claude Code prompts with file ownership: one line each, `WO-NNN (class S|M|L): goal; paths_allowed`.
Every work order listed here carries this episode in its `Traces:` line.

## Acceptance suite

Positive, negative, boundary, recovery and performance cases, each as the `just <recipe>` that runs it and the
expected exit code.

## Integration target

The vertical slice that consumes this episode (SL-PLAN-004) and the demo that shows it working in the running
product.

## Kohi reference

The matching Kohi arc, if any, as a pointer only. Kohi is a behavioural guide: no source is read or copied.

## Release applicability

The milestone it ships in (Foundation, Core loop, Production, Early Access or 1.0) and the target platforms.

## Completion record

Filled in when the episode reaches Demonstrated; the registry holds the same facts.

- Revision: <commit id of the merge that completed it>
- Test evidence: <evidence/WO-NNN.json for each work order, with the acceptance commands and exit codes>
- Benchmarks: <benchmark ids, or none>
- Open defects: <ids or one line each, or none>
- Approved exceptions: <SL-EXC-nnn, or none>
