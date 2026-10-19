---
id: SL-ARCH-002
kind: ARCH
title: "Transaction, Undo and Recovery Protocol"
status: Draft
version: 0.2.0
owner: founder
episodes: [70, 71, 76, 77, 79, 80]
depends_on: [SL-GOV-001, SL-GOV-006, SL-ARCH-001]
supersedes: []
decisions: [ADR-0009, ADR-0010, ADR-0014, ADR-0015, ADR-0016, ADR-0018, ADR-0019, ADR-0020]
max_lines: 460
---
# SL-ARCH-002: Transaction, Undo and Recovery Protocol

## Purpose

This document controls how project facts change: the protocol by which every authoritative edit becomes a revision
(master plan section 2.4; section 3, F5), how an edit is undone (episode 071), how the durable log is laid out and
how a crash or corruption is survived (episodes 076 and 077), and the hooks that Volumes 17 and 18 build on (master
plan section 4.2). Data formats of records, values, schemas and revisions are SL-ARCH-001; this document refers to
them by rule or invariant id and never restates them.

## Status and decisions

Draft 0.2.0: version 0.1.0 was written after SL-ARCH-001 and before any Volume 04 code (master plan, Authoring
order); 0.2.0 is reconciled with that code by WO-062 (`tx_states`, `failpoints` and `corruption` are bound by
`just fabric-contract-lock`, the builders' decisions are under "Code reconciliation", code still to change is an open
issue naming its work order). Binding
decisions: ADR-0009, ADR-0010, ADR-0014 (revision store), ADR-0015 (commit log and recovery default), ADR-0016 (F5
enforcement), ADR-0018 (undo), ADR-0019 (collaboration and replay seams), ADR-0020 (acceptance state). The founder
approves through `registry/SL-REG-001.approvals.json`. Rule ids are ARCH2-Rnn; invariants are FAB-INV-<area>-nn.

## Normative text

### Transaction model (episode 070, F5)

- ARCH2-R01: Every authoritative mutation MUST enter through `transaction_manager`; `store_backend::commit` takes a `const mutation_capability&` that only the manager (and the test-only backdoor) can construct (ADR-0016; master plan section 3, F5).
- ARCH2-R02: State MUST be immutable snapshots; a head is a ref and only a successful backend commit moves it (ADR-0014).
- ARCH2-R03: A patch MUST be {version, base revision, ordered ops, meta {label, undo group, coalesce key, idempotency key}} with the closed op set of `ops` below and paths of field number, map key or element id, never positions (ADR-0019).
- ARCH2-R04: Before validation a patch MUST be normalised: every mutating op carries `expect` or `captured`, filled from the staged state when the author left it empty (ADR-0018).
- ARCH2-R05: Validation MUST run the stages of `validation_stages` in order, collect every diagnostic, sort them by (stage, record id, path, rule) and return them; a rejected transaction writes nothing (master plan section 2.4, "Structured rejection, nothing written").
- ARCH2-R06: A transaction MUST follow the `tx_states` machine; staging on a closed transaction is `tx_closed`, and destroying an open transaction aborts it (master plan section 2.4).
- ARCH2-R07: Commit MUST follow `commit_steps` under one commit mutex per workspace; the commit point is step C4 (ADR-0015).
- ARCH2-R08: A stale base MUST be rejected with `stale_base` and a conflict report under `reject_stale` (the default); `rebase_if_commutes` MAY rebase only when the footprints of all intervening patches are disjoint from this one (ADR-0019).
- ARCH2-R09: A retried commit with the same (actor, idempotency key) and the same patch digest MUST return the original receipt; a different patch is `idempotency_mismatch` (ADR-0019).
- ARCH2-R10: Before the commit point `abort()` MUST write nothing; after it history is append-only, a revert is a new transaction, and ref creation and reset are transactions recorded in history (ADR-0016).
- ARCH2-R11: Clock and id minting MUST be injected; same base, same patch and same injected values give an identical revision oid, and commit time lives only in the commit frame and the receipt (ADR-0014; ADR-0019).
- ARCH2-R12: Transaction objects MUST be single-threaded; readers hold immutable snapshots and take no lock; one writer process holds the workspace lock and others open read-only (ADR-0015).
- ARCH2-R13: The manager MUST emit tx committed, rejected, conflicted and recovered events with correlation, ordered by commit seq (master plan section 3, F6).
- ARCH2-R14: The invariants stage MUST refuse a derived record whose provenance lacks any F3 field (`provenance_incomplete`) on every commit path (master plan section 3, F3).
- ARCH2-R15: `validate()` MUST be a dry run that reports exactly what `commit()` would and writes nothing (master plan section 2.4).
- ARCH2-R16: One transaction MUST produce at most one revision (ADR-0014).
- ARCH2-R17: Debug and sanitizer builds MUST count every backend mutation in the F5 ledger and treat a mutation outside `f5::authorized_scope` as a bypass: counted, logged as a fatal `fabric/f5` event, `SL_DEBUG_ASSERT` (ADR-0016).
- ARCH2-R18: Schema and domain registries MUST freeze when the workspace opens (ADR-0016).
- ARCH2-R19: Failpoints FP0-FP6 MUST exist as test-armed no-ops and the outcome after a crash at each MUST match `failpoints` (ADR-0015).

```sl-data
{"ops": [
{"op": "create_record", "args": "id, type, body", "inverse": "delete_record with captured body", "footprint": "create(id)"},
{"op": "delete_record", "args": "id, captured", "inverse": "create_record of captured (same id, revival)", "footprint": "remove(id)"},
{"op": "set", "args": "id, path, value, expect", "inverse": "set expect (or unset if expect absent)", "footprint": "write(id, path)"},
{"op": "unset", "args": "id, path, expect", "inverse": "set expect", "footprint": "write(id, path)"},
{"op": "insert_elem", "args": "id, oset path, element id, order key, value", "inverse": "remove_elem", "footprint": "create(id, path/elem)"},
{"op": "remove_elem", "args": "id, oset path, element id, captured", "inverse": "insert_elem of captured", "footprint": "remove(id, path/elem)"},
{"op": "move_elem", "args": "id, oset path, element id, order key, expect order key", "inverse": "move_elem back", "footprint": "write(id, path/elem)"},
{"op": "retype_record", "args": "id, new type, body, captured (migration only)", "inverse": "retype back", "footprint": "write(id)"},
{"op": "annotate", "args": "label, note (no state change)", "inverse": "annotate", "footprint": "none"}],
"validation_stages": ["structural (paths, op legality over absent, live, tombstoned targets)", "schema (types, ranges, presence, finite floats, provenance completeness)", "authority (domain, innermost-domain grants, actor kind)", "references (strong and owned refs live and type-compatible; delete blocked by inbound strong refs; owned children cascade)", "invariants (id class, owned forest acyclic, tombstone immutable, derivation acyclic, registered domain invariants with declared read sets)", "policy (approval hooks, Volume 20)", "locks (lock_authority, Volume 18)"],
"tx_states": {"states": ["Open", "Validating", "Rejected", "Committing", "Committed", "Conflict", "Failed", "Indeterminate", "Aborted"],
"transitions": [["Open", "stage", "Open"], ["Open", "validate", "Validating"], ["Validating", "dry run done", "Open"], ["Open", "commit", "Validating"],
["Validating", "diagnostics with error", "Rejected"], ["Validating", "pass", "Committing"], ["Committing", "base moved, policy refuses", "Conflict"], ["Committing", "lock denied", "Conflict"],
["Committing", "error before commit point", "Failed"], ["Committing", "commit frame durable", "Committed"], ["Committing", "error after frame written", "Indeterminate"], ["Open", "abort or destroy", "Aborted"]],
"errors": {"Rejected": "validation_failed", "Conflict": "stale_base | conflict | locked", "Failed": "store_io | object_corrupt", "Indeterminate": "commit_indeterminate", "Aborted": "tx_aborted"}},
"commit_steps": [
{"step": "C1", "text": "take the commit mutex; re-read head; apply commit_policy to a moved base"},
{"step": "C2", "text": "call lock_authority again under the mutex"},
{"step": "C3", "text": "materialise: encode records and tombstones, rebuild trie spine and membership map, root, schema_set, patch object, tx record, revision"},
{"step": "C4", "text": "backend.commit(cap, batch{objects, ref_updates [{head, expected base, new revision}], commit_time}); log backend: loose blobs fsynced, object frames + commit frame appended, one fsync; COMMIT POINT"},
{"step": "C5", "text": "publish the new head in the in-memory ref table (MVCC)"},
{"step": "C6", "text": "notify commit observers in registration order (dependency index, impact, undo stacks), then events by seq"},
{"step": "C7", "text": "return receipt {revision, root, tx id, seq, commit_time_us, impact summary, undo token}"}],
"failpoints": [
{"fp": "FP0", "at": "before validate", "outcome": "not committed"}, {"fp": "FP1", "at": "after validate", "outcome": "not committed"},
{"fp": "FP2", "at": "after materialise", "outcome": "not committed; objects in memory only"}, {"fp": "FP3", "at": "mid commit frame write", "outcome": "not committed; torn tail truncated on recovery"},
{"fp": "FP4", "at": "after write, before fsync", "outcome": "either, decided by frame intactness; no ack was given, retry by idempotency key"},
{"fp": "FP5", "at": "after fsync, before ack", "outcome": "committed; retry returns the original receipt"}, {"fp": "FP6", "at": "after ack, before events", "outcome": "committed; recovery emits tx recovered"}]}
```

### Undo and redo (episode 071)

- ARCH2-R20: Undo MUST be a new committed transaction applying the inverse of the acting actor's last group and carrying `undo_of` (ADR-0018).
- ARCH2-R21: Undo stacks MUST be per actor and rebuilt from tx records after reopen (ADR-0018).
- ARCH2-R22: A group MUST undo as one transaction or not at all (ADR-0018).
- ARCH2-R23: An intervening edit to a value the inverse expects MUST make undo fail with `undo_conflict`, write nothing and report the footprints (ADR-0018).
- ARCH2-R24: Each op MUST be reversible, compensable or irreversible; undo refuses to cross an irreversible barrier (`undo_barrier`, naming the op) and runs a compensation as its own transaction bracketed by intent frames (ADR-0018).
- ARCH2-R25: Redo MUST be cleared by the actor's next non-undo commit, and redo restores the exact root digest (ADR-0018).
- ARCH2-R26: Coalescing MUST happen inside one open transaction and a drag MUST commit once on release (ADR-0018).
- ARCH2-R27: Delete followed by undo MUST restore the same id and record object id (ADR-0018; ADR-0011).

### Journal format (episode 076)

- ARCH2-R28: A workspace MUST use the layout of `workspace_layout`; `index/` is derived and deletable, `quarantine/` is never deleted by the engine (ADR-0015).
- ARCH2-R29: Log segments, frames, frame types and the commit payload MUST follow `journal` byte for byte, little-endian (ADR-0015).
- ARCH2-R30: A commit frame MUST be valid iff its CRC is intact, its seq is the previous seq plus one, its `prev` equals the SHA-256 of the previous commit payload, and the object frames since the previous commit hash to `objects_digest` with every listed loose blob present and verified (ADR-0015).
- ARCH2-R31: An unknown frame type in 6-127 MUST be `journal_corrupt`; types 128-255 are skippable notes from a newer minor (ADR-0015).
- ARCH2-R32: A checkpoint MUST be a derived file whose checksum covers all its bytes; an invalid one is ignored and reported (ADR-0015).
- ARCH2-R33: Durability MUST be selectable as none, flush or fsync, default fsync, with one fsync per commit (ADR-0015).
- ARCH2-R34: The writer lock MUST be an OFD advisory lock on `.slfabric/lock`; a lock file not locked by a live process is stale and is taken over and reported (ADR-0015).

```sl-data
{"workspace_layout": ["project.slp (locator: format, store, project id)", ".slfabric/FORMAT (text 'slfabric 1.0')", ".slfabric/lock", ".slfabric/log/seg-NNNNNNNN.slg (rotate at 256 MiB)",
".slfabric/loose/ab/<64 hex> (objects above 1 MiB)", ".slfabric/checkpoint/ckpt-<16 hex seq>.slc", ".slfabric/quarantine/<seq or oid>-<what>", ".slfabric/index/ (derived)", ".slfabric/pins/<name> (64 hex revision digest)"],
"journal": {
"segment_header": "24 bytes: 'SLGF', major u8=1, minor u8=0, reserved u16=0, segment number u32le, first seq u64le, crc32c u32le over bytes 0-19",
"frame": "len u32le (= 1 + payload bytes, at most 64 MiB) | crc32c u32le over (type || payload) | type u8 | payload",
"frame_types": [{"type": 1, "name": "object", "payload": "one SLFO-framed object"}, {"type": 2, "name": "commit", "payload": "SLB/1 struct, fields below"},
{"type": 3, "name": "intent_begin", "payload": "struct {1 operation id, 2 kind string, 3 tx id optional, 4 args value}"},
{"type": 4, "name": "intent_end", "payload": "struct {1 operation id, 2 outcome completed|compensated|abandoned}"}, {"type": 5, "name": "note", "payload": "diagnostic, ignored"}],
"commit_fields": ["1 seq u64", "2 prev digest (absent for seq 1)", "3 tx record digest", "4 revision digest", "5 ref_updates list {1 name, 2 expected digest optional, 3 new digest}",
"6 object_count u64", "7 objects_digest = SHA-256 over the 33-byte digests of this commit's object frames in log order", "8 loose blobs list of digests", "9 commit_time_us i64 (unhashed sidecar)", "10 actor id"],
"checkpoint": "'SLCK', major u8, minor u8, SLB/1 struct {1 seq, 2 segment, 3 offset after commit frame, 4 last commit payload digest, 5 refs, 6 schema_set, 7 open intents}, SHA-256 of all preceding bytes",
"crc": "CRC-32C (Castagnoli), reflected, init 0xFFFFFFFF, final xor 0xFFFFFFFF"}}
```

### Recovery (episode 076)

- ARCH2-R35: Opening a workspace for write MUST run `recovery_steps`; recovery is idempotent and a second run reports zero loss and the same heads (ADR-0015).
- ARCH2-R36: An incomplete transaction MUST mean one of: a torn tail frame, orphan object frames after the last valid commit, or an open intent; each is counted in the recovery report (ADR-0015).
- ARCH2-R37: The default MUST be rollback-and-quarantine; roll-forward of an open intent MAY happen only on explicit request (ADR-0015).
- ARCH2-R38: A `commit_indeterminate` workspace MUST refuse further commits (`recovery_required`) until reopened, and reopen MUST decide the commit by frame intactness (ADR-0015).
- ARCH2-R39: Recovery from a checkpoint plus the tail MUST equal recovery from the whole log (ADR-0015).
- ARCH2-R40: The recovery report MUST state lost bytes, truncated frames, orphan objects, open intents, per-ref recovered heads, whether a checkpoint was used, quarantined files and corruption findings (master plan, Volume 04, episode 076).

```sl-data
{"recovery_steps": [
"R1 take the writer lock (writer_locked if a live process holds it; C11 if stale)", "R2 read FORMAT (C8: newer major is format_unsupported, nothing touched)",
"R3 newest checkpoint whose checksum and last-commit digest verify, else segment 1 offset 24 (C12 if a checkpoint is invalid)",
"R4 scan frames: length bounds, CRC, type; the first failure with no valid frame after it in the last segment is a torn tail (C1); a failure followed by valid frames is mid-log damage (C2)",
"R5 per commit: seq + 1, prev chain (C3), objects digest and loose blobs (C4, C5)", "R6 object frames after the last valid commit are orphans: counted, never visible",
"R7 intent_begin without intent_end: open intents, reported with options continue or compensate", "R8 copy torn bytes to quarantine/, then truncate at the failure offset",
"R9 rebuild refs from commit ref_updates; verify each head closure (C4, C5)", "R10 write a new checkpoint; return recovery_report"]}
```

### Corruption policies (episode 077)

- ARCH2-R41: Each class of `corruption` MUST be detected and handled as listed: detect, quarantine (move or copy, never delete), open at the newest verifiable revision on a read-only `recovered/<seq>` view, and report exact loss (ADR-0015).
- ARCH2-R42: A repair MUST be a transaction with epistemic class inferred built only from verifiable sources (an intact copy by hash, the log, a checkpoint, an ancestor revision); repair never invents data (ADR-0015).
- ARCH2-R43: Strict open MUST refuse any finding with `store_corrupt`; lenient open MUST open read-only with flagged records (master plan, Volume 04, episode 077).
- ARCH2-R44: `fsck` MUST walk reachability and verify every object id, and `sl_fabric_fsck` MUST exit non-zero naming the object id or frame offset of each finding (master plan, Volume 04, episode 077).

```sl-data
{"corruption": [
{"class": "C1", "name": "torn tail", "detect": "R4", "policy": "quarantine tail bytes, truncate, open normally", "evidence": "lost_bytes"},
{"class": "C2", "name": "mid-log CRC mismatch", "detect": "R4", "policy": "quarantine segment copy, read-only view at last intact commit before damage", "evidence": "offset, commits after damage"},
{"class": "C3", "name": "broken chain or seq gap", "detect": "R5", "policy": "as C2", "evidence": "seq, expected and found prev"},
{"class": "C4", "name": "object hash mismatch", "detect": "R5, R9, read verify", "policy": "quarantine object, open newest revision whose closure verifies", "evidence": "oid, records affected"},
{"class": "C5", "name": "missing object", "detect": "R5, R9", "policy": "as C4", "evidence": "oid, records affected"},
{"class": "C6", "name": "undecodable record", "detect": "decode", "policy": "quarantine, read-only, repair proposal", "evidence": "oid, rule, offset"},
{"class": "C7", "name": "schema-invalid record", "detect": "check under stored schema_set", "policy": "flag record read-only, repair proposal", "evidence": "record id, diagnostics"},
{"class": "C8", "name": "unknown newer format", "detect": "R2, frame header", "policy": "refuse format_unsupported, touch nothing", "evidence": "found and supported version"},
{"class": "C9", "name": "dangling strong reference at rest", "detect": "fsck", "policy": "report, repair proposal (weaken or restore)", "evidence": "from, to, path"},
{"class": "C10", "name": "id-class confusion", "detect": "fsck, decode", "policy": "quarantine record", "evidence": "record id, type record class"},
{"class": "C11", "name": "stale lock", "detect": "R1", "policy": "take over, report", "evidence": "previous holder info"},
{"class": "C12", "name": "corrupt checkpoint", "detect": "R3", "policy": "ignore, rebuild, report", "evidence": "file name"}]}
```

### Collaboration seams (Volume 18, master plan section 4.2)

- ARCH2-R45: Every patch MUST expose its footprint, and `commutes` MUST be symmetric and sound (ADR-0019).
- ARCH2-R46: `lock_authority::check(origin, footprint)` MUST be consulted at validation and again at step C2; `null_lock_authority` is the default (ADR-0019).
- ARCH2-R47: A `merge_driver` registry keyed by type id MUST exist with the default field-level three-way driver (ADR-0019).
- ARCH2-R48: Concurrent oset inserts at an equal order key MUST converge by element id (ADR-0019).
- ARCH2-R49: Patches MUST be the unit of synchronisation: canonical bytes, base revision, ids minted by the author (ADR-0019; ADR-0011).

### Replay seams (Volume 17, master plan section 4.2)

- ARCH2-R50: Replaying ordered tx records from a base snapshot with the recorded clock values and id_source seed MUST reproduce every revision oid, and the root digest is the state hash (ADR-0019).
- ARCH2-R51: A revision pin MUST keep a revision and its closure through any collection (ADR-0014).
- ARCH2-R52: An old revision MUST open read-only with its stored schema_set under a newer engine (ADR-0014).

### API surface (namespace `sl::fabric`)

```cpp
class mutation_capability;                                   // passkey: friend transaction_manager, testing::backdoor
class store_backend { public: result<object> get(const digest&) const; result<digest> read_ref(std::string_view) const;
  result<commit_ack> commit(const mutation_capability&, commit_batch); result<verify_report> verify(verify_scope) const; };
  // also: result<bool> has(const digest&) const; result<std::vector<ref_entry>> list_refs() const;
class memory_backend; class log_backend;                     // log_backend::open(vfs&, dir, durability | log_options)
class revision_links;                                        // link_reader for verify (revision.hpp)
struct patch; struct patch_op; struct footprint; result<patch> normalize(const patch&, const state_reader&);
patch invert(const patch&); result<patch> compose(const patch&, const patch&); bool commutes(const footprint&, const footprint&) noexcept;
result<patch> rebase_if_disjoint(const patch&, const patch& onto);
class transaction { result<void> stage(const patch&); const staged_state& view() const noexcept; result<std::vector<diagnostic>> validate();
  result<commit_receipt> commit(); std::span<const diagnostic> diagnostics() const noexcept; void abort() noexcept; };
class transaction_manager { result<transaction> begin(tx_options); result<snapshot> head(std::string_view ref = "main") const;
  validator_registry& validators() noexcept; event_bus& events() noexcept; void set_lock_authority(std::shared_ptr<lock_authority>);
  void add_observer(std::shared_ptr<commit_observer>); merge_driver_registry& merge_drivers() noexcept;
  result<snapshot> at(const digest&) const; result<record_id> mint(id_class); failpoint_set& failpoints() noexcept;
  result<commit_receipt> create_ref(name, target, const tx_origin&); result<commit_receipt> reset_ref(name, target, const tx_origin&); };
class undo_manager { undo_manager(transaction_manager&, actor_id, undo_options{ref, intents, compensators});
  result<commit_receipt> undo(); result<commit_receipt> redo(); result<void> rebuild_from_history(const snapshot&);
  begin_group / group_meta / undo_stack / redo_stack / last_conflict; };  class intent_log;   // undo intents (in memory by default)
class workspace { static result<std::unique_ptr<workspace>> create_memory(workspace_options);
  static result<std::unique_ptr<workspace>> create_with(std::shared_ptr<store_backend>, workspace_options);
  static result<std::unique_ptr<workspace>> create(const std::filesystem::path&, workspace_options);
  static result<std::unique_ptr<workspace>> open(const std::filesystem::path&, open_mode, const recovery_policy&, workspace_options);
  const recovery_report& last_recovery() const noexcept; result<snapshot> at(const digest&) const; open_mode mode() const noexcept; };
result<recovered_log> recover_log(vfs&, const std::filesystem::path& store_dir, ...);   // recovery.hpp
namespace f5 { struct ledger { std::uint64_t mutations, bypassing; }; ledger read() noexcept; class authorized_scope;
  void note_mutation(std::string_view site, const store_backend* = nullptr) noexcept; inline constexpr bool enabled; }
std::span<const failpoint_info> failpoint_table() noexcept; std::span<const corruption_policy> corruption_policies() noexcept;
result<fsck_report> fsck(vfs&, const std::filesystem::path&, fsck_options);
```

### Code reconciliation (WO-062)

What the Volume 04 code does where version 0.1.0 was silent or said otherwise; each item is descriptive and the rules above stand. Items that still need a code change are open issues below.

- Patches (WO-043): `patch_op` carries `expect_type` (retype inverse) and a three-state expectation (unchecked, absent, value); the patch layer has no tombstones (deleted = absent; tombstones are written at commit); a failed expectation is `conflict`; the patch value layout is documented in patch.hpp (newer major or unknown op kind `format_unsupported`, malformed `type_mismatch`); `compose` takes base and meta from the first patch and is associative; diagnostics carry their stage for the ARCH2-R05 sort.
- Transactions (WO-050): tx id = derive(tx, actor, "tx/" + hex of the tx record object id), recoverable and entropy-free; the manager writes the patch object (kind 8, base = parent); the tx record holds the first `undo_of` only (the patch keeps the list) and has no field for per-op provenance or epistemic class changes (`class_changes` are judged, not stored). The built-in validator chain is fabric.structural, fabric.schema, fabric.authority, fabric.references, fabric.invariants, fabric.provenance and fabric.identity, and `create_project` / `open_project` add fabric.manifest; locks are asked after the validators (a denial is Conflict with `locked`). Idempotency compares the coalesced ops and meta without base and expectations; the index is rebuilt from tx records on open and rebuilt receipts have seq 0, commit_time_us 0 and `recovered`. `create_ref` and `reset_ref` commit annotate-only patches, a reset revision has parents {target, old head}; `head()` of a missing ref is the empty snapshot; `tx_options.sync` is recorded, the store decides durability.
- F5 (WO-051): every backend commit call counts one mutation (plus one for intent writes); a bypass is always counted and logged fatal on `fabric/f5`, and the debug assertion fires only on a store a live transaction manager governs, so test stores seeded through `testing::backdoor` do not abort. R-F5-MUTABLE exempts mutex and atomic members.
- Undo (WO-052): undo intents go to an `intent_log` (in memory by default), not to journal frames; a compensation is an annotate op marked compensable with note {1 command, 2 args}, committed newest-first after the inverse with key `sl.undo.compensate/<op id>`; stacks follow one ref's first-parent history through a commit observer and skip annotate-only commits. Inverse staging failures are `undo_conflict`, an empty redo stack `undo_empty`. ARCH2-R25's exact root holds when no id was retired in between: an undone create leaves a tombstone, which is history.
- Journal (WO-053): `commit_batch` has no tx, revision or actor fields, so commit frame fields 3 and 4 take the first tx and revision objects of the batch and field 10 is the null actor; frame type 0 is `journal_corrupt`; intent frames belong to the following commit and an intents-only batch still writes a commit frame; the loose threshold is measured on the SLFO frame size and a loose blob is written as `<hex>.tmp`, fsynced, renamed and its directory synced; the durability flush is fdatasync; FORMAT is `slfabric 1.0\n`; segment names have 8 decimal digits; a commit never spans segments.
- Recovery (WO-055): checkpoint field 8 is the offset of the last commit frame; checkpoints are written only by a write open, 2 kept; damage before the used checkpoint is left to read verification and fsck. C1 is only a partial frame or torn segment header (whole orphan frames are counted, not corruption); segment header damage is C2; an unparsable payload C6; bracketing C3; objects digest C4; a missing loose blob or linked object C5. Quarantine names: `<seq>-tail-<seg>-<off>`, `<seq>-header-<seg>`, `<seq>-checkpoint-<name>`, `<oid>-loose-tmp`. A read-only open only inspects (a needed repair is `recovery_required`) and refuses commits with core `permission_denied`; C11 uses `writer_locked`; `recovery_policy.open_store = false` gives the report only.
- Corruption and fsck (WO-059): C5 has no quarantine; a bad pin file is reported as C6; tail damage with no resync point is C1; a lenient write open is allowed only for C1, C9, C11 and C12; fsck exits 2 for a missing or unreadable store and accepts a workspace or a `.slfabric` directory.
- Smaller decisions, as built: `rebase_if_disjoint` returns the patch unchanged (the new base is the caller's), order-key conflicts are reported as bytes and remove beats a move of the same element (WO-043); the manager publishes one recovered event per ref on every open, `workspace_options.subscribers` and `omit_builtin` exist, rebase refuses multi-parent revisions (WO-050); vfs `pwrite` on an append handle is `invalid_argument`, locking is F_OFD_SETLK else F_SETLK, and the Windows vfs is a stub (WO-053); nested `begin_group` joins the open group and observers cannot be removed (destroyed managers leave a no-op observer) (WO-052); the lock holder text is not fsynced (WO-055); quarantine names `<seq>-<file>`, `<oid>-loose`, `<oid>-object`, and flagged records come from `check_records` on the recovered revision (WO-059).
- Replay seams (WO-062): FAB-INV-RPL-01 replays the first-parent history from the store alone (revision, tx record, patch object) with the recorded commit times and the same seed; FAB-INV-RPL-02 binds the pin to a ref and the sweep to the verify reachability walk (revision_links from every ref), since no collector and no `.slfabric/pins` reader exist yet; FAB-INV-RPL-03 opens an old revision under a newer registry with its records' stored type versions and its schema_set digest.
- Demo (WO-062): `just fabric-demo` and the system test `fabric_demo` run `sl_fabric_cli demo`: create, impact, undo, commit, reopen, a crash at FP4 (a torn commit frame before fsync), recovery with the report, the retry by idempotency key, and the F2 and F5 counters.

### Invariants

```sl-data
{"invariants": [
{"id": "FAB-INV-STO-01", "rule": "ARCH2-R02", "text": "get verifies the object id; a mismatch is object_corrupt and returns no bytes", "test": "tests/unit/fabric/store_cas_test.cpp", "wo": "WO-042", "verified_by": "just fabric-test store"},
{"id": "FAB-INV-STO-02", "rule": "ARCH2-R02", "text": "putting an existing object is idempotent", "test": "tests/unit/fabric/store_put_test.cpp", "wo": "WO-042", "verified_by": "just fabric-test store"},
{"id": "FAB-INV-STO-03", "rule": "ARCH2-R07", "text": "a commit batch is all or nothing under an injected failure at each step", "test": "tests/unit/fabric/store_atomic_test.cpp", "wo": "WO-042", "verified_by": "just fabric-test store"},
{"id": "FAB-INV-STO-04", "rule": "ARCH2-R02", "text": "ref compare-and-swap: expected mismatch rejects, create-if-absent works", "test": "tests/unit/fabric/store_ref_test.cpp", "wo": "WO-042", "verified_by": "just fabric-test store"},
{"id": "FAB-INV-STO-05", "rule": "ARCH2-R02", "text": "the backend contract suite passes on memory_backend", "test": "tests/unit/fabric/store_contract_memory_test.cpp", "wo": "WO-042", "verified_by": "just fabric-test store"},
{"id": "FAB-INV-STO-06", "rule": "ARCH2-R29", "text": "the backend contract suite passes on log_backend over the posix and faulty vfs", "test": "tests/integration/fabric/logstore_contract_test.cpp", "wo": "WO-053", "verified_by": "just fabric-test logstore"},
{"id": "FAB-INV-STO-07", "rule": "ARCH2-R01", "text": "mutation_capability is not constructible, copyable or movable outside its friends; commit without it does not compile", "test": "tests/unit/fabric/store_capability_test.cpp", "wo": "WO-042", "verified_by": "just fabric-test store"},
{"id": "FAB-INV-PAT-01", "rule": "ARCH2-R03", "text": "ops apply deterministically and check expect; insert_elem on a list is rejected", "test": "tests/unit/fabric/patch_apply_test.cpp", "wo": "WO-043", "verified_by": "just fabric-test patch"},
{"id": "FAB-INV-PAT-02", "rule": "ARCH2-R45", "text": "the footprint covers every record and path an op reads or writes", "test": "tests/unit/fabric/patch_footprint_test.cpp", "wo": "WO-043", "verified_by": "just fabric-test patch"},
{"id": "FAB-INV-PAT-03", "rule": "ARCH2-R03", "text": "patch to value round trip keeps the version; an unknown op kind under the same major is rejected", "test": "tests/unit/fabric/patch_value_test.cpp", "wo": "WO-043", "verified_by": "just fabric-test patch"},
{"id": "FAB-INV-PAT-04", "rule": "ARCH2-R04", "text": "normalize fills expect and captured from the staged state; a normalised patch is self-inverting", "test": "tests/unit/fabric/patch_normalize_test.cpp", "wo": "WO-043", "verified_by": "just fabric-test patch"},
{"id": "FAB-INV-UNDO-01", "rule": "ARCH2-R20", "text": "apply(invert(p)) restores the root digest for 500 seeded patches over every op kind", "test": "tests/unit/fabric/patch_invert_test.cpp", "wo": "WO-043", "verified_by": "just fabric-test patch"},
{"id": "FAB-INV-UNDO-02", "rule": "ARCH2-R20", "text": "invert(invert(p)) equals p", "test": "tests/unit/fabric/patch_invert_twice_test.cpp", "wo": "WO-043", "verified_by": "just fabric-test patch"},
{"id": "FAB-INV-UNDO-03", "rule": "ARCH2-R22", "text": "apply(compose(a, b)) equals applying a then b", "test": "tests/unit/fabric/patch_compose_test.cpp", "wo": "WO-043", "verified_by": "just fabric-test patch"},
{"id": "FAB-INV-MRG-01", "rule": "ARCH2-R45", "text": "commutes is symmetric, and when true both orders give the same root", "test": "tests/unit/fabric/merge_commutes_test.cpp", "wo": "WO-043", "verified_by": "just fabric-test merge"},
{"id": "FAB-INV-MRG-02", "rule": "ARCH2-R08", "text": "rebase_if_disjoint equals sequential application; overlapping footprints refuse", "test": "tests/unit/fabric/merge_rebase_test.cpp", "wo": "WO-043", "verified_by": "just fabric-test merge"},
{"id": "FAB-INV-MRG-03", "rule": "ARCH2-R48", "text": "concurrent oset inserts converge in both orders, ties broken by element id", "test": "tests/unit/fabric/merge_oset_test.cpp", "wo": "WO-043", "verified_by": "just fabric-test merge"},
{"id": "FAB-INV-MRG-04", "rule": "ARCH2-R47", "text": "the default merge driver merges disjoint field edits and reports overlapping ones", "test": "tests/unit/fabric/merge_driver_test.cpp", "wo": "WO-043", "verified_by": "just fabric-test merge"},
{"id": "FAB-INV-TX-01", "rule": "ARCH2-R19", "text": "at every failpoint FP0-FP6 the head is the old or the new revision, never between", "test": "tests/unit/fabric/tx_atomic_test.cpp", "wo": "WO-050", "verified_by": "just fabric-test tx"},
{"id": "FAB-INV-TX-02", "rule": "ARCH2-R12", "text": "staged changes are invisible to other snapshots until commit", "test": "tests/unit/fabric/tx_isolation_test.cpp", "wo": "WO-050", "verified_by": "just fabric-test tx"},
{"id": "FAB-INV-TX-03", "rule": "ARCH2-R08", "text": "a stale base under reject_stale is stale_base with the head unchanged", "test": "tests/unit/fabric/tx_stale_test.cpp", "wo": "WO-050", "verified_by": "just fabric-test tx"},
{"id": "FAB-INV-TX-04", "rule": "ARCH2-R05", "text": "a transaction rejected at each stage leaves the store byte-identical", "test": "tests/unit/fabric/tx_reject_clean_test.cpp", "wo": "WO-050", "verified_by": "just fabric-test tx"},
{"id": "FAB-INV-TX-05", "rule": "ARCH2-R05", "text": "diagnostics are complete and sorted by stage, record, path, rule", "test": "tests/unit/fabric/tx_diagnostics_test.cpp", "wo": "WO-050", "verified_by": "just fabric-test tx"},
{"id": "FAB-INV-TX-06", "rule": "ARCH2-R09", "text": "a retry with the same idempotency key returns the original receipt; another patch is idempotency_mismatch", "test": "tests/unit/fabric/tx_idempotent_test.cpp", "wo": "WO-050", "verified_by": "just fabric-test tx"},
{"id": "FAB-INV-TX-07", "rule": "ARCH2-R11", "text": "same base, patch and injected values give the golden revision oid whatever the commit time", "test": "tests/unit/fabric/tx_determinism_test.cpp", "wo": "WO-050", "verified_by": "just fabric-test tx"},
{"id": "FAB-INV-TX-08", "rule": "ARCH2-R10", "text": "history is append-only and abort leaves no trace", "test": "tests/unit/fabric/tx_history_test.cpp", "wo": "WO-050", "verified_by": "just fabric-test tx"},
{"id": "FAB-INV-TX-09", "rule": "ARCH2-R05", "text": "validators and readers inside a transaction see staged state", "test": "tests/unit/fabric/tx_read_your_writes_test.cpp", "wo": "WO-050", "verified_by": "just fabric-test tx"},
{"id": "FAB-INV-TX-10", "rule": "ARCH2-R12", "text": "N reader threads and one writer see monotonic revisions with consistent roots (TSan)", "test": "tests/integration/fabric/tx_concurrent_test.cpp", "wo": "WO-050", "verified_by": "just fabric-test tx"},
{"id": "FAB-INV-TX-11", "rule": "ARCH2-R46", "text": "a lock_authority denial is locked and writes nothing", "test": "tests/unit/fabric/tx_lock_test.cpp", "wo": "WO-050", "verified_by": "just fabric-test tx"},
{"id": "FAB-INV-TX-12", "rule": "ARCH2-R08", "text": "an expect mismatch is conflict with a report listing footprints; rebase_if_commutes rebases only disjoint patches", "test": "tests/unit/fabric/tx_conflict_test.cpp", "wo": "WO-050", "verified_by": "just fabric-test tx"},
{"id": "FAB-INV-TX-13", "rule": "ARCH2-R14", "text": "a derived record without complete provenance is refused at the invariants stage", "test": "tests/unit/fabric/tx_provenance_test.cpp", "wo": "WO-050", "verified_by": "just fabric-test tx"},
{"id": "FAB-INV-TX-14", "rule": "ARCH2-R15", "text": "validate reports what commit would and writes nothing", "test": "tests/unit/fabric/tx_dryrun_test.cpp", "wo": "WO-050", "verified_by": "just fabric-test tx"},
{"id": "FAB-INV-TX-15", "rule": "ARCH2-R16", "text": "one commit makes exactly one revision whose tx record decodes to the committed patch", "test": "tests/unit/fabric/tx_record_test.cpp", "wo": "WO-050", "verified_by": "just fabric-test tx"},
{"id": "FAB-INV-TX-16", "rule": "ARCH2-R13", "text": "committed, rejected and conflicted events carry correlation and arrive in seq order", "test": "tests/unit/fabric/tx_events_test.cpp", "wo": "WO-050", "verified_by": "just fabric-test tx"},
{"id": "FAB-INV-F5-01", "rule": "ARCH2-R17", "text": "a normal commit increments mutations and never bypassing", "test": "tests/unit/fabric/f5_ledger_test.cpp", "wo": "WO-051", "verified_by": "just fabric-test f5"},
{"id": "FAB-INV-F5-02", "rule": "ARCH2-R17", "text": "a rogue commit through testing::backdoor is counted, logged and trips the captured assertion; Release asserts the ledger is compiled out", "test": "tests/unit/fabric/f5_bypass_test.cpp", "wo": "WO-051", "verified_by": "just fabric-test f5"},
{"id": "FAB-INV-F5-03", "rule": "ARCH2-R01", "text": "public headers expose no setter, mutable member, extra friend or capability leak", "test": "tools/fabric/test_surface_check.py", "wo": "WO-051", "verified_by": "just fabric-surface-check"},
{"id": "FAB-INV-F5-04", "rule": "ARCH2-R01", "text": "commit without a capability and capability construction outside friends do not compile", "test": "tests/unit/fabric/f5_compile_test.cpp", "wo": "WO-051", "verified_by": "just fabric-test f5"},
{"id": "FAB-INV-F5-05", "rule": "ARCH2-R18", "text": "registries refuse changes after the workspace opens", "test": "tests/unit/fabric/f5_freeze_test.cpp", "wo": "WO-051", "verified_by": "just fabric-test f5"},
{"id": "FAB-INV-UNDO-04", "rule": "ARCH2-R20", "text": "undo is a committed transaction with undo_of; history only grows", "test": "tests/unit/fabric/undo_basic_test.cpp", "wo": "WO-052", "verified_by": "just fabric-test undo"},
{"id": "FAB-INV-UNDO-05", "rule": "ARCH2-R22", "text": "a group undoes atomically under injected failure", "test": "tests/unit/fabric/undo_group_test.cpp", "wo": "WO-052", "verified_by": "just fabric-test undo"},
{"id": "FAB-INV-UNDO-06", "rule": "ARCH2-R25", "text": "redo restores the exact root and is cleared by a new non-undo commit", "test": "tests/unit/fabric/undo_redo_test.cpp", "wo": "WO-052", "verified_by": "just fabric-test undo"},
{"id": "FAB-INV-UNDO-07", "rule": "ARCH2-R24", "text": "an irreversible op is a barrier: undo_barrier names it", "test": "tests/unit/fabric/undo_barrier_test.cpp", "wo": "WO-052", "verified_by": "just fabric-test undo"},
{"id": "FAB-INV-UNDO-08", "rule": "ARCH2-R24", "text": "a compensation runs as its own transaction inside intent_begin and intent_end", "test": "tests/unit/fabric/undo_compensation_test.cpp", "wo": "WO-052", "verified_by": "just fabric-test undo"},
{"id": "FAB-INV-UNDO-09", "rule": "ARCH2-R23", "text": "an intervening edit makes undo undo_conflict and writes nothing", "test": "tests/unit/fabric/undo_conflict_test.cpp", "wo": "WO-052", "verified_by": "just fabric-test undo"},
{"id": "FAB-INV-UNDO-10", "rule": "ARCH2-R21", "text": "undo stacks are rebuilt from tx records after reopen", "test": "tests/unit/fabric/undo_rebuild_test.cpp", "wo": "WO-052", "verified_by": "just fabric-test undo"},
{"id": "FAB-INV-UNDO-11", "rule": "ARCH2-R26", "text": "200 coalesced sets in one open transaction make one revision and one undo entry", "test": "tests/unit/fabric/undo_coalesce_test.cpp", "wo": "WO-052", "verified_by": "just fabric-test undo"},
{"id": "FAB-INV-UNDO-12", "rule": "ARCH2-R27", "text": "delete then undo restores the same id and record object id", "test": "tests/unit/fabric/undo_delete_test.cpp", "wo": "WO-052", "verified_by": "just fabric-test undo"},
{"id": "FAB-INV-JRN-01", "rule": "ARCH2-R29", "text": "frame codec: CRC-32C vectors, length bounds and type rules", "test": "tests/unit/fabric/journal_frame_test.cpp", "wo": "WO-053", "verified_by": "just fabric-test journal"},
{"id": "FAB-INV-JRN-02", "rule": "ARCH2-R30", "text": "a commit frame is valid iff CRC, seq, prev chain and objects digest hold", "test": "tests/unit/fabric/journal_commit_test.cpp", "wo": "WO-053", "verified_by": "just fabric-test journal"},
{"id": "FAB-INV-JRN-03", "rule": "ARCH2-R31", "text": "unknown frame types 6-127 are journal_corrupt; 128-255 are skipped", "test": "tests/unit/fabric/journal_types_test.cpp", "wo": "WO-053", "verified_by": "just fabric-test journal"},
{"id": "FAB-INV-JRN-04", "rule": "ARCH2-R29", "text": "segment rotation keeps seq and the prev chain across files", "test": "tests/unit/fabric/journal_segment_test.cpp", "wo": "WO-053", "verified_by": "just fabric-test journal"},
{"id": "FAB-INV-JRN-05", "rule": "ARCH2-R33", "text": "the faulty vfs is exact: drop unsynced, tear last append, fail after N; win32 vfs is unsupported", "test": "tests/unit/fabric/vfs_faulty_test.cpp", "wo": "WO-053", "verified_by": "just fabric-test vfs"},
{"id": "FAB-INV-RCV-01", "rule": "ARCH2-R35", "text": "truncation at every byte offset of a 40-commit log recovers to the oracle after k commits", "test": "tests/integration/fabric/recovery_truncation_sweep_test.cpp", "wo": "WO-055", "verified_by": "just fabric-crash-sweep"},
{"id": "FAB-INV-RCV-02", "rule": "ARCH2-R30", "text": "every single-bit flip in committed frames is detected or provably harmless", "test": "tests/integration/fabric/recovery_bitflip_sweep_test.cpp", "wo": "WO-055", "verified_by": "just fabric-crash-sweep"},
{"id": "FAB-INV-RCV-03", "rule": "ARCH2-R30", "text": "a deleted middle commit is detected by the prev chain", "test": "tests/unit/fabric/recovery_chain_test.cpp", "wo": "WO-055", "verified_by": "just fabric-test recovery"},
{"id": "FAB-INV-RCV-04", "rule": "ARCH2-R35", "text": "recovery is idempotent", "test": "tests/unit/fabric/recovery_idempotent_test.cpp", "wo": "WO-055", "verified_by": "just fabric-test recovery"},
{"id": "FAB-INV-RCV-05", "rule": "ARCH2-R36", "text": "open intents and orphan objects are reported and never visible", "test": "tests/unit/fabric/recovery_incomplete_test.cpp", "wo": "WO-055", "verified_by": "just fabric-test recovery"},
{"id": "FAB-INV-RCV-06", "rule": "ARCH2-R39", "text": "checkpoint plus tail equals the whole-log scan", "test": "tests/unit/fabric/recovery_checkpoint_test.cpp", "wo": "WO-055", "verified_by": "just fabric-test recovery"},
{"id": "FAB-INV-RCV-07", "rule": "ARCH2-R40", "text": "recovery report fields equal the oracle exactly", "test": "tests/unit/fabric/recovery_report_test.cpp", "wo": "WO-055", "verified_by": "just fabric-test recovery"},
{"id": "FAB-INV-RCV-08", "rule": "ARCH2-R34", "text": "a second writer is writer_locked; a stale lock is taken over and reported", "test": "tests/unit/fabric/workspace_lock_test.cpp", "wo": "WO-055", "verified_by": "just fabric-test workspace"},
{"id": "FAB-INV-RCV-09", "rule": "ARCH2-R35", "text": "a crash after every storage primitive of seeded workloads loses no acknowledged commit and recovers to the oracle", "test": "tests/integration/fabric/recovery_crash_sweep_test.cpp", "wo": "WO-055", "verified_by": "just fabric-crash-sweep"},
{"id": "FAB-INV-RCV-10", "rule": "ARCH2-R38", "text": "an indeterminate commit blocks commits, is decided on reopen, and a keyed retry returns the receipt", "test": "tests/unit/fabric/recovery_indeterminate_test.cpp", "wo": "WO-055", "verified_by": "just fabric-test recovery"},
{"id": "FAB-INV-RCV-11", "rule": "ARCH2-R07", "text": "one commit's storage primitive trace on the posix vfs equals the golden protocol order", "test": "tests/integration/fabric/recovery_optrace_test.cpp", "wo": "WO-055", "verified_by": "just fabric-test recovery"},
{"id": "FAB-INV-COR-01", "rule": "ARCH2-R41", "text": "each class C1-C12 is detected and classified on planted fixture stores", "test": "tests/integration/fabric/corruption_classes_test.cpp", "wo": "WO-059", "verified_by": "just fabric-test corruption"},
{"id": "FAB-INV-COR-02", "rule": "ARCH2-R41", "text": "quarantine never deletes; quarantined bytes equal the damaged bytes", "test": "tests/unit/fabric/corruption_quarantine_test.cpp", "wo": "WO-059", "verified_by": "just fabric-test corruption"},
{"id": "FAB-INV-COR-03", "rule": "ARCH2-R41", "text": "the loss report equals the oracle exactly and surviving records verify", "test": "tests/unit/fabric/corruption_loss_report_test.cpp", "wo": "WO-059", "verified_by": "just fabric-test corruption"},
{"id": "FAB-INV-COR-04", "rule": "ARCH2-R42", "text": "repairs are transactions with epistemic class inferred and never invent data", "test": "tests/unit/fabric/corruption_repair_tx_test.cpp", "wo": "WO-059", "verified_by": "just fabric-test corruption"},
{"id": "FAB-INV-COR-05", "rule": "ARCH2-R43", "text": "strict open refuses with store_corrupt; lenient open is read-only at the newest verifiable revision", "test": "tests/unit/fabric/corruption_open_mode_test.cpp", "wo": "WO-059", "verified_by": "just fabric-test corruption"},
{"id": "FAB-INV-COR-06", "rule": "ARCH2-R44", "text": "sl_fabric_fsck exits non-zero naming the object id or offset of each planted finding", "test": "just fabric-fsck", "wo": "WO-059", "verified_by": "just fabric-fsck"},
{"id": "FAB-INV-RPL-01", "rule": "ARCH2-R50", "text": "replaying tx records with recorded clock and seed reproduces every revision oid", "test": "tests/integration/fabric/replay_digest_test.cpp", "wo": "WO-062", "verified_by": "just fabric-conformance"},
{"id": "FAB-INV-RPL-02", "rule": "ARCH2-R51", "text": "a pinned revision and its closure survive a reachability sweep", "test": "tests/unit/fabric/replay_pin_test.cpp", "wo": "WO-062", "verified_by": "just fabric-conformance"},
{"id": "FAB-INV-RPL-03", "rule": "ARCH2-R52", "text": "an old revision opens read-only with its stored schema_set under a newer schema registry", "test": "tests/unit/fabric/replay_schema_test.cpp", "wo": "WO-062", "verified_by": "just fabric-conformance"}],
"retired": []}
```

## Measures and enforcement points

- F5 (ARCH2-R01, R17): numerator mutations bypassing the transaction manager, denominator all mutations, threshold 0, instrumented in Debug and sanitizer builds; `just fabric-test f5`, `just fabric-surface-check`, `just neg-fabric-f5` (planted setter and removed scope line) (master plan section 3, F5).
- Atomicity and isolation (ARCH2-R05 to R16): `just fabric-test tx`; `just workflow ci-tsan` for FAB-INV-TX-10.
- Crash safety (ARCH2-R29 to R40): `just fabric-crash-sweep` (truncation, bit flip and per-primitive crash sweeps; sizes scale with `SL_FABRIC_SWEEP_SCALE`), `just neg-fabric-crash-sweep` (a copy with the head published before the objects are durable must fail); `just fabric-soak` runs random seeds outside `check` and prints the seed on failure.
- Corruption (ARCH2-R41 to R44): `just fabric-fsck`, `just neg-fabric-fsck`.
- Memory safety: `just workflow ci-asan` in the store, codec, log and recovery work orders; `just tidy` where storage code lands (ADR-0009).
- Transaction coverage on the scorecard (master plan section 3.3): `just fabric-scorecard` exports ledger numerator and denominator, invariant coverage and objects per commit, with `just neg-fabric-scorecard`.
- Performance (master plan section 6.1): work-bound proxies only (objects per commit, validator invocations at most changed records times validators, fsyncs per commit = 1); wall-time budgets are Volume 16.

## Traceability

Master plan sections 2.1, 2.3, 2.4, 3 (F3 refusal, F5, F6), 3.3, 4.2, 6.1, Release gates (gate 3, crash recovery);
episodes SL-EP-070, SL-EP-071, SL-EP-076, SL-EP-077, SL-EP-079, SL-EP-080; requirements REQ-FRAG-005,
REQ-DOCTRINE-002, REQ-RELEASE-001, REQ-RELEASE-003 (crash recovery part); decisions ADR-0009, ADR-0010, ADR-0014 to
ADR-0016, ADR-0018 to ADR-0020; consumers Volume 06 episode 116 (command system), Volume 17, Volume 18, Volume 20
episode 390. SL-ARCH-001 for data formats.

## Open issues

Named seams: what Volume 04 leaves to later volumes, with the hook that exists now (none is silently decided).

```sl-data
{"seams": [
{"id": "S1", "volume": 18, "episodes": [345, 346, 347], "what": "type-specific merge drivers (graph, scene)", "hook": "merge_driver registry, default field-level driver"},
{"id": "S2", "volume": 18, "episodes": [349], "what": "locks and leases with expiry", "hook": "lock_authority, null_lock_authority"},
{"id": "S3", "volume": 18, "episodes": [352, 354, 355], "what": "operation-log transport and remote patch admission", "hook": "canonical patch bytes, idempotency keys"},
{"id": "S4", "volume": 18, "episodes": [358], "what": "selective undo by transform against intervening patches", "hook": "expect, footprints, undo_conflict report"},
{"id": "S5", "volume": 18, "episodes": [341, 342], "what": "accepted revisions, branch policy, review marks in collab.*", "hook": "refs, merge_base, revision DAG with two parents"},
{"id": "S6", "volume": 18, "episodes": [356], "what": "provenance across branches", "hook": "provenance on tx records, oid-addressed"},
{"id": "S7", "volume": 18, "episodes": [337, 343], "what": "pack files, garbage collection, retention of unpinned revisions", "hook": "pins, reachability walk in verify"},
{"id": "S8", "volume": 10, "episodes": [182, 348], "what": "large-blob chunking and binary asset versioning", "hook": "loose objects, blob_ref, single CAS (ADR-0014)"},
{"id": "S9", "volume": 17, "episodes": [327, 331], "what": "runtime state hashing beyond the document root", "hook": "root digest as document state hash"},
{"id": "S10", "volume": 17, "episodes": [322, 336], "what": "recorded clock and entropy streams for runtime replay", "hook": "injected clock, deterministic_source(seed, stream)"},
{"id": "S11", "volume": 17, "episodes": [328], "what": "schema-aware restore of runtime snapshots", "hook": "schema_set per revision, read-only old revisions"},
{"id": "S12", "volume": 17, "episodes": [333], "what": "branching replay histories", "hook": "refs and pins on revisions"},
{"id": "S13", "volume": 18, "episodes": [355], "what": "multi-process writers and the collaboration service as a peer", "hook": "one writer lock, read-only followers"},
{"id": "S14", "volume": 26, "episodes": [], "what": "Windows and macOS durability and locking (ADR-0006)", "hook": "vfs interface; vfs_win32 returns unsupported"},
{"id": "S15", "volume": 21, "episodes": [], "what": "real power-loss and lost-fsync testing", "hook": "faulty vfs simulation only"},
{"id": "S16", "volume": 10, "episodes": [], "what": "hash agility (BLAKE3) and big-endian hosts", "hook": "algo byte in digests; little-endian layouts"}]}
```

- Long staged transactions (a sculpt stroke, a large import) cannot spill a draft to the log; an uncommitted stroke is lost on crash.
- Per-actor undo versus a per-document stack for multi-user sessions is decided for Volume 04 only (ADR-0018).
- Whether ref-only transactions need approval gates (Volume 20) is open.
- The default fsync per commit against edit latency is unmeasured; Volume 16 measures it.
- Whether the collaboration service (Rust) runs this transaction manager or a port is undecided (ADR-0019).
- Selection changes are `shell` document transactions; keeping them out of undo history may need a per-actor ref.
- Code changes the reconciliation found (WO-062). WO-063 releases without new Fabric code, so each names WO-064, a Fabric hardening work order the commander still has to cut.
  - Stale comments: the built-in validator list in transaction.hpp and "the only revival the structural validator accepts" in undo.hpp predate WO-058 (R-INV-ID-REUSE, fabric.identity) (WO-064).
  - A lenient recovered workspace reports `mode()` write while commits are refused with `permission_denied` (WO-064: report read_only).
  - Undo intents are not journal frames 3 and 4: the manager has no intent call (WO-064); undo commits through the entry's last subsystem and carries no provenance, so a cross-subsystem group or a revived derived record can be refused (WO-064).
  - Reopen is not O(delta): `log_backend::open` hashes every object and the manager rebuilds the idempotency index from the whole history; rebuilt receipts lose seq and commit time (WO-064).
  - Newer-minor (R-SCH-NEWER) and deprecation (R-SCH-DEPRECATED) validators are opt-in; the manager does not register them by default (WO-064).
  - Delete checks scan the base snapshot (O(N)); unchanged records get no second-owner check; ref targets are checked by id class only (WO-064).
  - Test budget: fabric_tree_history, fabric_tx_atomic and fabric_deps_cycle run near the 10 s unit timeout under sanitizers and load; the truncation sweep near its 60 s (WO-064: shrink or move to the integration tier).
  - `testing::backdoor` commits print one fatal `fabric/f5` line each in passing tests (WO-064: quieter logging for ungoverned stores); gcc accepts a subclass of `mutation_capability` in a requires-expression (WO-064: make it final).
  - No `just fixtures-lock` recipe exists; the lock is `python3 -I tools/testing/fixtures.py lock` (WO-063 or WO-064: add the recipe).
  - Revision pins: `.slfabric/pins/<name>` is checked by fsck but nothing reads it as a collection root, and no collector exists (S7, Volume 18).

## Change log

| Version | Date | Change |
| :---- | :---- | :---- |
| 0.1.0 | 2026-10-18 | First draft, after SL-ARCH-001 and before any Volume 04 code (WO-035). |
| 0.2.0 | 2026-10-18 | Reconciled with the Volume 04 code (WO-062): API surface rewritten to the headers; "Code reconciliation" records the builders' decisions of WO-043 to WO-059 and the replay seam bindings; code changes listed as open issues for WO-064; `tx_states`, `failpoints` and `corruption` bound by `just fabric-contract-lock`; nothing retired. |
