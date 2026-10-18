---
id: ADR-0012
title: "Canonical value model and the SLB/1 encoding, proven by two implementations"
status: Proposed
date: 2026-10-18
decision_refs: []
supersedes: []
---
# ADR-0012: Canonical value model and the SLB/1 encoding, proven by two implementations

## Context

Episode 068 asks for a stable encoding with version tags, validation and corruption handling; episode 062 for
primitive, compound, tagged, optional, reference and versioned types (master plan, Volume 04). Revisions are content
addressed (master plan section 1.5, row Version control), so one value must have exactly one byte string or the same
fact gets two object ids, a second representation that F2 forbids (master plan section 3, F2). Determinism is a
contract proven by tests (master plan section 1.5, row Determinism). Tooling is Python stdlib only (ADR-0004).

## Decision

- Every persisted fact MUST be an immutable value tree with structural sharing; edits produce a new value and never mutate the old one (master plan section 2.4).
- SLB/1 MUST be self-describing: one tag byte then payload. Tags: 0x01 false, 0x02 true, 0x03 uint (LEB128), 0x04 nint (LEB128 of -(v+1)), 0x05 f32, 0x06 f64 (little-endian IEEE), 0x07 string (varint length, UTF-8), 0x08 bytes, 0x09 id (16 bytes), 0x0A digest (algo + 32 bytes), 0x0B list, 0x0C map, 0x0D struct, 0x0E variant (varint case, value), 0x0F oset (master plan, Volume 04, episode 068).
- The encoding MUST be canonical: shortest varints; a non-negative integer always as uint; map entries sorted by the encoded key bytes, unique; struct fields ascending by field number, unique; oset entries sorted by (order key bytes, element id), element ids unique; no trailing bytes (master plan section 3, F2).
- Floats MUST be finite; NaN and infinities are rejected (R-SER-NAN) and -0.0 is written as +0.0, so the decoder rejects the -0.0 pattern (R-SER-NEGZERO) (master plan section 1.5, row Determinism).
- An absent optional field MUST be omitted; a present field MUST be written even when it equals the declared default, and readers MUST NOT materialize defaults into stored values (master plan section 4.2: old data keeps its meaning when a default changes).
- Unknown struct fields MUST be preserved byte-exact under the `preserve` policy (the storage default) and rejected under `strict` (master plan section 4.2, mixed-version peers of Volume 18).
- Limits MUST be enforced before allocation: depth 64, 2^24 items per container, record body 16 MiB (master plan, Volume 04, episode 068 "validation").
- Every stored object MUST carry the frame header of ADR-0011 (magic `SLFO`, major 1, minor 0, kind, flags, varint length); a newer major is refused and a newer minor is read-only (master plan, Volume 04, episode 068 "version tags").
- Canonical text MUST be a schema-directed JSON projection (fields by name in field-number order, ids as text ids, digests as `sha256:<hex>`, integers beyond 2^53-1 as decimal strings, floats as shortest round trip) that is bijective with SLB/1; it is an encoding, never a second source of truth (master plan section 1.3, zero 2).
- Determinism MUST be proven by two implementations: a Python stdlib reference (`tools/fabric/slb_ref.py`) with an accept corpus and a must-reject corpus naming each rule, and the C++ codec, which must reproduce every accept vector byte for byte on clang and gcc (master plan section 1.5, row Determinism; ADR-0004).
- Wall-clock time, correlation ids, pointers and process-local values MUST NOT appear in any hashed body (master plan section 4.2).

## Consequences

- WO-038 writes the reference and corpus, WO-041 the C++ codec with `just fabric-determinism` (clang and gcc corpora
  diffed against each other and against the committed corpus) and `just neg-fabric-determinism`.
- Changing a default is a breaking schema change (SL-ARCH-001 compatibility table), never a silent change of old data.
- Self-description costs one tag byte per value; accepted for schema-free validation, corruption detection and
  unknown-field preservation.

## Alternatives

- Schema-driven untagged binary (D2 draft): rejected, unknown fields cannot be preserved without the schema and an
  independent reference needs the whole type system.
- Protobuf, FlatBuffers or CBOR: rejected, none guarantees one encoding per value without extra rules, and each is a
  new dependency (SL-GOV-008).
- Default elision on write: rejected, it changes the meaning or the bytes of old data when a default changes.
