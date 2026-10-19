---
id: SL-VOL-04-SPEC
kind: VOL
title: "Volume 04 Specification: Core architecture, identity, project data (the Fabric)"
status: Draft
version: 0.2.0
owner: founder
episodes: [61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80]
depends_on: [SL-GOV-001, SL-ARCH-001, SL-ARCH-002]
supersedes: []
decisions: [ADR-0010, ADR-0011, ADR-0012, ADR-0013, ADR-0014, ADR-0015, ADR-0016, ADR-0017, ADR-0018, ADR-0019, ADR-0020]
---
# SL-VOL-04-SPEC: Volume 04 Specification

## Purpose

Volume 04 builds the Fabric, the canonical data model that keeps editor, runtime, assets and tools from drifting
apart; it has no Kohi counterpart (master plan, Volume 04 block). This document holds one record per episode 061-080
(master plan section 6.1): purpose, the work orders that trace it, exactly one primary work order whose acceptance
recipe is the episode's acceptance, the seam it leaves, a work-bound performance case and release applicability. It is
a view: the registry SL-REG-002 stays authoritative (master plan section 6.3, rule 1). The architecture is SL-ARCH-001
(data) and SL-ARCH-002 (protocols).

## Status and decisions

Status is Draft (version 0.2.0): 0.1.0 was written with both ARCH documents before any Volume 04 code (master plan,
Authoring order); 0.2.0 (WO-063) updates the seams to what the code shipped. A record's `title` and `purpose` are the SL-REG-002 `title` and `summary`, verbatim; `work_orders` repeats the
`Traces:` lines of `work-orders/WO-035.md` to `WO-064.md`. ADR-0020 caps 061-080 at Demonstrated until Volume 03 is
Accepted and both ARCH documents are Approved; builders change no status. Binding decisions: ADR-0010 to ADR-0020.

## Normative text

- VOL4-R01: Each episode 061-080, and no other, MUST have one record with purpose, work orders, primary work order, acceptance recipe, Kohi reference, seam, performance case and release applicability (master plan section 6.1).
- VOL4-R02: A record's title and purpose MUST NOT diverge from the registry (master plan section 6.3, rules 1 and 2).
- VOL4-R03: `work_orders` MUST equal the set of work orders whose `Traces:` line names the episode, in both directions (master plan section 6.3, rule 4).
- VOL4-R04: Each record MUST name exactly one `primary_wo`, a member of `work_orders` whose acceptance list contains the record's `acceptance` command; a work order MUST be primary for at most one episode (master plan section 6.3, rule 4; ADR-0020).
- VOL4-R05: Document, tooling and infrastructure work orders (WO-035, WO-036, WO-038, WO-040, WO-042, WO-043, WO-046, WO-051, WO-053) MAY trace episodes and MUST NOT be primary (master plan section 6.3, rule 4).
- VOL4-R06: Episodes 061-080 MAY reach Demonstrated on recorded evidence and MUST NOT reach Accepted before the conditions of ADR-0020 hold (master plan section 6.2; ADR-0020).
- VOL4-R07: A seam MUST state what the slice leaves undone, so no later work reads absence as completion (master plan section 6.3, rule 3).
- VOL4-R08: The performance case MUST be work-bound (objects, probes, visited nodes), not wall time, until Volume 16 provides benchmarks (master plan section 6.1, acceptance suite).

```sl-data
{
 "volume": "SL-VOL-04",
 "episodes": [
  {"episode": "SL-EP-061", "title": "Canonical identity algebra", "purpose": "stable IDs, typed references, namespaces, identity lifetime", "work_orders": ["WO-035", "WO-037", "WO-058", "WO-062"], "primary_wo": "WO-037", "acceptance": "just fabric-test id", "kohi_ref": "none", "seam": "random-id collisions are bounded (2^-112 per pair), not detected beyond the trie's duplicate-key refusal; package namespaces are Volume 38", "performance": "1e5 mints with no collision; parse and format are O(1)", "release": "Foundation; Linux desktop; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-062", "title": "Canonical type system", "purpose": "primitive, compound, tagged, optional, reference, versioned types", "work_orders": ["WO-035", "WO-039", "WO-045"], "primary_wo": "WO-039", "acceptance": "just fabric-test type", "kohi_ref": "none", "seam": "no NaN-carrying or handle types; check() needs a frozen registry; Luau and web bindings consume the generated metadata in Volumes 19 and 36", "performance": "with() on a 10k-node value copies at most depth nodes", "release": "Foundation; Linux desktop; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-063", "title": "Schema versioning", "purpose": "compatibility, migration, deprecation, failure semantics", "work_orders": ["WO-035", "WO-040", "WO-057", "WO-062", "WO-064"], "primary_wo": "WO-057", "acceptance": "just fabric-test migration", "kohi_ref": "none", "seam": "a migration is one transaction (no chunking for very large projects); the lock pins the seven fabric.* system modules at 1.0 (30 types), later modules extend it; the newer-minor and deprecation validators are built in (WO-064)", "performance": "migrating N records is one transaction writing O(N) records", "release": "Foundation; Linux desktop; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-064", "title": "Project manifest", "purpose": "metadata, capabilities, target profiles, required modules", "work_orders": ["WO-035", "WO-054", "WO-064"], "primary_wo": "WO-054", "acceptance": "just fabric-test manifest", "kohi_ref": "none", "seam": "capabilities are checked against an injected catalog built from SL-REG-004; no package fetch or publish (Volume 38)", "performance": "open-time checks are O(modules + capabilities + profiles)", "release": "Foundation; Linux desktop; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-065", "title": "Asset and entity identities", "purpose": "assets vs entities vs instances vs derived resources", "work_orders": ["WO-035", "WO-037", "WO-058"], "primary_wo": "WO-058", "acceptance": "just fabric-test identity", "kohi_ref": "none", "seam": "tombstones are kept forever; retention and legal deletion are a founder decision", "performance": "a tombstone lookup is one trie probe", "release": "Foundation; Linux desktop; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-066", "title": "Ownership and authority", "purpose": "which subsystem owns each record and who may mutate it (F2)", "work_orders": ["WO-035", "WO-040", "WO-048", "WO-062"], "primary_wo": "WO-048", "acceptance": "just fabric-ownership-check", "kohi_ref": "none", "seam": "grants are static; roles and collaboration authorization are Volumes 18 and 20; ownership transfer needs an ADR", "performance": "at most one authority check per typed domain boundary per op", "release": "Foundation; Linux desktop; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-067", "title": "Provenance and epistemic classes", "purpose": "observed, imported, asserted, calculated, inferred, accepted", "work_orders": ["WO-035", "WO-049"], "primary_wo": "WO-049", "acceptance": "just fabric-test provenance", "kohi_ref": "none", "seam": "Volume 04 refuses incomplete provenance on the commit path only; the artifact-store refusal (F3 enforcement point) is episode 182", "performance": "derivation key is O(inputs)", "release": "Foundation; Linux desktop; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-068", "title": "Canonical serialization", "purpose": "stable encoding, version tags, validation, corruption handling", "work_orders": ["WO-035", "WO-037", "WO-038", "WO-041"], "primary_wo": "WO-041", "acceptance": "just fabric-determinism", "kohi_ref": "none", "seam": "big-endian hosts untested; BLAKE3 reserved, not adopted (seam S16)", "performance": "decode is O(bytes); limits fail before allocation; 2000-value round trip", "release": "Foundation; Linux desktop; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-069", "title": "Event envelopes", "purpose": "IDs, schema versions, timestamps, causality, correlation", "work_orders": ["WO-035", "WO-047"], "primary_wo": "WO-047", "acceptance": "just fabric-event-schema-check", "kohi_ref": "none", "seam": "in-process bus only; the committed event payload is the tx record and the impact report stays out of the event payload and is reached through impact_engine::committed (WO-064); cross-process transport and the overall F6 measure stay with episodes 301 and 319", "performance": "event ids derive without I/O; delivery is O(subscribers)", "release": "Foundation; Linux desktop; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-070", "title": "Transaction model", "purpose": "atomic edits, validation, commit boundaries, rollback (F5)", "work_orders": ["WO-035", "WO-042", "WO-043", "WO-046", "WO-050", "WO-051", "WO-062", "WO-064"], "primary_wo": "WO-050", "acceptance": "just fabric-test tx", "kohi_ref": "none", "seam": "one writer process per workspace; the collaboration service's authority is undecided (seam S13); reopen is not O(delta) because the log is hashed on open (a later work order)", "performance": "a one-record edit in a 100k-record project writes at most depth + 4 trie objects plus root, revision, tx and patch, and one fsync", "release": "Foundation; Linux desktop; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-071", "title": "Undo/redo semantics", "purpose": "reversible ops, grouping, compensation for irreversible actions", "work_orders": ["WO-035", "WO-043", "WO-052", "WO-064"], "primary_wo": "WO-052", "acceptance": "just fabric-test undo", "kohi_ref": "none", "seam": "per-actor undo only; selective undo by transform is Volume 18 (seam S4); no draft spill for long strokes; undo intents live in an in-memory intent_log, not journal frames 3 and 4 (a later work order)", "performance": "a group undo is one transaction; 200 coalesced sets make one revision", "release": "Foundation; Linux desktop; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-072", "title": "Dependency graph", "purpose": "source dependencies, derived outputs, invalidation, cycles", "work_orders": ["WO-035", "WO-044", "WO-062"], "primary_wo": "WO-044", "acceptance": "just fabric-test deps", "kohi_ref": "none", "seam": "the graph index is in memory and rebuilt on open (no persisted index)", "performance": "closure over 300 seeded graphs matches the oracle; build is O(records + edges)", "release": "Foundation; Linux desktop; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-073", "title": "Change-impact engine", "purpose": "affected assets, systems, tests, build targets", "work_orders": ["WO-035", "WO-056", "WO-064"], "primary_wo": "WO-056", "acceptance": "just fabric-test impact", "kohi_ref": "none", "seam": "impact names affected ids only; scheduling tests and builds is Volumes 10 and 21", "performance": "visited nodes never exceed closure plus frontier edges", "release": "Foundation; Linux desktop; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-074", "title": "Runtime vs document boundary", "purpose": "persistent project state vs transient simulation state", "work_orders": ["WO-035", "WO-061", "WO-064"], "primary_wo": "WO-061", "acceptance": "just fabric-test boundary", "kohi_ref": "none", "seam": "Volume 05 consumes document_projection; the id-to-ECS-handle mapping is episode 091", "performance": "a projection shares the snapshot (O(1) to create)", "release": "Foundation; Linux desktop; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-075", "title": "Reference resolver", "purpose": "IDs across loaded documents, packages, external references", "work_orders": ["WO-035", "WO-060"], "primary_wo": "WO-060", "acceptance": "just fabric-test resolver", "kohi_ref": "none", "seam": "external references resolve only through import records; package publishing and mounting from the network are Volume 38", "performance": "resolve costs at most 1 + pins trie probes", "release": "Foundation; Linux desktop; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-076", "title": "Project recovery", "purpose": "journals, checkpoints, incomplete-transaction detection", "work_orders": ["WO-035", "WO-042", "WO-053", "WO-055", "WO-064"], "primary_wo": "WO-055", "acceptance": "just fabric-crash-sweep", "kohi_ref": "none", "seam": "durability is simulated by a faulty vfs, never power-loss tested (seam S15); Windows unverified (seam S14, ADR-0006); .slfabric/pins is checked by fsck but nothing collects from it (seam S7)", "performance": "recovery reads only the tail after the newest valid checkpoint", "release": "Foundation; Linux desktop; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-077", "title": "Corruption policies", "purpose": "validation failures, quarantine, repair, loss reporting", "work_orders": ["WO-035", "WO-059"], "primary_wo": "WO-059", "acceptance": "just fabric-fsck", "kohi_ref": "none", "seam": "repairs use only local verifiable sources; remote repair sources are Volume 18", "performance": "fsck is O(reachable objects)", "release": "Foundation; Linux desktop; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-078", "title": "Schema-generated tooling", "purpose": "serializers, inspectors, validators, API metadata from schemas", "work_orders": ["WO-035", "WO-040", "WO-045"], "primary_wo": "WO-045", "acceptance": "just fabric-gen-check", "kohi_ref": "none", "seam": "the text-form JSON Schema is a cut line (FAB-INV-GEN-05); Luau and TypeScript stubs are Volumes 19 and 36", "performance": "generator output is byte-stable across runs", "release": "Foundation; Linux desktop; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-079", "title": "Architecture conformance suite", "purpose": "identity, ownership, migration, transaction, dependency invariants", "work_orders": ["WO-035", "WO-036", "WO-062", "WO-064"], "primary_wo": "WO-062", "acceptance": "just fabric-conformance", "kohi_ref": "none", "seam": "performance cases are work-bound proxies; wall-time benchmarks are Volume 16; the replay pin case (FAB-INV-RPL-02) binds to ref reachability because no collector exists", "performance": "the conformance suite fits the integration tier timeout (60 s) at SL_FABRIC_SWEEP_SCALE=1", "release": "Foundation; Linux desktop; Windows unverified (ADR-0006)"},
  {"episode": "SL-EP-080", "title": "Release the canonical model", "purpose": "approved schemas plus executable contract tests", "work_orders": ["WO-035", "WO-063"], "primary_wo": "WO-063", "acceptance": "just acceptance-061-080", "kohi_ref": "none", "seam": "Demonstrated-capped until Volume 03 is Accepted and both ARCH documents are Approved (ADR-0020); both ARCH documents are InReview 0.3.1, not Approved; Windows unverified, durability simulated (S15), big-endian untested, wall time unmeasured", "performance": "fabric-gate wall time is recorded in docs/episodes/061-080.md", "release": "Foundation; Linux desktop; Windows unverified (ADR-0006)"}
 ]
}
```

## Measures and enforcement points

- VOL4-R01 to VOL4-R05: `just plan-check` parses the `sl-data` block and applies R-PLAN-VOL04 (added by WO-035): exactly episodes 061-080 in order, title and purpose against SL-REG-002, work orders against the `Traces:` lines both ways, one primary per record whose acceptance list holds the record's command, at most one episode per primary; `just neg-plan` proves each failure.
- VOL4-R06: `just ep-advance` and `just registry-check` once the founder adds the ADR-0020 cap (Open issues).
- VOL4-R07 and VOL4-R08: founder review at WO-063, where `docs/episodes/061-080.md` records the observed numbers.
- Every document: `just doc-check`; the register entry: `just registry-docs-check`.

## Traceability

Master plan Volume 04 block, sections 4.2, 6.1, 6.2 and 6.3; SL-REG-002 episodes 061-080; SL-ARCH-001 and SL-ARCH-002;
work orders WO-035 to WO-063 (waves in `build/design/vol04-draft/WAVES.md` until merged); ADR-0010 to ADR-0020;
requirements REQ-DOCTRINE-001, REQ-DOCTRINE-002, REQ-DOCTRINE-005, REQ-FRAG-002, REQ-FRAG-005, REQ-RELEASE-001.

## Open issues

- The ADR-0020 cap for 061-080 is not yet implemented in `ep-advance` or `registry-check`; that is a governed change to the registry tooling owned by the founder.
- The proposed intra-volume `depends_on` edges of ADR-0020 are a registry amendment the founder applies with an impact note (master plan section 6.3 rule 3); until then every 061-080 record has `depends_on: []`.
- Reading master plan section 2.2 row 08 under ADR-0014 (the Fabric store is the one CAS) needs an impact note for episodes 182 and 348.
- Windows (ADR-0006), big-endian hosts and real power loss are unverified for the whole volume; the seams are SL-ARCH-002 S14-S16.
- The acceptance recipes of 061-080 are implemented (WO-063 landed `fabric-gate` and `acceptance-061-080`); WO-064 fixed most code changes the WO-062 reconciliation found; what remains (undo intents, O(delta) reopen, delete-check scans, schema objects per revision) is an open issue of the ARCH documents for a later work order.

## Change log

| Version | Date | Change |
| :---- | :---- | :---- |
| 0.1.0 | 2026-10-18 | First draft with SL-ARCH-001 and SL-ARCH-002 (WO-035). |
| 0.2.0 | 2026-10-18 | Seams updated to what shipped (WO-063); WO-064 added to the work orders it traces. |
