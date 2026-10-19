#pragma once

// Undo and redo as new transactions (WO-052, SL-ARCH-002 ARCH2-R20 to ARCH2-R27; ADR-0018).
//
// History is append-only (ARCH2-R10), so an undo never rewinds a head: it is an ordinary
// transaction through the transaction_manager whose patch is the composition of the inverses of
// the undone transactions' stored patches, newest first, with meta.undo_of naming every undone tx
// (the tx record keeps only the first, the patch object keeps the full list; WO-050).
//
// Stacks (ARCH2-R21). Each undo_manager keeps the undo and redo stacks of one actor on one ref,
// and they are a pure function of the ref's first-parent history: the manager registers a commit
// observer that applies every commit published on the ref to the stacks (C6), and
// rebuild_from_history() replays the same step over the tx records and patch objects from the
// root to a head, so a manager made after a reopen holds identical stacks. The step, for a commit
// of this actor that changes state (annotate-only patches, such as create_ref and reset_ref,
// change nothing relative to their first parent and are skipped; commits of other actors only
// matter as intervening edits):
//   undo_of equals the txs of the top undo entry       an undo: the entry moves to the redo stack
//                                                      as {this tx}
//   undo_of equals the txs of the top redo entry       a redo: the entry moves back to the undo
//                                                      stack as {this tx}
//   undo_of equals the previous undo's undo_of         a compensation of that undo (below)
//   otherwise                                          an edit: the redo stack is cleared
//                                                      (ARCH2-R25) and the commit joins the top
//                                                      entry when both carry the same non-null
//                                                      undo group and no undo or redo came between,
//                                                      else it opens a new entry
// A commit on the ref whose first parent is not the head the stacks were built at (a reset)
// rebuilds them from the new head.
//
// Groups (ARCH2-R22). begin_group() mints an undo group id; every transaction the caller begins
// with group_meta() in its tx_options joins the group, and undo() reverts the whole group as one
// transaction or not at all (an armed failpoint or a store fault leaves head and stacks as they
// were).
//
// Reversibility (ARCH2-R24). Every op carries a reversibility mark (patch.hpp):
//   reversible     inverted mechanically (invert(), WO-043)
//   irreversible   a barrier: undo of its entry is refused with `undo_barrier` naming the op,
//                  and every entry below it is dropped (nothing can be undone past it)
//   compensable    an annotate op made by op_compensation() declares a compensation {command,
//                  args}; a mutating op marked compensable is not inverted mechanically but is
//                  covered by the compensations of its entry (an entry with a compensable
//                  mutation and no compensation is a barrier)
// undo() first commits the inverse of the reversible ops (when there are any), then runs every
// compensation of the entry, newest first, each as its own transaction bracketed by an
// intent_begin and an intent_end record (kind "compensate:<command>", operation id
// derive(operation, actor, "compensate/<tx>/<op index>")) written to the intent_log, with outcome
// `compensated`, or `abandoned` when the compensator or its commit fails. A compensation
// transaction carries the same undo_of as its undo and the idempotency key
// "sl.undo.compensate/<operation id>", which is how history tells it from an inverse. An undo
// that ran a compensation cannot be redone: it clears the redo stack.
//
// Conflicts (ARCH2-R23). The inverse carries the expectations of the undone patches inverted, so
// an intervening edit to a value it expects makes staging (or normalisation at commit) fail; the
// undo is then refused with `undo_conflict`, nothing is written and last_conflict() reports the
// inverse's footprint, the overlapping footprint entries of the revisions committed since the
// entry and those revisions (newest first). Selective undo by transform is Volume 18 (seam S4).
//
// Coalescing (ARCH2-R26) happens inside one open transaction: the manager composes the staged
// ops at commit, so repeated sets of one path keep the first expectation and the last value and a
// drag commits once, as one revision and one undo entry.
//
// Delete then undo (ARCH2-R27): the inverse of delete_record re-creates the captured body under
// the same id with undo_of set, which is the only revival the invariants stage accepts
// (R-INV-ID-REUSE, fabric.identity), and the record object id is the same.
//
// Intents: transaction_manager has no intent API and only it may commit to a store (F5), so the
// intent records go to an intent_log; memory_intent_log is the default. Writing them as journal
// frames 3/4 is WO-053/WO-055 work (recorded for WO-062).
//
// Threading: an undo_manager is used by one thread; its observer may run on any committing thread,
// and the stacks are guarded by a mutex. undo(), redo() and the compensations commit through the
// manager and hold no lock meanwhile. The transaction_manager must outlive the undo_manager; the
// observer it registers stays registered (the manager has no removal) but does nothing once the
// undo_manager is gone.

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

/// What a compensator is asked to do (ADR-0018): a registered command and its arguments.
struct compensation {
  std::string command;
  value args;

  friend bool operator==(const compensation &, const compensation &) noexcept = default;
};

/// An annotate op marked compensable whose note is struct {1 command string, 2 args}.
/// `invalid_argument` for an empty command; `encode_invalid` for a command SLB/1 cannot hold.
[[nodiscard]] result<patch_op> op_compensation(const compensation &c);
/// The compensation an op declares: nullopt for any op that is not an annotate marked
/// compensable; `type_mismatch` for such an op whose note is not the layout above.
[[nodiscard]] result<std::optional<compensation>> compensation_of(const patch_op &op);

/// Turns a compensation into the patch that compensates it, over the current head.
class compensator {
public:
  virtual ~compensator() = default;
  /// The command this compensator serves.
  [[nodiscard]] virtual std::string_view command() const noexcept = 0;
  /// The patch to commit; an error abandons the compensation.
  [[nodiscard]] virtual result<patch> compensate(const compensation &c,
                                                 const snapshot &head) const = 0;

protected:
  compensator() = default;
  compensator(const compensator &) = default;
  compensator &operator=(const compensator &) = default;
  compensator(compensator &&) = default;
  compensator &operator=(compensator &&) = default;
};

/// Compensators by command.
class compensator_registry {
public:
  /// `invalid_argument` for a null compensator or an empty command; `already_exists` for a
  /// command registered already.
  [[nodiscard]] result<void> add(std::shared_ptr<const compensator> c);
  /// The compensator of `command`, or null.
  [[nodiscard]] const compensator *find(std::string_view command) const noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return by_command_.size(); }

private:
  std::map<std::string, std::shared_ptr<const compensator>, std::less<>> by_command_;
};

/// Where intent_begin and intent_end records go (ARCH2-R24, R36).
class intent_log {
public:
  virtual ~intent_log() = default;
  [[nodiscard]] virtual result<void> write(const intent_record &r) = 0;

protected:
  intent_log() = default;
  intent_log(const intent_log &) = default;
  intent_log &operator=(const intent_log &) = default;
  intent_log(intent_log &&) = default;
  intent_log &operator=(intent_log &&) = default;
};

/// The in-memory intent log: every record in write order. Thread-safe.
class memory_intent_log final : public intent_log {
public:
  [[nodiscard]] result<void> write(const intent_record &r) override;
  /// Every record written, in order.
  [[nodiscard]] std::vector<intent_record> records() const;
  /// Operations with a begin and no end, ascending.
  [[nodiscard]] std::vector<record_id> open() const;

private:
  mutable std::mutex mutex_;
  std::vector<intent_record> records_;
};

/// The op that makes an entry a barrier: the first irreversible op, or the first compensable
/// mutation of an entry that declares no compensation.
struct undo_barrier_info {
  tx_id tx;
  std::size_t op_index = 0;
  op_kind kind = op_kind::annotate;
  record_id target;
  reversibility rev = reversibility::irreversible;

  friend bool operator==(const undo_barrier_info &, const undo_barrier_info &) noexcept = default;
};

/// One stack entry: the transactions undone (or redone) together, in commit order.
struct undo_entry {
  std::vector<tx_id> txs;
  /// The revision each tx produced, in the same order.
  std::vector<digest> revisions;
  /// The label of the first tx.
  std::string label;
  /// The undo group (null for a single edit and for entries made by undo or redo).
  record_id group;
  /// The owner code of the subsystem of the last tx: the undo commits through it.
  std::string subsystem;
  std::optional<undo_barrier_info> barrier;
  /// Compensation declarations and compensable mutations in the entry's txs.
  std::size_t compensations = 0;
  std::size_t compensable_mutations = 0;

  friend bool operator==(const undo_entry &, const undo_entry &) = default;
};

struct undo_options {
  /// The ref whose history the stacks follow and the undo transactions commit to.
  std::string ref = "main";
  /// Null: a memory_intent_log owned by the undo manager.
  std::shared_ptr<intent_log> intents;
  /// Null: no compensator (undo of an entry with a compensation is `not_found`).
  std::shared_ptr<const compensator_registry> compensators;
};

class undo_manager {
public:
  /// Registers the commit observer and builds the stacks from the head of options.ref; a failed
  /// build is returned by every later undo(), redo(), begin_group() and rebuild_from_history()
  /// until a rebuild succeeds.
  undo_manager(transaction_manager &manager, actor_id actor, undo_options options = {});
  undo_manager(const undo_manager &) = delete;
  undo_manager(undo_manager &&) = delete;
  undo_manager &operator=(const undo_manager &) = delete;
  undo_manager &operator=(undo_manager &&) = delete;
  ~undo_manager();

  /// Opens a group (nested calls join the open group): returns its id, minted from the manager.
  [[nodiscard]] result<record_id> begin_group(std::string label);
  /// Closes the innermost begin_group(); `invalid_argument` when none is open.
  [[nodiscard]] result<void> end_group();
  /// The meta a transaction of the open group carries ({label, undo group}); empty otherwise.
  [[nodiscard]] patch_meta group_meta() const;

  /// Reverts the top entry (file comment). `undo_empty`, `undo_barrier` (the message names the
  /// op), `undo_conflict` (see last_conflict()), `not_found` for an unregistered compensator,
  /// the manager's errors otherwise. Returns the receipt of the last transaction it committed.
  [[nodiscard]] result<commit_receipt> undo();
  /// Re-applies the top redo entry: the inverse of its undo, restoring the exact root.
  /// `undo_empty` when there is nothing to redo, `undo_conflict`, the manager's errors.
  [[nodiscard]] result<commit_receipt> redo();
  [[nodiscard]] bool can_undo() const noexcept;
  [[nodiscard]] bool can_redo() const noexcept;
  /// The label of the entry undo() would revert; nullopt when can_undo() is false.
  [[nodiscard]] std::optional<std::string> undo_label() const;
  /// Replaces the stacks with those of the history ending at `head` (ARCH2-R21).
  [[nodiscard]] result<void> rebuild_from_history(const snapshot &head);

  /// The stacks, bottom first.
  [[nodiscard]] std::vector<undo_entry> undo_stack() const;
  [[nodiscard]] std::vector<undo_entry> redo_stack() const;
  /// The report of the last `undo_conflict`, if any.
  [[nodiscard]] std::optional<conflict_report> last_conflict() const;
  [[nodiscard]] intent_log &intents() noexcept { return *intents_; }
  [[nodiscard]] const actor_id &actor() const noexcept { return actor_; }

  struct state;

private:
  [[nodiscard]] result<commit_receipt> revert(bool redo);

  transaction_manager *manager_;
  actor_id actor_;
  undo_options options_;
  std::shared_ptr<intent_log> intents_;
  std::shared_ptr<state> state_;
};

} // namespace sl::fabric
