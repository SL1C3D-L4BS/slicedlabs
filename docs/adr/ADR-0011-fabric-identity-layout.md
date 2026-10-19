---
id: ADR-0011
title: "Fabric identity: 128-bit record ids minted at authoring time, SHA-256 object ids"
status: Accepted
date: 2026-10-18
decision_refs: []
supersedes: []
---
# ADR-0011: Fabric identity: 128-bit record ids minted at authoring time, SHA-256 object ids

## Context

Every authoritative project fact has one identity and one history (master plan sections 1.1 and 1.2, principle 1).
Episode 061 asks for stable IDs, typed references, namespaces and identity lifetime; episode 065 separates assets,
entities, instances and derived resources (master plan, Volume 04). Collaboration (Volume 18) constrains identity
design and must be written down before Volume 04 freezes (master plan section 4.2): offline replicas, cloned
workspaces and restored backups mint ids concurrently (episodes 352 and 354). Replay (Volume 17) needs ids that do not
depend on wall clock or process state (master plan section 4.2; episode 327). The revision store is content addressed
(master plan section 1.5, row Version control).

## Decision

- A record id MUST be 16 bytes: byte 0 = id class, byte 1 = mint scheme, bytes 2-15 = 112-bit payload; the text form is a three-letter class prefix, `_`, and 26 Crockford base32 characters of all 16 bytes (master plan section 2.2, row 02).
- Mint schemes MUST be: 1 random (payload from an injected `id_source`), 2 derived (`SHA-256("slf-derive/1" || class || parent id || u32le key length || key)` truncated to 14 bytes), 3 well_known (ordinal big-endian in bytes 12-15, zero elsewhere), 4 imported (`SHA-256("slf-import/1" || class || len-prefixed system || len-prefixed external id)` truncated) (master plan section 4.2).
- Random ids MUST be minted at authoring time, inside the patch, never at commit; `mint()` returns `sl::result<record_id>` and an entropy failure is the value `entropy_failure`, never a zero or repeated id (ADR-0009; master plan section 4.2).
- Production uses `os_entropy_source::create()` (getrandom, with a start-up self-test that two draws differ and are non-zero); tests and replay use `deterministic_source(seed, stream)` (SHA-256 counter mode) (master plan section 1.5, row Determinism).
- There MUST be no per-replica serial allocator and no replica registry: a cloned or restored store keeps minting from fresh entropy, so diverging copies never collide by construction (master plan section 4.2).
- An id MUST contain no time, host, user or counter, and its class never changes; an id is never reused, and a retired id stays as a tombstone leaf for every class (master plan section 1.2, principle 1).
- Instance ids MUST derive from (parent instance or document, source entity) and derived-resource ids from the derivation key, so re-instantiation, re-import and re-cook keep ids (master plan, Volume 04, episode 065 summary).
- Content identity (object id, oid) MUST be `{algo u8, 32 bytes}`; algo 1 = SHA-256 over `"slf-obj/1" 0x00 || major || minor || kind || flags || varint(length) || payload`, so the preimage covers the whole frame header and a flipped version byte changes the oid (master plan section 1.5, row Version control).
- A content digest MUST NOT be used as an entity identity, and a name MUST NOT be an identity (master plan section 1.2, principle 1).

## Consequences

- WO-037 implements `id.hpp`, `digest.hpp` and in-house SHA-256 (no new dependency, SL-GOV-008 build-versus-adopt);
  WO-038 produces hashlib vectors that WO-041 checks the C++ code against.
- Collision risk of random ids is the 112-bit birthday bound (about 2^56 ids per project before a 50 % chance);
  stated in SL-ARCH-001 Open issues, not hidden.
- `algo` 2 is reserved for BLAKE3; adopting it needs a tier-A dependency under SL-GOV-008 and its own ADR.
- Tombstones for every class make the retired-id set grow without bound; retention and legal deletion are an open
  question for the founder.

## Alternatives

- ULID or UUIDv7: rejected, a clock inside identity breaks replay (master plan section 4.2).
- Per-replica origin plus durable serial blocks: rejected, a cloned workspace or restored backup reuses the origin and
  rewound serials and mints colliding ids; registering replicas also makes the manifest a contended document.
- Content hash as entity identity: rejected, it changes on every edit.
- A central allocator: rejected, local authoring must work with no remote service (master plan section 2.3).
