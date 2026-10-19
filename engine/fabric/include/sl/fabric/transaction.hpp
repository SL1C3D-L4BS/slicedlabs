#pragma once

// The transaction manager: the sole mutation path (WO-050, SL-ARCH-002 ARCH2-R01 to ARCH2-R19,
// ARCH2-R46; master plan section 2.4 and section 3, F5; ADR-0015, ADR-0016, ADR-0019).
//
// Every authoritative change enters through transaction_manager::begin(): a transaction stages
// patch ops over an immutable snapshot (read-your-writes through view()), validate() is a dry run
// and commit() runs the commit protocol. The manager is the only production code that constructs
// a mutation_capability (store.hpp), so store_backend::commit is unreachable except through
// commit(), create_ref() and reset_ref() (and the test-only testing::backdoor).
//
// Commit (SL-ARCH-002 `commit_steps`): the staged ops are coalesced (compose) and normalised over
// the base (ARCH2-R04), then validated: every validator of the frozen validator_registry runs in
// stage order (structural, schema, authority, references, invariants, policy), every diagnostic
// is collected and sorted (ARCH2-R05), and an error rejects the transaction with
// `validation_failed`, writing nothing. The lock_authority is consulted after the validators (the
// locks stage) and again at C2; a denial is `locked`. Then, under the one commit mutex:
//   C1  re-read the head; a moved base is `stale_base` under reject_stale, and is rebased under
//       rebase_if_commutes only when the footprint of every intervening patch is disjoint from
//       this one (rebase_if_disjoint), after which normalisation and validation run again over the
//       new head; an overlap is `conflict`
//   C2  lock_authority again
//   C3  materialise: the patch object (kind 8), the tx record (kind 7), the record and tombstone
//       objects, trie nodes, root and revision (revision_builder, WO-046)
//   C4  backend.commit(capability, batch with the ref CAS {ref, expected base, new revision} and
//       commit_time_us): the commit point
//   C5  publish the new head in the in-memory ref table (readers take snapshots without the mutex)
//   C6  commit observers in registration order, then the events, in seq order
//   C7  return the commit_receipt
// The failpoints FP0, FP1, FP2, FP5 and FP6 (failpoints.hpp) sit before validation, after it,
// after C3, after C4 and after C5. An error after the commit point leaves the transaction
// Indeterminate (`commit_indeterminate`), and every later begin() is `recovery_required` until the
// manager is reopened over its store (ARCH2-R38).
//
// Identity and time (ARCH2-R11): the tx id is derive(tx, actor, "tx/" + hex of the tx record
// object id), so it is a pure function of the hashed tx record and is recovered from history; the
// clock is read once per commit and its value lives only in the commit batch and the receipt. The
// injected id_source serves mint() for authors. Same base, same patch and same injected values
// give the same revision object id whatever the clock says.
//
// Idempotency (ARCH2-R09): a commit whose meta carries an idempotency key is indexed by (actor,
// key) with the digest of its coalesced patch stripped of expectations and base. A later commit
// with the same actor and key returns the original receipt without writing when its digest is the
// same, and fails with `idempotency_mismatch` otherwise. The index is rebuilt from the tx records
// of every ref when a manager opens; a receipt rebuilt so is marked `recovered` (seq and
// commit_time_us are not in hashed objects; WO-055 restores them from the log).
//
// Events (ARCH2-R13, WO-047): "fabric.tx.committed" (subject: the tx id; causality: tx and new
// revision), "fabric.tx.rejected" and "fabric.tx.conflicted" (scope "fabric.tx/<ref>"; causality:
// the base revision) and, when a manager opens over a store that has heads, one
// "fabric.tx.recovered" per ref (subject: the head's tx id). The payload is the SLB/1 tx record
// (for a rejected or conflicted commit, the record it would have written); the correlation is the
// origin's, else the calling thread's. Events and observers run under the commit mutex: a handler
// or observer must not commit through the same manager.
//
// Threading (ARCH2-R12): a transaction is single-threaded; any number of threads may call head(),
// at() and begin() and read snapshots while one commits. The manager must outlive every
// transaction it began.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/authority.hpp"
#include "sl/fabric/clock.hpp"
#include "sl/fabric/deps.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/events.hpp"
#include "sl/fabric/failpoints.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/log_backend.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/provenance.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

/// SL-ARCH-002 `tx_states`.
enum class tx_state : std::uint8_t {
  open = 1,
  validating = 2,
  rejected = 3,
  committing = 4,
  committed = 5,
  conflict = 6,
  failed = 7,
  indeterminate = 8,
  aborted = 9,
};

/// "Open", "Validating", ... as SL-ARCH-002 spells them; "Unknown" otherwise.
[[nodiscard]] std::string_view to_string(tx_state s) noexcept;
/// True for every state but Open and Validating.
[[nodiscard]] bool is_terminal(tx_state s) noexcept;

/// What C1 does with a base that moved (ARCH2-R08).
enum class commit_policy : std::uint8_t { reject_stale = 1, rebase_if_commutes = 2 };

/// Who edits and what they claim about the facts (ADR-0017; SL-ARCH-001 ARCH1-R42, R45).
struct tx_origin {
  actor_id actor;
  /// The subsystem the edit comes through: an owner code ("05") or a prefix name ("content").
  std::string subsystem;
  /// Event correlation; nullopt takes the calling thread's (events.hpp).
  std::optional<correlation_record> correlation;
  /// The F3 provenance of the derived records the transaction writes (tx record field 3).
  std::optional<fabric::provenance> provenance;
  /// The epistemic class of the facts written (tx record origin field 2).
  epistemic_class epistemic = epistemic_class::asserted;
  /// Epistemic class changes the transaction makes, judged by the provenance validator. Not
  /// stored: the tx record has no field for them (recorded for WO-062).
  std::vector<class_change> class_changes;

  friend bool operator==(const tx_origin &, const tx_origin &) = default;
};

struct tx_options {
  tx_origin origin;
  /// The ref the transaction commits to.
  std::string ref = "main";
  /// The revision to stage over; nullopt: the head of `ref` when begin() runs.
  std::optional<digest> base;
  commit_policy policy = commit_policy::reject_stale;
  /// Requested durability. The store decides: memory_backend has none, log_backend syncs as it was
  /// opened (log_options); recorded for WO-055.
  durability sync = durability::fsync;
  /// Label, undo group, coalesce key, idempotency key, undo_of (ARCH2-R03). A staged whole patch
  /// fills the fields left empty here.
  patch_meta meta;
};

/// What an undo needs to find the transaction again (WO-052).
struct undo_token {
  tx_id tx;
  digest tx_record;
  digest revision;

  friend bool operator==(const undo_token &, const undo_token &) noexcept = default;
};

/// C7: what a successful commit returns.
struct commit_receipt {
  std::string ref;
  digest revision;
  digest root;
  tx_id tx;
  /// The store's commit sequence number (0 when rebuilt from history).
  std::uint64_t seq = 0;
  /// The injected clock at C3 (0 when rebuilt from history).
  std::int64_t commit_time_us = 0;
  /// The changed records grouped by id class; the dependency closure is added by observers
  /// (WO-056 attaches it through the receipt_builder).
  impact_summary impact;
  undo_token undo;
  /// What commit observers attached, by name.
  std::map<std::string, value, std::less<>> attachments;
  /// True when the receipt was rebuilt from history after a reopen.
  bool recovered = false;

  friend bool operator==(const commit_receipt &, const commit_receipt &) = default;
};

/// Why a commit is in state Conflict (ARCH2-R08, R46): the code (`stale_base`, `conflict` or
/// `locked`), the bases, this patch's footprint, the overlapping entries and the revisions
/// committed in between (newest first).
struct conflict_report {
  errc code = errc::conflict;
  std::string ref;
  std::optional<digest> base;
  std::optional<digest> head;
  footprint ours;
  std::vector<footprint_overlap> overlaps;
  std::vector<digest> intervening;
  std::string message;
};

class transaction_manager;

/// One transaction (ARCH2-R06, R12): move-only and single-threaded. Destroying an open transaction
/// aborts it.
class transaction {
public:
  transaction(transaction &&other) noexcept;
  transaction &operator=(transaction &&other) noexcept;
  transaction(const transaction &) = delete;
  transaction &operator=(const transaction &) = delete;
  ~transaction();

  /// Applies `op` to the view. `tx_closed` unless Open; the patch-layer errors of apply (a filled
  /// expectation that does not hold is `conflict` and sets conflict()); on error nothing is staged.
  [[nodiscard]] result<void> stage(const patch_op &op);
  /// Stages every op of `p`, all or nothing. `stale_base` when p names a base other than this
  /// transaction's; meta fields left empty in tx_options are taken from `p`.
  [[nodiscard]] result<void> stage(const patch &p);
  /// The staged state over the base: reads see the staged ops first (read-your-writes).
  [[nodiscard]] const staged_state &view() const noexcept;
  /// The ops staged so far, in order, as the author gave them.
  [[nodiscard]] const patch &staged() const noexcept;
  /// The revision the transaction stages over (nullopt for an empty ref).
  [[nodiscard]] const std::optional<digest> &base() const noexcept;

  /// The dry run (ARCH2-R15): everything commit() decides before C3, written nowhere. Returns the
  /// sorted diagnostics (errors included) when commit() would reach C3 or would reject; the error
  /// commit() would return otherwise (`stale_base`, `conflict`, `locked`, a read error).
  /// `tx_closed` or `tx_aborted` unless Open; the transaction stays Open.
  [[nodiscard]] result<std::vector<diagnostic>> validate();
  /// The commit protocol (file comment). Errors: `tx_closed` / `tx_aborted` unless Open,
  /// `recovery_required`, `idempotency_mismatch`, `validation_failed` (see diagnostics()),
  /// `stale_base` / `conflict` / `locked` (see conflict()), an armed failpoint's error or a store
  /// error (Failed), `commit_indeterminate` after the commit point.
  [[nodiscard]] result<commit_receipt> commit();
  /// Ends an open transaction without writing anything (ARCH2-R10); harmless otherwise.
  void abort() noexcept;

  [[nodiscard]] tx_state state() const noexcept;
  /// The diagnostics of the last validate() or commit(), sorted (ARCH2-R05).
  [[nodiscard]] std::span<const diagnostic> diagnostics() const noexcept;
  /// The report of the last conflict, if any.
  [[nodiscard]] const std::optional<conflict_report> &conflict() const noexcept;
  [[nodiscard]] const tx_options &options() const noexcept;

private:
  friend class transaction_manager;
  struct impl;
  explicit transaction(std::unique_ptr<impl> p) noexcept;

  std::unique_ptr<impl> impl_;
};

/// Everything a manager is built from; the registries must be frozen and, with the store, outlive
/// the manager.
struct manager_config {
  store_backend *store = nullptr;
  const schema_registry *schemas = nullptr;
  const domain_registry *domains = nullptr;
  const actor_directory *actors = nullptr;
  std::shared_ptr<const fabric::clock> clock;
  std::shared_ptr<id_source> ids;
  /// Validators appended after the built-in ones (custom stages such as policy).
  std::vector<std::shared_ptr<const validator>> validators;
  /// Names of built-in validators to leave out (tests over schema-less generated records).
  std::vector<std::string> omit_builtin;
  /// Merge drivers by type (ARCH2-R47).
  std::vector<std::pair<type_id, std::shared_ptr<const merge_driver>>> merge_drivers;
  /// Subscriptions made before the manager opens, so they see its recovered events.
  std::vector<std::pair<std::string, event_handler>> subscribers;
  /// The event source id; null: derive(entity, well_known(namespace, 1), "fabric.tx_manager").
  record_id event_source;
};

/// What the built-in validators need.
struct builtin_validator_env {
  const domain_registry *domains = nullptr;
  const actor_directory *actors = nullptr;
  const provenance_source *provenance = nullptr;
};

/// The built-in validator chain (src/tx_builtin_validators.cpp), in registration order; each
/// can be left out by name (manager_config::omit_builtin, workspace_options::omit_builtin):
///   fabric.structural        (structural) R-STR-TARGET a mutating op with a null target or a
///                            target of class tx, event, element or operation; R-STR-TYPE a
///                            create or retype with a null type (the manager itself reports
///                            R-STR-APPLY, a patch that no longer applies)
///   fabric.schema            (schema) R-SCH-UNKNOWN an unregistered type, R-SCH-RECORD a type
///                            that is not a record type, then every R-TY-* finding of
///                            schema_registry::check
///   fabric.schema.newer      (schema) R-SCH-NEWER a write to a record written by a newer
///                            version of its type (migration.hpp, ARCH1-R23)
///   fabric.schema.deprecated (schema) R-SCH-DEPRECATED, a warning for a write of a deprecated
///                            field (migration.hpp, ARCH1-R24)
///   fabric.authority         (authority) WO-048
///   fabric.references        (references) R-REF-DANGLING a strong or owned ref of a written
///                            record to a record that is not live after the patch; R-REF-BLOCKED
///                            a deleted record still referenced strongly by a live record;
///                            R-REF-CASCADE a deleted owner whose owned children stay live
///   fabric.invariants        (invariants) R-INV-OWNED-CYCLE an owned cycle through a written
///                            record; R-INV-OWNED-TWICE one child owned by two written records
///   fabric.provenance        (invariants) WO-049, over `env.provenance`
///   fabric.identity          (invariants) R-INV-ID-CLASS a record id whose class is not its
///                            type's record class; R-INV-ID-REUSE a write over a tombstoned id
///                            that is not the undo of the retiring transaction (identity.hpp,
///                            ARCH1-R05, ARCH2-R27)
///   fabric.boundary          (invariants) R-BND-OBSERVED a runtime observation (patch label
///                            boundary.hpp observation_label) whose origin's epistemic class is
///                            not observed (ARCH1-R60), over `env.provenance`
/// Every message starts with the error code name ("ref_dangling: ...").
[[nodiscard]] result<std::vector<std::shared_ptr<const validator>>>
builtin_validators(const builtin_validator_env &env);

class transaction_manager {
  struct open_key {
    explicit open_key() = default;
  };
  struct state;

public:
  /// `invalid_argument` for a missing store, registry, clock or id source or an unfrozen domain
  /// registry; `schema_invalid` for an unfrozen schema registry; `already_exists` for a repeated
  /// validator or merge driver; the store's read errors. Reads every ref, rebuilds
  /// the idempotency index from history and emits one recovered event per ref.
  [[nodiscard]] static result<std::unique_ptr<transaction_manager>> create(manager_config config);

  transaction_manager(open_key key, std::unique_ptr<state> s) noexcept;

  transaction_manager(const transaction_manager &) = delete;
  transaction_manager(transaction_manager &&) = delete;
  transaction_manager &operator=(const transaction_manager &) = delete;
  transaction_manager &operator=(transaction_manager &&) = delete;
  ~transaction_manager();

  /// A new Open transaction over options.base, else the head of options.ref. `recovery_required`
  /// after an indeterminate commit; `invalid_argument` for a null actor, a subsystem that is no
  /// owner code or prefix name, or an invalid ref name; the store's errors for an unreadable base.
  [[nodiscard]] result<transaction> begin(tx_options options);
  /// The published head of `ref`: an immutable snapshot, read without the commit mutex. A ref
  /// that does not exist yet reads as the empty snapshot (no revision).
  [[nodiscard]] result<snapshot> head(std::string_view ref = "main") const;
  /// The revision `revision`, read-only.
  [[nodiscard]] result<snapshot> at(const digest &revision) const;
  /// Every published head, sorted by name.
  [[nodiscard]] std::vector<ref_entry> refs() const;

  /// The frozen validator registry (ARCH2-R18).
  [[nodiscard]] validator_registry &validators() noexcept;
  [[nodiscard]] event_bus &events() noexcept;
  /// Replaces the lock authority (null restores null_lock_authority).
  void set_lock_authority(std::shared_ptr<lock_authority> locks);
  /// Adds an observer notified at C6 in registration order (a null one is ignored).
  void add_observer(std::shared_ptr<commit_observer> observer);
  /// The frozen merge driver registry.
  [[nodiscard]] merge_driver_registry &merge_drivers() noexcept;
  [[nodiscard]] failpoint_set &failpoints() noexcept;

  /// Creates ref `name` at revision `target` as a recorded transaction (ARCH2-R10): a revision
  /// whose parent is `target`, with the same root and a tx record of an annotate-only patch.
  /// `already_exists` for an existing ref; `invalid_argument` for a bad name or origin.
  [[nodiscard]] result<commit_receipt> create_ref(std::string_view name, const digest &target,
                                                  const tx_origin &origin);
  /// Moves ref `name` to the state of revision `target` as a recorded transaction: a revision with
  /// parents {target, old head}, the root of `target` and an annotate-only patch, so the old head
  /// stays in history. `not_found` for a ref that does not exist.
  [[nodiscard]] result<commit_receipt> reset_ref(std::string_view name, const digest &target,
                                                 const tx_origin &origin);

  /// A random id of class `c` from the injected id_source (serialised).
  [[nodiscard]] result<record_id> mint(id_class c);
  /// True after an indeterminate commit (ARCH2-R38).
  [[nodiscard]] bool recovery_required() const noexcept;
  /// Commits published by this manager.
  [[nodiscard]] std::uint64_t commits() const noexcept;
  [[nodiscard]] const store_backend &store() const noexcept;
  [[nodiscard]] const schema_registry &schemas() const noexcept;

private:
  friend class transaction;

  // The commit protocol for one transaction (C1-C7).
  [[nodiscard]] result<commit_receipt> commit_transaction(transaction::impl &t);
  // Step C4: the only construction of a mutation_capability outside the test backdoor.
  [[nodiscard]] result<commit_ack> write_batch(commit_batch batch) noexcept;

  std::unique_ptr<state> state_;
};

} // namespace sl::fabric
