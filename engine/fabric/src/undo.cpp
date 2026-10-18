// The undo manager: undo and redo as new transactions over per-actor stacks that are a pure
// function of history (WO-052, SL-ARCH-002 ARCH2-R20 to ARCH2-R27; ADR-0018). The stack step,
// grouping, barriers, compensations and conflicts are described in sl/fabric/undo.hpp.

#include <algorithm>
#include <cstddef>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/core/log.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/gen/fabric.history.gen.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/undo.hpp"
#include "sl/fabric/validate.hpp"

namespace sl::fabric {

struct undo_manager::state {
  transaction_manager *manager = nullptr;
  actor_id actor;
  std::string ref;

  mutable std::mutex mutex;
  std::vector<undo_entry> undo;
  std::vector<undo_entry> redo;
  // The undo_of of the last undo or redo, and whether it was an undo: a later commit with the
  // same undo_of is a compensation of it.
  std::optional<std::vector<tx_id>> last_action;
  bool last_action_undo = false;
  // True while the top undo entry is an edit that a commit of the same group may still join.
  bool extendable = false;
  // The head the stacks were built at; nullopt for the empty ref.
  std::optional<digest> synced;
  bool built = false;
  std::optional<error> failure;
  std::optional<conflict_report> conflict;
  // The open group.
  record_id group;
  std::string group_label;
  std::size_t group_depth = 0;
};

namespace {

namespace history = gen::fabric_history;

constexpr std::string_view log_system = "fabric.undo";
constexpr std::string_view compensate_key_prefix = "sl.undo.compensate/";

// One commit as the stack step sees it.
struct commit_info {
  tx_id tx;
  record_id actor;
  digest revision;
  patch p; // normalised, with its meta
  std::string subsystem;
};

// One revision of history with its tx record and patch.
struct revision_view {
  history::revision revision;
  history::tx_record tx;
  digest tx_object;
};

std::string owner_subsystem(std::string_view s) {
  const auto sub = subsystem_from_string(s);
  return sub ? std::string(owner_code(*sub)) : std::string(s);
}

// transaction.hpp: the tx id is derive(tx, actor, "tx/" + hex of the tx record object id).
tx_id tx_id_of(const record_id &actor, const digest &tx_record) {
  return tx_id::from(derive(id_class::tx, actor, "tx/" + tx_record.to_hex())).value_or(tx_id{});
}

result<revision_view> read_revision(const store_backend &store, const digest &rev) {
  auto ro = store.get(rev);
  if (!ro) {
    return std::unexpected(ro.error());
  }
  auto r = decode_revision(*ro);
  if (!r) {
    return std::unexpected(r.error());
  }
  auto txo = store.get(r->tx);
  if (!txo) {
    return std::unexpected(txo.error());
  }
  auto tx = decode_tx_record(*txo);
  if (!tx) {
    return std::unexpected(tx.error());
  }
  return revision_view{.revision = std::move(*r), .tx = std::move(*tx), .tx_object = txo->id};
}

result<patch> read_patch(const store_backend &store, const digest &id) {
  auto po = store.get(id);
  if (!po) {
    return std::unexpected(po.error());
  }
  if (po->header.kind != object_kind::patch) {
    return make_error(errc::object_corrupt, "object " + id.to_hex() + " is not a patch object");
  }
  auto v = decode(po->payload);
  if (!v) {
    return std::unexpected(v.error());
  }
  return from_value(*v);
}

result<patch> patch_of_revision(const store_backend &store, const digest &rev) {
  auto view = read_revision(store, rev);
  if (!view) {
    return std::unexpected(view.error());
  }
  return read_patch(store, view->tx.patch);
}

// Annotations without a compensable or irreversible mark change nothing the stacks track.
bool significant(const patch &p) noexcept {
  return std::ranges::any_of(p.ops, [](const patch_op &op) {
    return op.kind != op_kind::annotate || op.rev != reversibility::reversible;
  });
}

bool is_compensation_tx(const patch &p) noexcept {
  return p.meta.idempotency_key.starts_with(compensate_key_prefix);
}

undo_barrier_info barrier_at(const commit_info &c, std::size_t index) {
  const patch_op &op = c.p.ops[index];
  return undo_barrier_info{
      .tx = c.tx, .op_index = index, .kind = op.kind, .target = op.target, .rev = op.rev};
}

// Adds commit `c` to entry `e`: its tx, revision, subsystem and reversibility marks.
void extend(undo_entry &e, const commit_info &c) {
  e.txs.push_back(c.tx);
  e.revisions.push_back(c.revision);
  e.subsystem = c.subsystem;
  for (std::size_t i = 0; i < c.p.ops.size(); ++i) {
    const patch_op &op = c.p.ops[i];
    if (op.rev == reversibility::irreversible) {
      if (!e.barrier || e.barrier->rev != reversibility::irreversible) {
        e.barrier = barrier_at(c, i);
      }
    } else if (op.rev == reversibility::compensable) {
      if (op.kind == op_kind::annotate) {
        ++e.compensations;
        if (e.barrier && e.barrier->rev == reversibility::compensable) {
          e.barrier.reset();
        }
      } else {
        ++e.compensable_mutations;
        if (!e.barrier && e.compensations == 0) {
          e.barrier = barrier_at(c, i);
        }
      }
    }
  }
}

undo_entry entry_of(const commit_info &c, std::string label, const record_id &group) {
  undo_entry e;
  e.label = std::move(label);
  e.group = group;
  extend(e, c);
  return e;
}

// An irreversible top entry stops the stack: nothing below it can be reached.
void trim(std::vector<undo_entry> &stack) {
  if (stack.size() < 2) {
    return;
  }
  const std::optional<undo_barrier_info> &barrier = stack.back().barrier;
  if (barrier && barrier->rev == reversibility::irreversible) {
    stack.erase(stack.begin(), stack.end() - 1);
  }
}

// The stack step (undo.hpp file comment); the caller holds the mutex.
void step(undo_manager::state &s, const commit_info &c) {
  if (c.actor != s.actor.id() || !significant(c.p)) {
    return;
  }
  const std::vector<tx_id> &undo_of = c.p.meta.undo_of;
  const bool compensation = is_compensation_tx(c.p);
  if (!undo_of.empty()) {
    if (s.last_action && *s.last_action == undo_of) {
      if (compensation && s.last_action_undo) {
        s.redo.clear();
      }
      return;
    }
    if (!s.undo.empty() && s.undo.back().txs == undo_of) {
      undo_entry undone = std::move(s.undo.back());
      s.undo.pop_back();
      s.last_action = undo_of;
      s.last_action_undo = true;
      s.extendable = false;
      if (compensation) {
        s.redo.clear();
        return;
      }
      s.redo.push_back(entry_of(c, std::move(undone.label), record_id{}));
      return;
    }
    if (!s.redo.empty() && s.redo.back().txs == undo_of) {
      undo_entry redone = std::move(s.redo.back());
      s.redo.pop_back();
      s.last_action = undo_of;
      s.last_action_undo = false;
      s.extendable = false;
      s.undo.push_back(entry_of(c, std::move(redone.label), record_id{}));
      trim(s.undo);
      return;
    }
  }
  // An edit (or an undo whose entry is not on the stacks, which is then an edit like any other).
  s.last_action.reset();
  s.redo.clear();
  const record_id &group = c.p.meta.undo_group;
  if (s.extendable && !group.is_null() && !s.undo.empty() && s.undo.back().group == group) {
    extend(s.undo.back(), c);
  } else {
    s.undo.push_back(entry_of(c, c.p.meta.label, group));
  }
  s.extendable = true;
  trim(s.undo);
}

void reset_stacks(undo_manager::state &s) {
  s.undo.clear();
  s.redo.clear();
  s.last_action.reset();
  s.last_action_undo = false;
  s.extendable = false;
  s.conflict.reset();
}

// Rebuilds the stacks from the first-parent history ending at `head`; the caller holds the mutex.
result<void> rebuild(undo_manager::state &s, const snapshot &head) {
  reset_stacks(s);
  s.built = false;
  const store_backend &store = head.store();
  std::vector<std::pair<digest, revision_view>> chain;
  std::optional<digest> cur = head.revision_digest();
  while (cur) {
    auto view = read_revision(store, *cur);
    if (!view) {
      return std::unexpected(view.error());
    }
    std::optional<digest> parent;
    if (!view->revision.parents.empty()) {
      parent = view->revision.parents.front();
    }
    chain.emplace_back(*cur, std::move(*view));
    cur = parent;
  }
  for (const auto &[rev, v] : std::views::reverse(chain)) {
    if (v.tx.actor != s.actor.id()) {
      continue;
    }
    auto p = read_patch(store, v.tx.patch);
    if (!p) {
      return std::unexpected(p.error());
    }
    step(s, commit_info{.tx = tx_id_of(v.tx.actor, v.tx_object),
                        .actor = v.tx.actor,
                        .revision = rev,
                        .p = std::move(*p),
                        .subsystem = owner_subsystem(v.tx.origin.subsystem)});
  }
  s.synced = head.revision_digest();
  s.built = true;
  s.failure.reset();
  return {};
}

// Rebuilds, or empties the stacks and keeps the error for later calls.
result<void> rebuild_or_fail(undo_manager::state &s, const snapshot &head) {
  if (auto ok = rebuild(s, head); !ok) {
    reset_stacks(s);
    s.failure = ok.error();
    return ok;
  }
  return {};
}

// C6: applies every commit published on the ref to the stacks.
class undo_observer final : public commit_observer {
public:
  explicit undo_observer(std::weak_ptr<undo_manager::state> s) noexcept : state_(std::move(s)) {}

  [[nodiscard]] result<void> on_commit(const commit_event &ev,
                                       receipt_builder & /*receipt*/) override {
    const std::shared_ptr<undo_manager::state> s = state_.lock();
    if (!s) {
      return {};
    }
    auto head = s->manager->head(s->ref);
    if (!head) {
      return std::unexpected(head.error());
    }
    if (head->revision_digest() != ev.revision) {
      return {}; // a commit on another ref
    }
    auto view = read_revision(head->store(), ev.revision);
    if (!view) {
      return std::unexpected(view.error());
    }
    std::optional<digest> parent;
    if (!view->revision.parents.empty()) {
      parent = view->revision.parents.front();
    }
    const std::scoped_lock lock(s->mutex);
    if (!s->built || s->failure || parent != s->synced) {
      return rebuild_or_fail(*s, *head);
    }
    step(*s, commit_info{.tx = ev.tx,
                         .actor = ev.origin.actor.id(),
                         .revision = ev.revision,
                         .p = ev.patch,
                         .subsystem = owner_subsystem(ev.origin.subsystem)});
    s->synced = ev.revision;
    return {};
  }

private:
  std::weak_ptr<undo_manager::state> state_;
};

std::string describe(const undo_barrier_info &b) {
  if (b.rev == reversibility::irreversible) {
    return std::format("op {} ({}) of tx {} on {} is irreversible; nothing can be undone past it",
                       b.op_index, to_string(b.kind), b.tx.to_string(), b.target.to_string());
  }
  return std::format("op {} ({}) of tx {} on {} is compensable but its entry declares no "
                     "compensation",
                     b.op_index, to_string(b.kind), b.tx.to_string(), b.target.to_string());
}

// Errors of staging or committing an inverse that mean the state moved under it.
bool conflict_code(errc code) noexcept {
  return code == errc::conflict || code == errc::not_found || code == errc::already_exists ||
         code == errc::type_mismatch;
}

// The undo_conflict report: the inverse's footprint against every revision since `since`.
conflict_report make_conflict(const transaction_manager &m, const std::string &ref,
                              const patch &inverse, const std::optional<digest> &base,
                              const digest &since, std::string message) {
  conflict_report r;
  r.code = errc::undo_conflict;
  r.ref = ref;
  r.base = base;
  r.ours = footprint_of(inverse);
  r.message = std::move(message);
  auto head = m.head(ref);
  if (!head) {
    return r;
  }
  r.head = head->revision_digest();
  std::optional<digest> cur = r.head;
  while (cur && *cur != since) {
    auto view = read_revision(m.store(), *cur);
    if (!view) {
      break;
    }
    auto p = read_patch(m.store(), view->tx.patch);
    if (!p) {
      break;
    }
    r.intervening.push_back(*cur);
    auto o = r.ours.overlaps(footprint_of(*p));
    r.overlaps.insert(r.overlaps.end(), o.begin(), o.end());
    cur = view->revision.parents.empty() ? std::nullopt
                                         : std::optional<digest>(view->revision.parents.front());
  }
  return r;
}

struct pending_compensation {
  tx_id tx;
  std::size_t op_index = 0;
  compensation what;
  const compensator *by = nullptr;
};

} // namespace

undo_manager::undo_manager(transaction_manager &manager, actor_id actor, undo_options options)
    : manager_(&manager), actor_(actor), options_(std::move(options)),
      intents_(options_.intents ? options_.intents : std::make_shared<memory_intent_log>()),
      state_(std::make_shared<state>()) {
  state_->manager = manager_;
  state_->actor = actor_;
  state_->ref = options_.ref;
  manager.add_observer(std::make_shared<undo_observer>(state_));
  auto head = manager.head(options_.ref);
  const std::scoped_lock lock(state_->mutex);
  if (state_->built) {
    return; // a commit in the meantime made the observer build the stacks
  }
  if (!head) {
    state_->failure = head.error();
    return;
  }
  if (auto ok = rebuild_or_fail(*state_, *head); !ok) {
    sl::log::emit(sl::log::level::warn, log_system, "undo stacks not built",
                  {{"actor", actor_.to_string()}, {"error", ok.error().message}});
  }
}

undo_manager::~undo_manager() = default;

result<record_id> undo_manager::begin_group(std::string label) {
  {
    const std::scoped_lock lock(state_->mutex);
    if (state_->failure) {
      return std::unexpected(*state_->failure);
    }
    if (state_->group_depth > 0) {
      ++state_->group_depth;
      return state_->group;
    }
  }
  auto id = manager_->mint(id_class::operation);
  if (!id) {
    return std::unexpected(id.error());
  }
  const std::scoped_lock lock(state_->mutex);
  state_->group = *id;
  state_->group_label = std::move(label);
  state_->group_depth = 1;
  return *id;
}

result<void> undo_manager::end_group() {
  const std::scoped_lock lock(state_->mutex);
  if (state_->group_depth == 0) {
    return make_error(errc::invalid_argument, "no undo group is open");
  }
  if (--state_->group_depth == 0) {
    state_->group = record_id{};
    state_->group_label.clear();
  }
  return {};
}

patch_meta undo_manager::group_meta() const {
  const std::scoped_lock lock(state_->mutex);
  patch_meta m;
  if (state_->group_depth > 0) {
    m.label = state_->group_label;
    m.undo_group = state_->group;
  }
  return m;
}

bool undo_manager::can_undo() const noexcept {
  const std::scoped_lock lock(state_->mutex);
  return !state_->failure && !state_->undo.empty() && !state_->undo.back().barrier;
}

bool undo_manager::can_redo() const noexcept {
  const std::scoped_lock lock(state_->mutex);
  return !state_->failure && !state_->redo.empty();
}

std::optional<std::string> undo_manager::undo_label() const {
  const std::scoped_lock lock(state_->mutex);
  if (state_->failure || state_->undo.empty() || state_->undo.back().barrier) {
    return std::nullopt;
  }
  return state_->undo.back().label;
}

result<void> undo_manager::rebuild_from_history(const snapshot &head) {
  const std::scoped_lock lock(state_->mutex);
  return rebuild_or_fail(*state_, head);
}

std::vector<undo_entry> undo_manager::undo_stack() const {
  const std::scoped_lock lock(state_->mutex);
  return state_->undo;
}

std::vector<undo_entry> undo_manager::redo_stack() const {
  const std::scoped_lock lock(state_->mutex);
  return state_->redo;
}

std::optional<conflict_report> undo_manager::last_conflict() const {
  const std::scoped_lock lock(state_->mutex);
  return state_->conflict;
}

result<commit_receipt> undo_manager::undo() { return revert(false); }
result<commit_receipt> undo_manager::redo() { return revert(true); }

result<commit_receipt> undo_manager::revert(bool redo) {
  undo_entry e;
  {
    const std::scoped_lock lock(state_->mutex);
    if (state_->failure) {
      return std::unexpected(*state_->failure);
    }
    const std::vector<undo_entry> &stack = redo ? state_->redo : state_->undo;
    if (stack.empty()) {
      return make_error(errc::undo_empty, redo ? "nothing to redo" : "nothing to undo");
    }
    e = stack.back();
    if (e.barrier) {
      return make_error(errc::undo_barrier, describe(*e.barrier));
    }
    state_->conflict.reset();
  }
  const store_backend &store = manager_->store();

  // The inverse of the reversible ops, newest tx first, and the compensations, newest first.
  std::optional<patch> inverse;
  std::vector<pending_compensation> compensations;
  for (std::size_t i = e.revisions.size(); i-- > 0;) {
    auto p = patch_of_revision(store, e.revisions[i]);
    if (!p) {
      return std::unexpected(p.error());
    }
    for (std::size_t k = p->ops.size(); k-- > 0;) {
      auto c = compensation_of(p->ops[k]);
      if (!c) {
        return std::unexpected(c.error());
      }
      if (*c) {
        const compensator *by =
            options_.compensators ? options_.compensators->find((*c)->command) : nullptr;
        if (by == nullptr) {
          return make_error(errc::not_found, "no compensator for '" + (*c)->command + "' (op " +
                                                 std::to_string(k) + " of tx " +
                                                 e.txs[i].to_string() + ")");
        }
        compensations.push_back(
            pending_compensation{.tx = e.txs[i], .op_index = k, .what = std::move(**c), .by = by});
      }
    }
    std::erase_if(p->ops, [](const patch_op &op) {
      return op.kind == op_kind::annotate || op.rev != reversibility::reversible;
    });
    if (p->ops.empty()) {
      continue;
    }
    patch inv = invert(*p);
    if (!inverse) {
      inverse = std::move(inv);
      continue;
    }
    auto composed = compose(*inverse, inv);
    if (!composed) {
      return std::unexpected(composed.error());
    }
    inverse = std::move(*composed);
  }
  if (!inverse && compensations.empty()) {
    return make_error(errc::undo_empty, "the entry '" + e.label + "' has nothing to revert");
  }

  tx_options o;
  o.origin.actor = actor_;
  o.origin.subsystem = e.subsystem;
  o.ref = options_.ref;
  o.meta.label = (redo ? "redo " : "undo ") + e.label;
  o.meta.undo_of = e.txs;

  std::optional<commit_receipt> last;
  if (inverse) {
    inverse->base.reset();
    inverse->meta = patch_meta{};
    auto t = manager_->begin(o);
    if (!t) {
      return std::unexpected(t.error());
    }
    const std::optional<digest> base = t->base();
    const auto refuse = [&](const error &err) -> std::unexpected<error> {
      conflict_report r =
          make_conflict(*manager_, options_.ref, *inverse, base, e.revisions.back(), err.message);
      std::string message =
          std::format("{} of '{}' conflicts with {} later revision(s): {}", redo ? "redo" : "undo",
                      e.label, r.intervening.size(), err.message);
      {
        const std::scoped_lock lock(state_->mutex);
        state_->conflict = std::move(r);
      }
      return make_error(errc::undo_conflict, std::move(message));
    };
    if (auto ok = t->stage(*inverse); !ok) {
      if (conflict_code(ok.error().code)) {
        return refuse(ok.error());
      }
      return std::unexpected(ok.error());
    }
    auto r = t->commit();
    if (!r) {
      if (r.error().code == errc::conflict) {
        return refuse(r.error());
      }
      return std::unexpected(r.error());
    }
    last = std::move(*r);
  }

  for (const pending_compensation &pc : compensations) {
    const record_id operation =
        derive(id_class::operation, actor_.id(),
               "compensate/" + pc.tx.to_string() + "/" + std::to_string(pc.op_index));
    const auto end = [&](intent_outcome outcome) {
      return intents_->write(intent_record{
          .operation = operation, .kind = {}, .phase = intent_phase::end, .outcome = outcome});
    };
    if (auto ok = intents_->write(intent_record{.operation = operation,
                                                .kind = "compensate:" + pc.what.command,
                                                .phase = intent_phase::begin,
                                                .outcome = intent_outcome::none});
        !ok) {
      return std::unexpected(ok.error());
    }
    const auto abandon = [&](const error &err) -> std::unexpected<error> {
      if (auto ok = end(intent_outcome::abandoned); !ok) {
        sl::log::emit(sl::log::level::warn, log_system, "intent_end not written",
                      {{"operation", operation.to_string()}, {"error", ok.error().message}});
      }
      return std::unexpected(err);
    };
    auto head = manager_->head(options_.ref);
    if (!head) {
      return abandon(head.error());
    }
    auto p = pc.by->compensate(pc.what, *head);
    if (!p) {
      return abandon(p.error());
    }
    p->base.reset();
    p->meta = patch_meta{};
    tx_options co = o;
    co.meta.label = "compensate " + pc.what.command + " (" + o.meta.label + ")";
    co.meta.idempotency_key = std::string(compensate_key_prefix) + operation.to_string();
    auto t = manager_->begin(std::move(co));
    if (!t) {
      return abandon(t.error());
    }
    if (auto ok = t->stage(*p); !ok) {
      return abandon(ok.error());
    }
    auto r = t->commit();
    if (!r) {
      return abandon(r.error());
    }
    if (auto ok = end(intent_outcome::compensated); !ok) {
      return std::unexpected(ok.error());
    }
    last = std::move(*r);
  }
  if (!last) {
    return make_error(errc::internal, "undo committed nothing");
  }
  return std::move(*last);
}

} // namespace sl::fabric
