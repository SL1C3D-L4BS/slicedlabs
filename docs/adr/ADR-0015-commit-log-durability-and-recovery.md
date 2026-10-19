---
id: ADR-0015
title: "Durability: one CRC-framed commit log, intact commit frame is the commit point, rollback-and-quarantine"
status: Accepted
date: 2026-10-18
decision_refs: []
supersedes: []
---
# ADR-0015: Durability: one CRC-framed commit log, intact commit frame is the commit point, rollback-and-quarantine

## Context

Episode 076 asks for journals, checkpoints and incomplete-transaction detection; episode 077 for quarantine, repair and
loss reporting (master plan, Volume 04). Commits are atomic (master plan section 2.4). Crash recovery is part of
release gate 3 (master plan, Release gates). The first vertical slice ends with "undo, save, reopen, recover" (master
plan, First vertical slice). Real power loss cannot be produced on the reference machine, so durability must be testable
by simulation (machine fact: tests run on page-cached local storage).

## Decision

- The durable backend MUST append each commit as object frames followed by one commit frame to a single segmented log, `[len u32le][crc32c u32le][type u8][payload]`, and fsync once per commit by default (master plan section 2.4).
- The commit point MUST be the intact commit frame: a commit is valid iff its CRC is intact, its `prev` digest chains to the previous valid commit, and the object frames it counts precede it with a matching objects digest (master plan section 2.4).
- Blobs above 1 MiB MAY be stored as loose files, written and fsynced before the commit frame that names them (master plan, Volume 04, episode 076).
- Checkpoints MUST be derived accelerators; recovery from a checkpoint plus the tail MUST equal recovery from the whole log (master plan, Volume 04, episode 076).
- Recovery MUST truncate a torn tail, count orphan object frames, report open intents, verify the chain and rebuild refs; it is idempotent (master plan, Volume 04, episode 076).
- The default corruption policy MUST be rollback-and-quarantine: detect, move damaged bytes to `quarantine/` (never delete), open at the last intact commit on a recovery view, report exact loss; roll-forward of an intent is opt-in (master plan, Volume 04, episode 077).
- Repairs MUST be transactions with epistemic class inferred; repair never invents data (master plan section 2.4; episode 077).
- A write error or failed fsync after the commit frame was written MUST yield `commit_indeterminate` and stop further commits until reopen decides by frame intactness; a retry with the same idempotency key returns the original receipt (master plan section 2.4).
- One writer process per workspace MUST hold an advisory lock; other processes open read-only (master plan section 2.3: local authoring needs no service).

## Consequences

- WO-053 (vfs, CRC32C, frames, log backend), WO-055 (checkpoints, lock, recovery, sweeps), WO-059 (corruption classes,
  quarantine, fsck). `just fabric-crash-sweep` truncates at every byte offset and flips every bit of committed frames.
- Durability is simulated by a faulty vfs (drop unsynced, tear last append, fail after N), not power-loss tested; the
  episode records say so.
- Windows durability and locking are unverified (ADR-0006): `vfs_win32` returns `unsupported` until a Windows job runs.

## Alternatives

- Journal of intents plus a separate head file updated by rename (D1, D3 drafts): rejected, two files must agree
  after a crash and the rename ordering is platform dependent.
- An embedded database (SQLite, LMDB): rejected, a new dependency and a second format for the same objects.
- Roll-forward by default: rejected, it can surface a half-validated state the user never saw acknowledged.
