// The transaction manager and transactions: the sole mutation path (WO-050, SL-ARCH-002 ARCH2-R01
// to ARCH2-R19, ARCH2-R46; ADR-0015, ADR-0016, ADR-0019). The protocol is described in
// sl/fabric/transaction.hpp; this file follows its steps C1-C7 in order.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/core/log.hpp"
#include "sl/fabric/authority.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/deps.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/events.hpp"
#include "sl/fabric/failpoints.hpp"
#include "sl/fabric/gen/fabric.history.gen.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/provenance.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

namespace tx_detail {

namespace history = gen::fabric_history;

constexpr std::string_view log_system = "fabric.tx";
constexpr std::string_view committed_event = "fabric.tx.committed";
constexpr std::string_view rejected_event = "fabric.tx.rejected";
constexpr std::string_view conflicted_event = "fabric.tx.conflicted";
constexpr std::string_view recovered_event = "fabric.tx.recovered";

// ---- small helpers ----------------------------------------------------------------------------

bool valid_ref_name(std::string_view name) noexcept {
  return !name.empty() && name.size() <= max_ref_name && std::ranges::all_of(name, [](char c) {
    const auto b = static_cast<unsigned char>(c);
    return b >= 0x21 && b <= 0x7E;
  });
}

digest sha256_digest(byte_span bytes) noexcept {
  digest d;
  d.bytes = sha256::hash(bytes);
  return d;
}

// The patch object (kind 8) of `p`: its SLB/1 value encoded (patch.hpp to_value).
result<object> encode_patch_object(const patch &p) {
  auto v = to_value(p);
  if (!v) {
    return std::unexpected(v.error());
  }
  auto bytes = encode(*v);
  if (!bytes) {
    return std::unexpected(bytes.error());
  }
  return make_object(object_kind::patch, *bytes);
}

result<patch> decode_patch_object(const object &o) {
  if (o.header.kind != object_kind::patch) {
    return make_error(errc::object_corrupt, "object " + o.id.to_hex() + " is not a patch object");
  }
  auto v = decode(o.payload);
  if (!v) {
    return std::unexpected(v.error());
  }
  return from_value(*v);
}

// ARCH2-R09: what makes two commits "the same patch": the coalesced ops and meta, stripped of the
// base and of every expectation (those depend on the state a retry stages over, and a normalised
// stored patch must compare equal to the author's ops after a reopen).
result<digest> idempotency_digest(const patch &author) {
  patch p = author;
  p.base.reset();
  for (patch_op &op : p.ops) {
    op.expect = expectation{};
    op.expect_order_key.clear();
    op.expect_type = type_id{};
    if (op.kind == op_kind::delete_record || op.kind == op_kind::remove_elem ||
        op.kind == op_kind::retype_record) {
      op.captured.reset();
    }
    if (op.kind == op_kind::delete_record) {
      op.type = type_id{};
    }
  }
  auto v = to_value(p);
  if (!v) {
    return std::unexpected(v.error());
  }
  auto bytes = encode(*v);
  if (!bytes) {
    return std::unexpected(bytes.error());
  }
  return sha256_digest(*bytes);
}

tx_id tx_id_for(const actor_id &actor, const digest &tx_record) {
  // Pure in the hashed tx record (ARCH2-R11); the id class is tx by construction.
  const record_id id = derive(id_class::tx, actor.id(), "tx/" + tx_record.to_hex());
  return tx_id::from(id).value_or(tx_id{});
}

// ---- provenance over the transaction being validated (WO-049 seam) ---------------------------

// The provenance validator is registered once, but provenance belongs to each transaction: the
// manager registers the origin of the transaction under its validation_context for the duration
// of the run, and this source looks it up. Validations may run on several threads (dry runs).
class origin_frames final : public provenance_source {
public:
  class frame {
  public:
    frame(origin_frames &owner, const validation_context &ctx, const tx_origin &origin)
        : owner_(&owner), ctx_(&ctx) {
      const std::scoped_lock lock(owner_->mutex_);
      owner_->frames_.insert_or_assign(ctx_, &origin);
    }
    frame(const frame &) = delete;
    frame(frame &&) = delete;
    frame &operator=(const frame &) = delete;
    frame &operator=(frame &&) = delete;
    ~frame() {
      const std::scoped_lock lock(owner_->mutex_);
      owner_->frames_.erase(ctx_);
    }

  private:
    origin_frames *owner_;
    const validation_context *ctx_;
  };

  [[nodiscard]] result<std::optional<provenance>>
  origin(const validation_context &ctx) const override {
    const tx_origin *o = find(ctx);
    if (o == nullptr) {
      return std::optional<provenance>{};
    }
    return o->provenance;
  }
  // The tx record carries one provenance (the origin); no per-op override is stored (WO-062).
  [[nodiscard]] result<std::optional<provenance>>
  override_for([[maybe_unused]] const validation_context &ctx,
               [[maybe_unused]] std::size_t op_index) const override {
    return std::optional<provenance>{};
  }
  [[nodiscard]] result<std::vector<class_change>>
  class_changes(const validation_context &ctx) const override {
    const tx_origin *o = find(ctx);
    if (o == nullptr) {
      return std::vector<class_change>{};
    }
    return o->class_changes;
  }

private:
  [[nodiscard]] const tx_origin *find(const validation_context &ctx) const {
    const std::scoped_lock lock(mutex_);
    const auto it = frames_.find(&ctx);
    return it == frames_.end() ? nullptr : it->second;
  }

  mutable std::mutex mutex_;
  std::map<const validation_context *, const tx_origin *> frames_;
};

// ---- manager state ----------------------------------------------------------------------------

struct idempotency_entry {
  digest patch;
  commit_receipt receipt;
};

using idempotency_key = std::pair<record_id, std::string>;

record_id default_event_source() noexcept {
  return derive(id_class::entity, well_known(id_class::namespace_, 1), "fabric.tx_manager");
}

struct manager_core {
  explicit manager_core(manager_config &c)
      : store(c.store), schemas(c.schemas), domains(c.domains), actors(c.actors),
        clock(std::move(c.clock)), ids(std::move(c.ids)),
        events(c.event_source.is_null() ? default_event_source() : c.event_source, *clock),
        locks(std::make_shared<null_lock_authority>()) {}

  store_backend *store;
  const schema_registry *schemas;
  const domain_registry *domains;
  const actor_directory *actors;
  std::shared_ptr<const fabric::clock> clock;
  std::shared_ptr<id_source> ids;
  std::mutex ids_mutex;

  origin_frames frames;
  validator_registry validators;
  merge_driver_registry merge_drivers;
  failpoint_set failpoints;

  event_bus bus;
  event_source events;                  // used under commit_mutex only
  std::vector<subscription> subscribed; // after bus: unsubscribed first

  std::mutex commit_mutex; // C1: one writer per workspace (ARCH2-R07)

  mutable std::shared_mutex heads_mutex; // C5: the published ref table (MVCC)
  std::map<std::string, digest, std::less<>> heads;

  std::mutex config_mutex;
  std::shared_ptr<lock_authority> locks;
  std::vector<std::shared_ptr<commit_observer>> observers;

  std::mutex idem_mutex;
  std::map<idempotency_key, idempotency_entry> idem;

  std::atomic<bool> indeterminate{false};
  std::atomic<std::uint64_t> commits{0};

  [[nodiscard]] std::optional<digest> published(std::string_view ref) const {
    const std::shared_lock lock(heads_mutex);
    const auto it = heads.find(ref);
    if (it == heads.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  [[nodiscard]] result<snapshot> snapshot_of(const std::optional<digest> &revision) const {
    return revision ? snapshot::open(*store, *revision) : snapshot::empty(*store);
  }

  [[nodiscard]] std::shared_ptr<lock_authority> lock_authority_now() {
    const std::scoped_lock lock(config_mutex);
    return locks;
  }

  [[nodiscard]] std::vector<std::shared_ptr<commit_observer>> observers_now() {
    const std::scoped_lock lock(config_mutex);
    return observers;
  }

  [[nodiscard]] std::optional<idempotency_entry> find_idempotent(const idempotency_key &k) {
    const std::scoped_lock lock(idem_mutex);
    const auto it = idem.find(k);
    if (it == idem.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  // Publishes one event; a failure is logged and never fails a commit (F6 is observability).
  void publish(std::string_view type, const record_id &actor, event_about about,
               event_causality causality, const std::optional<correlation_record> &correlation,
               std::optional<value> payload) {
    event_spec spec{
        .type = std::string(type),
        .schema = event_schema_ref{.type = type_id_of("fabric.history.tx_record").id(),
                                   .major = 1,
                                   .minor = 0},
        .actor = actor,
        .about = std::move(about),
        .correlation = correlation,
        .causality = causality,
        .payload = std::move(payload),
    };
    auto e = events.make(std::move(spec));
    if (!e) {
      sl::log::emit(sl::log::level::error, log_system, "event not made",
                    {{"type", type}, {"error", e.error().message}});
      return;
    }
    if (auto ok = bus.publish(std::move(*e)); !ok) {
      sl::log::emit(sl::log::level::error, log_system, "event not published",
                    {{"type", type}, {"error", ok.error().message}});
    }
  }
};

// ---- transaction state ------------------------------------------------------------------------

enum class ref_mode : std::uint8_t { normal, create, reset };

struct tx_core {
  tx_core(transaction_manager &owner_, manager_core &m, tx_options o, snapshot b)
      : owner(&owner_), mgr(&m), options(std::move(o)), base(std::move(b)), view(base) {}
  tx_core(const tx_core &) = delete;
  tx_core(tx_core &&) = delete;
  tx_core &operator=(const tx_core &) = delete;
  tx_core &operator=(tx_core &&) = delete;
  ~tx_core() = default;

  transaction_manager *owner;
  manager_core *mgr;
  tx_options options;
  snapshot base;     // declared before view: view reads through it
  staged_state view; // read-your-writes
  patch raw;         // the ops as staged
  tx_state st = tx_state::open;
  std::vector<diagnostic> diags;
  std::optional<conflict_report> conflict;
  ref_mode mode = ref_mode::normal;
  std::vector<digest> extra_parents;
};

// The outcome of validation that commit materialises.
struct prepared {
  std::unique_ptr<snapshot> on; // the state the patch applies to (the base, or the rebased head)
  patch author;                 // coalesced staged ops with the transaction's meta
  patch normalized;             // over *on, base = on->revision_digest()
  std::unique_ptr<staged_state> after;
  std::vector<digest> intervening;
};

diagnostic structural(std::string rule, const record_id &record, std::string message) {
  return diagnostic{.rule = std::move(rule),
                    .severity = severity::error,
                    .record = record,
                    .path = {},
                    .message = std::move(message),
                    .stage = validator_stage::structural};
}

// True for the errors the patch layer reports about the patch itself (patch.hpp); anything else
// (a store read error, a corrupt object) is a failure to judge, not a finding.
bool patch_finding(errc code) noexcept {
  return code == errc::invalid_argument || code == errc::not_found ||
         code == errc::already_exists || code == errc::type_mismatch;
}

// The staged ops coalesced (ADR-0018: one open transaction, first expectation, last value), with
// the transaction's meta and no base.
result<patch> author_patch(const tx_core &t) {
  patch empty;
  empty.version = t.raw.version;
  auto c = compose(t.raw, empty);
  if (!c) {
    return std::unexpected(c.error());
  }
  c->base.reset();
  c->meta = t.options.meta;
  return c;
}

// The revisions from `head` back to `base` along first parents, newest first, with their patches.
// `conflict` when a revision has several parents (a reset or a merge has no single footprint) or
// when `base` is not on the chain.
result<std::vector<std::pair<digest, patch>>> intervening(const store_backend &store,
                                                          const std::optional<digest> &head,
                                                          const std::optional<digest> &base) {
  std::vector<std::pair<digest, patch>> out;
  std::optional<digest> cur = head;
  while (cur && cur != base) {
    auto ro = store.get(*cur);
    if (!ro) {
      return std::unexpected(ro.error());
    }
    auto rev = decode_revision(*ro);
    if (!rev) {
      return std::unexpected(rev.error());
    }
    if (rev->parents.size() > 1) {
      return make_error(errc::conflict, "revision " + cur->to_hex() +
                                            " has several parents (a ref reset or merge); no "
                                            "rebase across it");
    }
    auto txo = store.get(rev->tx);
    if (!txo) {
      return std::unexpected(txo.error());
    }
    auto tx = decode_tx_record(*txo);
    if (!tx) {
      return std::unexpected(tx.error());
    }
    auto po = store.get(tx->patch);
    if (!po) {
      return std::unexpected(po.error());
    }
    auto p = decode_patch_object(*po);
    if (!p) {
      return std::unexpected(p.error());
    }
    out.emplace_back(*cur, std::move(*p));
    cur = rev->parents.empty() ? std::nullopt : std::optional<digest>(rev->parents.front());
  }
  if (cur != base) {
    return make_error(errc::conflict, "the transaction base " +
                                          (base ? base->to_hex() : std::string("(empty)")) +
                                          " is not an ancestor of the head");
  }
  return out;
}

conflict_report make_report(const tx_core &t, errc code, const std::optional<digest> &head,
                            footprint ours, std::string message) {
  conflict_report r;
  r.code = code;
  r.ref = t.options.ref;
  r.base = t.base.revision_digest();
  r.head = head;
  r.ours = std::move(ours);
  r.message = std::move(message);
  return r;
}

// Everything commit decides before C3 (ARCH2-R05, R08, R15, R46), over `head` (the published head
// of the ref now). Side effects on `t`: diags and conflict. Errors: `validation_failed`,
// `stale_base`, `conflict`, `locked`, or a failure to judge.
result<prepared> prepare(manager_core &m, tx_core &t, const snapshot &head) {
  t.diags.clear();
  t.conflict.reset();
  prepared p;
  auto author = author_patch(t);
  if (!author) {
    t.diags.push_back(
        structural("R-STR-APPLY", record_id{},
                   std::format("{}: the staged ops do not compose: {}",
                               to_string(author.error().code), author.error().message)));
    return make_error(errc::validation_failed, "the staged ops do not compose");
  }
  p.author = std::move(*author);
  const footprint ours = footprint_of(p.author);
  p.on = std::make_unique<snapshot>(t.base);

  if (t.mode == ref_mode::normal && head.revision_digest() != t.base.revision_digest()) {
    auto walk = intervening(*m.store, head.revision_digest(), t.base.revision_digest());
    if (t.options.policy == commit_policy::reject_stale) {
      conflict_report r = make_report(
          t, errc::stale_base, head.revision_digest(), ours,
          "the base moved: " + t.options.ref + " is at " +
              (head.revision_digest() ? head.revision_digest()->to_hex() : std::string("(empty)")));
      if (walk) {
        for (const auto &[rev, q] : *walk) {
          r.intervening.push_back(rev);
          auto o = ours.overlaps(footprint_of(q));
          r.overlaps.insert(r.overlaps.end(), o.begin(), o.end());
        }
      }
      std::string message = r.message;
      t.conflict = std::move(r);
      return make_error(errc::stale_base, message);
    }
    if (!walk) {
      if (walk.error().code != errc::conflict) {
        return std::unexpected(walk.error());
      }
      t.conflict =
          make_report(t, errc::conflict, head.revision_digest(), ours, walk.error().message);
      return std::unexpected(walk.error());
    }
    for (const auto &[rev, q] : *walk) {
      p.intervening.push_back(rev);
      if (auto r = rebase_if_disjoint(p.author, q); !r) {
        conflict_report report =
            make_report(t, errc::conflict, head.revision_digest(), ours,
                        "overlaps revision " + rev.to_hex() + ": " + r.error().message);
        report.overlaps = ours.overlaps(footprint_of(q));
        report.intervening = p.intervening;
        std::string message = report.message;
        t.conflict = std::move(report);
        return make_error(errc::conflict, message);
      }
    }
    *p.on = head;
  }

  auto norm = normalize(p.author, *p.on);
  if (!norm) {
    if (norm.error().code == errc::conflict) {
      t.conflict = make_report(t, errc::conflict, head.revision_digest(), ours,
                               "an expectation does not hold: " + norm.error().message);
      return std::unexpected(norm.error());
    }
    if (!patch_finding(norm.error().code)) {
      return std::unexpected(norm.error());
    }
    t.diags.push_back(structural("R-STR-APPLY", record_id{},
                                 std::format("{}: the patch does not apply: {}",
                                             to_string(norm.error().code), norm.error().message)));
    return make_error(errc::validation_failed, "the patch does not apply");
  }
  norm->base = p.on->revision_digest();
  p.after = std::make_unique<staged_state>(*p.on);
  if (auto ok = p.after->apply(*norm); !ok) {
    if (!patch_finding(ok.error().code)) {
      return std::unexpected(ok.error());
    }
    t.diags.push_back(structural("R-STR-APPLY", record_id{},
                                 std::format("{}: the patch does not apply: {}",
                                             to_string(ok.error().code), ok.error().message)));
    return make_error(errc::validation_failed, "the patch does not apply");
  }

  const edit_origin origin{.actor = t.options.origin.actor,
                           .subsystem = t.options.origin.subsystem};
  {
    const validation_context ctx(*norm, *p.after, *m.schemas, origin);
    const origin_frames::frame frame(m.frames, ctx, t.options.origin);
    auto diags = m.validators.run(ctx);
    if (!diags) {
      return std::unexpected(diags.error());
    }
    t.diags = std::move(*diags);
    if (has_error(t.diags)) {
      const auto first = std::ranges::find_if(
          t.diags, [](const diagnostic &d) { return d.severity == severity::error; });
      return make_error(errc::validation_failed,
                        std::format("{} diagnostic(s); first {} [{}] on {}: {}", t.diags.size(),
                                    first->rule, to_string(first->stage), first->record.to_string(),
                                    first->message));
    }
    // The locks stage (ARCH2-R46): the lock authority judges the whole footprint.
    if (auto ok = m.lock_authority_now()->check(origin, ctx.footprint()); !ok) {
      if (ok.error().code != errc::locked) {
        return std::unexpected(ok.error());
      }
      t.conflict =
          make_report(t, errc::locked, head.revision_digest(), ctx.footprint(), ok.error().message);
      return std::unexpected(ok.error());
    }
  }
  p.normalized = std::move(*norm);
  return p;
}

tx_state state_for(errc code) noexcept {
  switch (code) {
  case errc::validation_failed:
  case errc::idempotency_mismatch:
    return tx_state::rejected;
  case errc::stale_base:
  case errc::conflict:
  case errc::locked:
    return tx_state::conflict;
  case errc::commit_indeterminate:
    return tx_state::indeterminate;
  default:
    return tx_state::failed;
  }
}

// The tx record a commit of `patch_digest` writes (ARCH1-R45).
history::tx_record tx_record_for(const tx_core &t, const digest &patch_digest) {
  history::tx_record rec;
  rec.actor = t.options.origin.actor.id();
  const auto sub = subsystem_from_string(t.options.origin.subsystem);
  rec.origin.subsystem = sub ? std::string(owner_code(*sub)) : t.options.origin.subsystem;
  rec.origin.epistemic = t.options.origin.epistemic;
  rec.provenance = t.options.origin.provenance;
  rec.patch = patch_digest;
  if (!t.options.meta.idempotency_key.empty()) {
    rec.idempotency_key = t.options.meta.idempotency_key;
  }
  rec.label = t.options.meta.label;
  if (!t.options.meta.undo_of.empty()) {
    rec.undo_of = t.options.meta.undo_of.front().id();
  }
  return rec;
}

// Emits the refusal event of a rejected or conflicted commit; the caller holds the commit mutex.
void emit_refusal(manager_core &m, const tx_core &t, errc code) {
  const tx_state s = state_for(code);
  if (s != tx_state::rejected && s != tx_state::conflict) {
    return;
  }
  std::optional<value> payload;
  if (auto author = author_patch(t)) {
    if (auto po = encode_patch_object(*author)) {
      if (auto v = tx_record_for(t, po->id).to_value()) {
        payload = std::move(*v);
      }
    }
  }
  event_causality causality;
  causality.revision = t.base.revision_digest();
  m.publish(s == tx_state::rejected ? rejected_event : conflicted_event,
            t.options.origin.actor.id(), event_about{.alt = "fabric.tx/" + t.options.ref},
            causality, t.options.origin.correlation, std::move(payload));
}

// An error made here (with its source location).
error err(errc code, std::string message) { return make_error(code, std::move(message)).error(); }

// Sets the terminal state for `e` and returns it.
std::unexpected<error> finish(tx_core &t, error e) {
  t.st = state_for(e.code);
  return std::unexpected(std::move(e));
}

std::unexpected<error> refuse_locked(manager_core &m, tx_core &t, error e) {
  emit_refusal(m, t, e.code);
  return finish(t, std::move(e));
}

std::unexpected<error> refuse(manager_core &m, tx_core &t, error e) {
  if (const tx_state s = state_for(e.code); s == tx_state::rejected || s == tx_state::conflict) {
    const std::scoped_lock lock(m.commit_mutex);
    emit_refusal(m, t, e.code);
  }
  return finish(t, std::move(e));
}

// Builds the idempotency index and the recovered events from the history of every ref.
result<void> rebuild_from_history(manager_core &m) {
  std::set<digest> seen;
  for (const auto &[ref, head] : m.heads) {
    std::deque<digest> todo{head};
    bool first = true;
    while (!todo.empty()) {
      const digest cur = todo.front();
      todo.pop_front();
      if (!seen.insert(cur).second) {
        continue;
      }
      auto ro = m.store->get(cur);
      if (!ro) {
        return std::unexpected(ro.error());
      }
      auto rev = decode_revision(*ro);
      if (!rev) {
        return std::unexpected(rev.error());
      }
      auto txo = m.store->get(rev->tx);
      if (!txo) {
        return std::unexpected(txo.error());
      }
      auto tx = decode_tx_record(*txo);
      if (!tx) {
        return std::unexpected(tx.error());
      }
      auto actor = actor_id::from(tx->actor);
      const tx_id id = actor ? tx_id_for(*actor, txo->id) : tx_id{};
      if (first) {
        // FP6: the events of the last commit may be lost; recovery reports each head.
        event_causality causality;
        if (!id.is_null()) {
          causality.tx = id.id();
        }
        causality.revision = cur;
        event_about about{.alt = "fabric.tx/" + ref};
        if (!id.is_null()) {
          about.alt = id.id();
        }
        std::optional<value> payload;
        if (auto v = tx->to_value()) {
          payload = std::move(*v);
        }
        m.publish(recovered_event, tx->actor, std::move(about), causality, std::nullopt,
                  std::move(payload));
        first = false;
      }
      if (tx->idempotency_key && actor) {
        auto po = m.store->get(tx->patch);
        if (!po) {
          return std::unexpected(po.error());
        }
        auto p = decode_patch_object(*po);
        if (!p) {
          return std::unexpected(p.error());
        }
        auto d = idempotency_digest(*p);
        if (!d) {
          return std::unexpected(d.error());
        }
        commit_receipt r;
        r.ref = ref;
        r.revision = cur;
        r.root = rev->root;
        r.tx = id;
        r.undo = undo_token{.tx = id, .tx_record = txo->id, .revision = cur};
        r.recovered = true;
        m.idem.try_emplace(idempotency_key{tx->actor, *tx->idempotency_key},
                           idempotency_entry{.patch = *d, .receipt = std::move(r)});
      }
      for (const digest &parent : rev->parents) {
        todo.push_back(parent);
      }
    }
  }
  return {};
}

impact_summary changed_summary(std::span<const staged_change> changes, const digest &revision) {
  impact_set set;
  set.at_revision = revision;
  for (const staged_change &c : changes) {
    set.nodes.push_back(impact_node{.id = c.id,
                                    .cls = c.id.cls(),
                                    .distance = 0,
                                    .via = record_id{},
                                    .via_kind = edge_kind::reference});
  }
  std::ranges::sort(set.nodes, {}, &impact_node::id);
  set.visited = set.nodes.size();
  return summarize(set);
}

} // namespace tx_detail

// ---- names ------------------------------------------------------------------------------------

std::string_view to_string(tx_state s) noexcept {
  switch (s) {
  case tx_state::open:
    return "Open";
  case tx_state::validating:
    return "Validating";
  case tx_state::rejected:
    return "Rejected";
  case tx_state::committing:
    return "Committing";
  case tx_state::committed:
    return "Committed";
  case tx_state::conflict:
    return "Conflict";
  case tx_state::failed:
    return "Failed";
  case tx_state::indeterminate:
    return "Indeterminate";
  case tx_state::aborted:
    return "Aborted";
  }
  return "Unknown";
}

bool is_terminal(tx_state s) noexcept { return s != tx_state::open && s != tx_state::validating; }

// ---- transaction ------------------------------------------------------------------------------

struct transaction::impl : tx_detail::tx_core {
  using tx_core::tx_core;
};

struct transaction_manager::state : tx_detail::manager_core {
  using manager_core::manager_core;
};

transaction::transaction(std::unique_ptr<impl> p) noexcept : impl_(std::move(p)) {}
transaction::transaction(transaction &&other) noexcept = default;

transaction &transaction::operator=(transaction &&other) noexcept {
  if (this != &other) {
    abort();
    impl_ = std::move(other.impl_);
  }
  return *this;
}

transaction::~transaction() { abort(); }

namespace {

result<void> require_open(tx_state s) {
  if (s == tx_state::open) {
    return {};
  }
  if (s == tx_state::aborted) {
    return make_error(errc::tx_aborted, "the transaction was aborted");
  }
  return make_error(errc::tx_closed,
                    "the transaction is " + std::string(to_string(s)) + ", not Open");
}

} // namespace

result<void> transaction::stage(const patch_op &op) {
  if (!impl_ || impl_->st != tx_state::open) {
    return make_error(errc::tx_closed, "staging on a closed transaction");
  }
  if (auto ok = impl_->view.apply(op); !ok) {
    if (ok.error().code == errc::conflict) {
      impl_->conflict =
          tx_detail::make_report(*impl_, errc::conflict, impl_->base.revision_digest(),
                                 footprint_of(op), ok.error().message);
    }
    return ok;
  }
  impl_->raw.ops.push_back(op);
  return {};
}

result<void> transaction::stage(const patch &p) {
  if (!impl_ || impl_->st != tx_state::open) {
    return make_error(errc::tx_closed, "staging on a closed transaction");
  }
  if (p.base && p.base != impl_->base.revision_digest()) {
    return make_error(errc::stale_base,
                      "the patch was authored on " + p.base->to_hex() +
                          ", the transaction stages over " +
                          (impl_->base.revision_digest() ? impl_->base.revision_digest()->to_hex()
                                                         : std::string("(empty)")));
  }
  if (p.version.major != patch_major) {
    return make_error(errc::format_unsupported,
                      std::format("patch major {} is not {}", p.version.major, patch_major));
  }
  if (auto ok = impl_->view.apply(p); !ok) {
    if (ok.error().code == errc::conflict) {
      impl_->conflict =
          tx_detail::make_report(*impl_, errc::conflict, impl_->base.revision_digest(),
                                 footprint_of(p), ok.error().message);
    }
    return ok;
  }
  impl_->raw.ops.insert(impl_->raw.ops.end(), p.ops.begin(), p.ops.end());
  impl_->raw.version.minor = std::max(impl_->raw.version.minor, p.version.minor);
  patch_meta &m = impl_->options.meta;
  if (m.label.empty()) {
    m.label = p.meta.label;
  }
  if (m.undo_group.is_null()) {
    m.undo_group = p.meta.undo_group;
  }
  if (m.coalesce_key.empty()) {
    m.coalesce_key = p.meta.coalesce_key;
  }
  if (m.idempotency_key.empty()) {
    m.idempotency_key = p.meta.idempotency_key;
  }
  if (m.undo_of.empty()) {
    m.undo_of = p.meta.undo_of;
  }
  return {};
}

const staged_state &transaction::view() const noexcept { return impl_->view; }
const patch &transaction::staged() const noexcept { return impl_->raw; }
const std::optional<digest> &transaction::base() const noexcept {
  return impl_->base.revision_digest();
}
tx_state transaction::state() const noexcept { return impl_ ? impl_->st : tx_state::aborted; }
std::span<const diagnostic> transaction::diagnostics() const noexcept { return impl_->diags; }
const std::optional<conflict_report> &transaction::conflict() const noexcept {
  return impl_->conflict;
}
const tx_options &transaction::options() const noexcept { return impl_->options; }

result<std::vector<diagnostic>> transaction::validate() {
  if (!impl_) {
    return make_error(errc::tx_closed, "moved-from transaction");
  }
  if (auto ok = require_open(impl_->st); !ok) {
    return std::unexpected(ok.error());
  }
  tx_detail::tx_core &t = *impl_;
  t.st = tx_state::validating;
  auto head = t.mgr->snapshot_of(t.mgr->published(t.options.ref));
  if (!head) {
    t.st = tx_state::open;
    return std::unexpected(head.error());
  }
  auto p = tx_detail::prepare(*t.mgr, t, *head);
  t.st = tx_state::open;
  if (!p && p.error().code != errc::validation_failed) {
    return std::unexpected(p.error());
  }
  return t.diags;
}

result<commit_receipt> transaction::commit() {
  if (!impl_) {
    return make_error(errc::tx_closed, "moved-from transaction");
  }
  return impl_->owner->commit_transaction(*impl_);
}

void transaction::abort() noexcept {
  if (impl_ && impl_->st == tx_state::open) {
    impl_->st = tx_state::aborted;
  }
}

// ---- transaction_manager ----------------------------------------------------------------------

transaction_manager::transaction_manager(open_key /*key*/, std::unique_ptr<state> s) noexcept
    : state_(std::move(s)) {}
transaction_manager::~transaction_manager() = default;

result<std::unique_ptr<transaction_manager>> transaction_manager::create(manager_config config) {
  if (config.store == nullptr || config.schemas == nullptr || config.domains == nullptr ||
      config.actors == nullptr || !config.clock || !config.ids) {
    return make_error(errc::invalid_argument,
                      "a transaction manager needs a store, the registries, a clock and an id "
                      "source");
  }
  if (!config.schemas->frozen()) {
    return make_error(errc::schema_invalid, "the schema registry is not frozen (ARCH2-R18)");
  }
  if (!config.domains->frozen()) {
    return make_error(errc::invalid_argument, "the domain registry is not frozen (ARCH2-R18)");
  }
  auto s = std::make_unique<state>(config);
  auto builtins = builtin_validators(
      builtin_validator_env{.domains = s->domains, .actors = s->actors, .provenance = &s->frames});
  if (!builtins) {
    return std::unexpected(builtins.error());
  }
  for (auto &v : *builtins) {
    if (std::ranges::find(config.omit_builtin, v->name()) != config.omit_builtin.end()) {
      continue;
    }
    if (auto ok = s->validators.add(std::move(v)); !ok) {
      return std::unexpected(ok.error());
    }
  }
  for (auto &v : config.validators) {
    if (auto ok = s->validators.add(std::move(v)); !ok) {
      return std::unexpected(ok.error());
    }
  }
  s->validators.freeze();
  for (auto &[type, driver] : config.merge_drivers) {
    if (auto ok = s->merge_drivers.add(type, std::move(driver)); !ok) {
      return std::unexpected(ok.error());
    }
  }
  s->merge_drivers.freeze();
  for (auto &[prefix, handler] : config.subscribers) {
    s->subscribed.push_back(s->bus.subscribe(prefix, std::move(handler)));
  }
  auto refs = s->store->list_refs();
  if (!refs) {
    return std::unexpected(refs.error());
  }
  for (const ref_entry &r : *refs) {
    s->heads.insert_or_assign(r.name, r.target);
  }
  if (auto ok = tx_detail::rebuild_from_history(*s); !ok) {
    return std::unexpected(ok.error());
  }
  return std::make_unique<transaction_manager>(open_key{}, std::move(s));
}

result<transaction> transaction_manager::begin(tx_options options) {
  state &s = *state_;
  if (s.indeterminate.load()) {
    return make_error(errc::recovery_required,
                      "a commit is indeterminate; reopen the workspace (ARCH2-R38)");
  }
  if (options.origin.actor.is_null()) {
    return make_error(errc::invalid_argument, "a transaction needs an actor");
  }
  if (!subsystem_from_string(options.origin.subsystem)) {
    return make_error(errc::invalid_argument, "'" + options.origin.subsystem +
                                                  "' is no subsystem owner code or prefix name");
  }
  if (!tx_detail::valid_ref_name(options.ref)) {
    return make_error(errc::invalid_argument,
                      "bad ref name '" + options.ref + "' (1 to 255 bytes of 0x21..0x7E)");
  }
  auto base = options.base ? snapshot::open(*s.store, *options.base)
                           : s.snapshot_of(s.published(options.ref));
  if (!base) {
    return std::unexpected(base.error());
  }
  options.base = base->revision_digest();
  return transaction(
      std::make_unique<transaction::impl>(*this, s, std::move(options), std::move(*base)));
}

result<snapshot> transaction_manager::head(std::string_view ref) const {
  return state_->snapshot_of(state_->published(ref));
}

result<snapshot> transaction_manager::at(const digest &revision) const {
  return snapshot::open(*state_->store, revision);
}

std::vector<ref_entry> transaction_manager::refs() const {
  const std::shared_lock lock(state_->heads_mutex);
  std::vector<ref_entry> out;
  out.reserve(state_->heads.size());
  for (const auto &[name, target] : state_->heads) {
    out.push_back(ref_entry{.name = name, .target = target});
  }
  return out;
}

validator_registry &transaction_manager::validators() noexcept { return state_->validators; }
event_bus &transaction_manager::events() noexcept { return state_->bus; }
merge_driver_registry &transaction_manager::merge_drivers() noexcept {
  return state_->merge_drivers;
}
failpoint_set &transaction_manager::failpoints() noexcept { return state_->failpoints; }

void transaction_manager::set_lock_authority(std::shared_ptr<lock_authority> locks) {
  const std::scoped_lock lock(state_->config_mutex);
  state_->locks = locks ? std::move(locks) : std::make_shared<null_lock_authority>();
}

void transaction_manager::add_observer(std::shared_ptr<commit_observer> observer) {
  if (!observer) {
    return;
  }
  const std::scoped_lock lock(state_->config_mutex);
  state_->observers.push_back(std::move(observer));
}

result<record_id> transaction_manager::mint(id_class c) {
  const std::scoped_lock lock(state_->ids_mutex);
  return fabric::mint(c, *state_->ids);
}

bool transaction_manager::recovery_required() const noexcept {
  return state_->indeterminate.load();
}
std::uint64_t transaction_manager::commits() const noexcept { return state_->commits.load(); }
const store_backend &transaction_manager::store() const noexcept { return *state_->store; }
const schema_registry &transaction_manager::schemas() const noexcept { return *state_->schemas; }

namespace {

result<value> ref_note(std::string_view op, std::string_view name, const digest &target) {
  auto op_v = value::string(op);
  auto name_v = value::string(name);
  if (!op_v || !name_v) {
    return make_error(errc::invalid_argument, "bad ref name");
  }
  return value::struct_({field{.no = 1, .v = *op_v}, field{.no = 2, .v = *name_v},
                         field{.no = 3, .v = value::digest(target)}});
}

} // namespace

result<commit_receipt> transaction_manager::create_ref(std::string_view name, const digest &target,
                                                       const tx_origin &origin) {
  tx_options o;
  o.origin = origin;
  o.ref = std::string(name);
  o.base = target;
  o.meta.label = "create_ref " + std::string(name);
  auto t = begin(std::move(o));
  if (!t) {
    return std::unexpected(t.error());
  }
  auto note = ref_note("create_ref", name, target);
  if (!note) {
    return std::unexpected(note.error());
  }
  t->impl_->mode = tx_detail::ref_mode::create;
  if (auto ok = t->stage(op_annotate(*note)); !ok) {
    return std::unexpected(ok.error());
  }
  return t->commit();
}

result<commit_receipt> transaction_manager::reset_ref(std::string_view name, const digest &target,
                                                      const tx_origin &origin) {
  tx_options o;
  o.origin = origin;
  o.ref = std::string(name);
  o.base = target;
  o.meta.label = "reset_ref " + std::string(name);
  auto t = begin(std::move(o));
  if (!t) {
    return std::unexpected(t.error());
  }
  auto note = ref_note("reset_ref", name, target);
  if (!note) {
    return std::unexpected(note.error());
  }
  t->impl_->mode = tx_detail::ref_mode::reset;
  if (auto ok = t->stage(op_annotate(*note)); !ok) {
    return std::unexpected(ok.error());
  }
  return t->commit();
}

result<commit_ack> transaction_manager::write_batch(commit_batch batch) noexcept {
  const mutation_capability cap{};
  return state_->store->commit(cap, std::move(batch));
}

result<commit_receipt> transaction_manager::commit_transaction(transaction::impl &t) {
  using namespace tx_detail;
  state &m = *state_;
  if (auto ok = require_open(t.st); !ok) {
    return std::unexpected(ok.error());
  }
  t.st = tx_state::validating;
  t.diags.clear();
  t.conflict.reset();
  if (m.indeterminate.load()) {
    return finish(t, err(errc::recovery_required,
                         "a commit is indeterminate; reopen the workspace (ARCH2-R38)"));
  }
  if (auto fp = m.failpoints.visit(failpoint::fp0); !fp) {
    return finish(t, fp.error());
  }

  // ARCH2-R09: a keyed retry of a committed patch returns the original receipt.
  std::optional<idempotency_key> key;
  digest idem_digest;
  if (t.mode == ref_mode::normal && !t.options.meta.idempotency_key.empty()) {
    auto author = author_patch(t);
    if (!author) {
      return finish(t, author.error());
    }
    auto d = idempotency_digest(*author);
    if (!d) {
      return finish(t, d.error());
    }
    idem_digest = *d;
    key = idempotency_key{t.options.origin.actor.id(), t.options.meta.idempotency_key};
  }
  const auto replay = [&](const idempotency_entry &e) -> result<commit_receipt> {
    if (e.patch != idem_digest) {
      return finish(t, err(errc::idempotency_mismatch, "idempotency key '" + key->second +
                                                           "' of actor " + key->first.to_string() +
                                                           " was used for another patch"));
    }
    t.st = tx_state::committed;
    return e.receipt;
  };
  if (key) {
    if (auto e = m.find_idempotent(*key)) {
      if (e->patch != idem_digest) {
        return refuse(m, t, replay(*e).error());
      }
      return replay(*e);
    }
  }

  // Validation over the base (or the rebased head), outside the commit mutex.
  auto head0 = m.snapshot_of(m.published(t.options.ref));
  if (!head0) {
    return finish(t, head0.error());
  }
  auto prep = prepare(m, t, *head0);
  if (!prep) {
    return refuse(m, t, prep.error());
  }
  if (auto fp = m.failpoints.visit(failpoint::fp1); !fp) {
    return finish(t, fp.error());
  }

  // C1: the commit mutex; re-read the head and apply the commit policy.
  const std::unique_lock lock(m.commit_mutex);
  if (m.indeterminate.load()) {
    return finish(t, err(errc::recovery_required,
                         "a commit is indeterminate; reopen the workspace (ARCH2-R38)"));
  }
  if (key) {
    if (auto e = m.find_idempotent(*key)) {
      if (e->patch != idem_digest) {
        return refuse_locked(m, t, replay(*e).error());
      }
      return replay(*e);
    }
  }
  const std::optional<digest> head = m.published(t.options.ref);
  std::optional<digest> expected = head;
  switch (t.mode) {
  case ref_mode::normal:
    if (head != prep->on->revision_digest()) {
      auto now = m.snapshot_of(head);
      if (!now) {
        return finish(t, now.error());
      }
      prep = prepare(m, t, *now);
      if (!prep) {
        return refuse_locked(m, t, prep.error());
      }
    }
    break;
  case ref_mode::create:
    if (head) {
      return finish(t, err(errc::already_exists, "ref " + t.options.ref + " exists"));
    }
    break;
  case ref_mode::reset:
    if (!head) {
      return finish(t, err(errc::not_found, "no ref " + t.options.ref));
    }
    if (prep->on->revision_digest() != head) {
      t.extra_parents = {*head};
    }
    break;
  }

  // C2: the lock authority again, under the mutex.
  const edit_origin origin{.actor = t.options.origin.actor,
                           .subsystem = t.options.origin.subsystem};
  if (auto ok = m.lock_authority_now()->check(origin, footprint_of(prep->normalized)); !ok) {
    if (ok.error().code == errc::locked) {
      t.conflict =
          make_report(t, errc::locked, head, footprint_of(prep->normalized), ok.error().message);
      return refuse_locked(m, t, ok.error());
    }
    return finish(t, ok.error());
  }
  t.st = tx_state::committing;

  // C3: materialise.
  auto patch_object = encode_patch_object(prep->normalized);
  if (!patch_object) {
    return finish(t, patch_object.error());
  }
  const history::tx_record record = tx_record_for(t, patch_object->id);
  auto tx_object = encode_tx_record(record);
  if (!tx_object) {
    return finish(t, tx_object.error());
  }
  const tx_id id = tx_id_for(t.options.origin.actor, tx_object->id);
  revision_builder builder(*prep->on, *m.schemas);
  const snapshot &on = *prep->on;
  const schema_registry &schemas = *m.schemas;
  const auto document_of = [&on, &schemas](const record_id &rid,
                                           const record_state &st) -> result<record_id> {
    if (const type_desc *ty = schemas.find(st.type); ty != nullptr && ty->document_root) {
      return rid;
    }
    auto e = on.get(rid);
    if (e) {
      return e->document;
    }
    if (e.error().code == errc::not_found) {
      return record_id{};
    }
    return std::unexpected(e.error());
  };
  if (auto ok = builder.stage(prep->after->changes(), document_of); !ok) {
    return finish(t, ok.error());
  }
  auto built = builder.build(
      revision_inputs{.tx = record, .tx_id = id.id(), .extra_parents = t.extra_parents});
  if (!built) {
    return finish(t, built.error());
  }
  commit_batch batch = std::move(built->batch);
  auto stored_patch = m.store->has(patch_object->id);
  if (!stored_patch) {
    return finish(t, stored_patch.error());
  }
  if (!*stored_patch) {
    batch.objects.insert(batch.objects.begin(), std::move(*patch_object));
  }
  batch.refs.push_back(
      ref_update{.name = t.options.ref, .expected = expected, .desired = built->revision});
  const std::int64_t commit_time = m.clock->now_us();
  batch.commit_time_us = commit_time;
  if (auto fp = m.failpoints.visit(failpoint::fp2); !fp) {
    return finish(t, fp.error()); // the objects lived in memory only
  }

  // C4: the commit point.
  auto ack = write_batch(std::move(batch));
  if (!ack) {
    if (ack.error().code == errc::commit_indeterminate) {
      m.indeterminate.store(true);
      return finish(t, ack.error());
    }
    if (ack.error().code == errc::ref_cas_failed) {
      t.conflict = make_report(t, errc::stale_base, head, footprint_of(prep->normalized),
                               ack.error().message);
      return refuse_locked(m, t, err(errc::stale_base, ack.error().message));
    }
    return finish(t, ack.error());
  }
  const auto after_commit_point = [&](const error &e) {
    m.indeterminate.store(true);
    return finish(
        t, err(errc::commit_indeterminate, e.message + "; the commit may be durable (ARCH2-R38)"));
  };
  if (auto fp = m.failpoints.visit(failpoint::fp5); !fp) {
    return after_commit_point(fp.error());
  }

  // C5: publish the new head.
  {
    const std::unique_lock heads_lock(m.heads_mutex);
    m.heads.insert_or_assign(t.options.ref, built->revision);
  }
  m.commits.fetch_add(1);
  commit_receipt receipt;
  receipt.ref = t.options.ref;
  receipt.revision = built->revision;
  receipt.root = built->root;
  receipt.tx = id;
  receipt.seq = ack->seq;
  receipt.commit_time_us = commit_time;
  receipt.impact = changed_summary(prep->after->changes(), built->revision);
  receipt.undo = undo_token{.tx = id, .tx_record = built->tx, .revision = built->revision};
  if (key) {
    const std::scoped_lock idem_lock(m.idem_mutex);
    m.idem.insert_or_assign(*key, idempotency_entry{.patch = idem_digest, .receipt = receipt});
  }
  if (auto fp = m.failpoints.visit(failpoint::fp6); !fp) {
    return after_commit_point(fp.error());
  }

  // C6: observers in registration order, then the committed event.
  commit_event ev;
  ev.tx = id;
  ev.seq = ack->seq;
  ev.revision = built->revision;
  ev.root = built->root;
  ev.origin = origin;
  ev.patch = prep->normalized;
  for (const staged_change &c : prep->after->changes()) {
    ev.changed.push_back(c.id);
  }
  receipt_builder attachments;
  for (const auto &observer : m.observers_now()) {
    if (auto ok = observer->on_commit(ev, attachments); !ok) {
      sl::log::emit(sl::log::level::warn, log_system, "commit observer failed",
                    {{"tx", id.to_string()}, {"error", ok.error().message}});
    }
  }
  receipt.attachments = attachments.attachments();
  if (key) {
    const std::scoped_lock idem_lock(m.idem_mutex);
    m.idem.insert_or_assign(*key, idempotency_entry{.patch = idem_digest, .receipt = receipt});
  }
  event_causality causality;
  causality.tx = id.id();
  causality.revision = built->revision;
  std::optional<value> payload;
  if (auto v = record.to_value()) {
    payload = std::move(*v);
  }
  m.publish(committed_event, t.options.origin.actor.id(), event_about{.alt = id.id()}, causality,
            t.options.origin.correlation, std::move(payload));

  // C7.
  t.st = tx_state::committed;
  return receipt;
}

} // namespace sl::fabric
