---
id: ADR-0013
title: "Schema source of truth, released-version lock and committed generated code"
status: Accepted
date: 2026-10-18
decision_refs: []
supersedes: []
---
# ADR-0013: Schema source of truth, released-version lock and committed generated code

## Context

Episode 078 asks for serializers, inspectors, validators and API metadata generated from schemas; episode 063 for
compatibility, migration, deprecation and failure semantics; episode 080 for approved schemas plus executable contract
tests (master plan, Volume 04). Editor, runtime, tools and services share schemas (master plan section 1.2, principle
3). A hand-written C++ schema next to a schema file would be a second authoritative representation (master plan
section 1.3, zero 2). Repository tooling is Python stdlib only and gates do not spawn processes (ADR-0004).

## Decision

- Schema source MUST be `schemas/fabric/<module>.sltype.json` in format `sl-type-def/1`, validated by `schemas/fabric/sl-type-def.schema.json`; nothing else defines a Fabric type (master plan section 1.3, zero 2).
- A module file MUST name its domain and owner subsystem; each type names its kind, version `major.minor`, record class, state class (document, derived or observation), and stable field numbers with a reserved list (master plan section 2.2).
- Each released (type, version) MUST have an entry in `schemas/fabric/schema.lock.json` holding the SHA-256 of the SLB/1 encoding of its definition value; a released entry is immutable and `just fabric-schema-check` fails when it changes (master plan, Volume 04, episodes 063 and 080).
- The same function (SLB/1 encoding of the definition, ADR-0012) MUST compute the lock hash in Python and the schema object id in C++, so the two cannot disagree (ADR-0012).
- A change MUST be classified by the compatibility table of SL-ARCH-001 and carry the version bump the table demands; breaking changes need a registered migration step (master plan, Volume 04, episode 063).
- `tools/fabric/slgen.py` (stdlib, deterministic) MUST emit committed output: C++ descriptors, typed structs with `to_value`/`from_value`, validator tables and inspector metadata JSON under `engine/fabric/gen/`; `just fabric-gen-check` regenerates in memory and fails on any difference (master plan, Volume 04, episode 078; ADR-0004).
- Fabric system records (revision, tx record, tombstone, import record, manifest, provenance, domain claim, event envelope) MUST be schema files from their first commit; there is no hand-written bootstrap set to replace later (master plan section 1.3, zero 2).
- Every type MUST have fixtures under `tests/fixtures/fabric/types/` before it is released; the conformance suite runs its generic cases over every registered type (master plan, Volume 04, episode 079).
- Runtime handles and transient state MUST NOT be expressible: there is no handle field type and no transient state class (master plan, Volume 04, episode 074).

## Consequences

- WO-040 delivers the format, its JSON Schema, the lock, the classifier and `fabric-schema-check` with
  `neg-fabric-schema`; WO-045 delivers the generator, the system modules and `fabric-gen-check` with `neg-fabric-gen`.
- The JSON Schema for the canonical text form is a cut line of WO-045: if it slips, its invariant moves to the
  retired list with a note, and nothing else depends on it.
- The lock starts empty; WO-063 pins the Volume 04 system modules as released 1.0 (episode 080).
- Generated C++ is committed, so building needs no Python; the gate runs Python only to diff.

## Alternatives

- Schemas as C++ code with a runtime registry and a JSON dump (D3 draft): rejected, episode 078 asks for generated
  serializers and inspectors, and Luau and web tools need a language-neutral source.
- Generating at build time: rejected, it puts Python on the build path and hides drift until build.
- A hand-written bootstrap set replaced later by generated code: rejected, it is a temporary second representation
  and a late swap under about twenty dependent work orders.
