---
id: ADR-0014
title: "Immutable Merkle revision store; the Fabric object store is the one content-addressed store"
status: Proposed
date: 2026-10-18
decision_refs: []
supersedes: []
---
# ADR-0014: Immutable Merkle revision store; the Fabric object store is the one content-addressed store

## Context

The version-control decision is a native content-addressed revision store (master plan section 1.5). The transaction
lifecycle shows one participant, "Revision store + CAS" (master plan section 2.4), while the subsystem table gives
revisions to 02, the content-addressed store to 08 and accepted revisions, branches and locks to 10 (master plan
section 2.2). Left implicit, the overlap yields two stores of the same bytes, which F2 forbids (master plan section 3).
Replay uses the state hash (episode 327) and collaboration needs immutable revisions and merge bases (episode 341).

## Decision

- Project state MUST be a set of immutable snapshots: a revision object names its parents, a root, the schema_set digest, the tx record digest and a generation; it contains no wall-clock time (master plan section 2.4; section 4.2).
- The root MUST hold a records map, a canonical radix-16 hash array mapped trie keyed by the first 16 bytes of `SHA-256("slf-key/1" || record id)`, whose shape depends only on its entry set, plus a per-document membership map (master plan section 1.5, row Version control).
- A head is a named ref; the only head mover is an atomic backend commit of new objects plus ref compare-and-swap, made only by the transaction manager (master plan section 3, F5).
- One transaction MUST produce at most one revision; commit time and actor live in the commit frame and receipt, which are not part of the revision preimage (master plan section 4.2).
- The Fabric object store MUST be the single content-addressed store of the project; Volume 10 (episode 182) and binary asset versioning (episode 348) extend it instead of building a second CAS (master plan section 2.4; section 3, F2).
- Subsystem 02 MUST own the mechanism (objects, revisions, refs, merge bases, pins) and subsystem 10 MUST own policy (accepted revisions, branch policy, locks and leases) through records in its own `collab.*` domain (master plan section 2.2, rows 02 and 10).
- A revision pin MUST keep a revision and its closure alive through any future collection, and every revision MUST pin its schema_set so it opens read-only under a newer engine (master plan section 4.2; episode 328).

## Consequences

- WO-042 (store seam, memory backend), WO-046 (trie, snapshot, diff, membership), WO-053 (log backend).
- Assigning the CAS to the Fabric store changes how master plan section 2.2 row 08 is read: an impact note and a
  traceability update for episodes 182 and 348 are required (master plan section 6.3 rule 3); the founder records
  them as a registry amendment, not this ADR.
- Hashed keys cost locality; the membership map restores per-document iteration in O(document size).

## Alternatives

- Whole-document snapshots with a document size cap (D3 draft): rejected, a 200-step drag rewrites the document 200
  times and merges become document-granular, which Volume 18 cannot undo.
- Keying the trie by raw record id: rejected, class and scheme bytes are shared, so the top levels degenerate.
- A time field in the revision digest: rejected, replay would need recorded clocks to reproduce digests.
- A second CAS for cooked artifacts in Volume 10: rejected under F2.
