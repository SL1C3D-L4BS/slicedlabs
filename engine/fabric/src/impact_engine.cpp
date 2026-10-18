// The change-impact engine: the incremental dependency index kept by a commit observer, impact
// reports and the currency check (WO-056, SL-ARCH-001 ARCH1-R57 to ARCH1-R59).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/deps.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/events.hpp"
#include "sl/fabric/gen/fabric.history.gen.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/impact.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/tree.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

namespace {

namespace history = gen::fabric_history;

struct revision_view {
  history::revision revision;
  history::tx_record tx;
};

result<revision_view> read_revision(const store_backend &store, const digest &rev) {
  auto ro = store.get(rev);
  if (!ro) {
    return std::unexpected(std::move(ro.error()));
  }
  auto r = decode_revision(*ro);
  if (!r) {
    return std::unexpected(std::move(r.error()));
  }
  auto txo = store.get(r->tx);
  if (!txo) {
    return std::unexpected(std::move(txo.error()));
  }
  auto tx = decode_tx_record(*txo);
  if (!tx) {
    return std::unexpected(std::move(tx.error()));
  }
  return revision_view{.revision = std::move(*r), .tx = std::move(*tx)};
}

result<snapshot> snapshot_at(const store_backend &store, const std::optional<digest> &revision) {
  return revision ? snapshot::open(store, *revision) : snapshot::empty(store);
}

bool is_derived(const schema_registry &reg, const type_id &type) noexcept {
  const type_desc *t = reg.find(type);
  return t != nullptr && t->state_class == state_class::derived;
}

std::optional<derivation_pin> pin_from_tx(const history::tx_record &tx, const digest &object) {
  if (!tx.provenance) {
    return std::nullopt;
  }
  return pin_of(*tx.provenance, object);
}

std::vector<record_id> input_ids(const derivation_pin *pin) {
  std::vector<record_id> ids;
  if (pin != nullptr) {
    ids.reserve(pin->inputs.size());
    for (const pinned_input &in : pin->inputs) {
      if (ids.empty() || ids.back() != in.id) {
        ids.push_back(in.id);
      }
    }
  }
  return ids;
}

result<std::vector<edge>> edges_of(const impact_options &options, const record_view &rv,
                                   const schema_registry &reg) {
  return options.edges ? options.edges(rv, reg) : default_edges(rv, reg);
}

// The pins of the derived records `unresolved` (live in `head`): the provenance of the tx of the
// newest first-parent revision at which each one's entry changed.
result<std::map<record_id, derivation_pin>> resolve_pins(const snapshot &head,
                                                         std::set<record_id> unresolved) {
  std::map<record_id, derivation_pin> out;
  std::optional<digest> cur = head.revision_digest();
  if (unresolved.empty() || !cur) {
    return out;
  }
  const store_backend &store = head.store();
  snapshot cur_snap = head;
  while (!unresolved.empty() && cur) {
    auto view = read_revision(store, *cur);
    if (!view) {
      return std::unexpected(std::move(view.error()));
    }
    std::optional<digest> parent;
    if (!view->revision.parents.empty()) {
      parent = view->revision.parents.front();
    }
    auto parent_snap = snapshot_at(store, parent);
    if (!parent_snap) {
      return std::unexpected(std::move(parent_snap.error()));
    }
    auto deltas = parent_snap->diff(cur_snap);
    if (!deltas) {
      return std::unexpected(std::move(deltas.error()));
    }
    for (const record_delta &d : *deltas) {
      const auto it = unresolved.find(d.id);
      if (it == unresolved.end()) {
        continue;
      }
      unresolved.erase(it);
      if (d.after && d.after->kind == entry_kind::record) {
        if (auto pin = pin_from_tx(view->tx, d.after->target)) {
          out.insert_or_assign(d.id, std::move(*pin));
        }
      }
    }
    cur = parent;
    cur_snap = std::move(*parent_snap);
  }
  return out;
}

impact_report make_report(const impact_index &index, const impact_set &set,
                          std::size_t summary_ids) {
  impact_report r;
  r.summary = summarize(set, summary_ids);
  r.visited = set.visited;
  r.edges_examined = set.edges_examined;
  r.at_revision = set.at_revision;
  std::map<std::pair<impact_role, type_id>, role_impact> roles;
  const auto add = [&](impact_role role, const impact_node &n) {
    type_id type;
    if (const auto t = index.types.find(n.id); t != index.types.end()) {
      type = t->second;
    }
    role_impact &ri = roles[{role, type}];
    ri.role = role;
    ri.type = type;
    ++ri.count;
    if (ri.first.size() < summary_ids) {
      ri.first.push_back(n.id);
    }
  };
  for (const impact_node &n : set.nodes) {
    bool test = false;
    bool target = false;
    for (const edge &e : index.graph.out(n.id)) {
      test = test || e.kind == edge_kind::validates;
      target = target || e.kind == edge_kind::builds;
    }
    if (test) {
      add(impact_role::test, n);
    }
    if (target) {
      add(impact_role::build_target, n);
    }
  }
  r.roles.reserve(roles.size());
  for (auto &[key, ri] : roles) {
    r.roles.push_back(std::move(ri));
  }
  return r;
}

closure_options closure_of(const impact_options &o, const std::optional<digest> &revision) {
  return closure_options{.kinds = all_edge_kinds,
                         .max_distance = o.max_distance,
                         .max_nodes = o.max_nodes,
                         .at_revision = revision};
}

error with_context(const error &e, std::string_view what) {
  error out = e;
  out.message = std::format("impact index marked for rebuild ({}): {}", what, e.message);
  return out;
}

} // namespace

struct impact_engine::state {
  transaction_manager *manager = nullptr;
  impact_options options;

  std::mutex mutex; // guards everything below
  impact_index index;
  bool stale = false;
  impact_stats stats;
  std::map<record_id, impact_report> reports;
  std::deque<record_id> report_order;

  // Rebuilds from the published head of the ref when marked (the "next query").
  [[nodiscard]] result<void> ensure_fresh() {
    if (!stale) {
      return {};
    }
    auto head = manager->head(options.ref);
    if (!head) {
      return std::unexpected(std::move(head.error()));
    }
    auto built = rebuild(*head, manager->schemas(), options);
    if (!built) {
      return std::unexpected(std::move(built.error()));
    }
    index = std::move(*built);
    stale = false;
    ++stats.rebuilds;
    return {};
  }

  // Applies the records-trie differences from the indexed revision to `next`, written by `tx`.
  // Returns the changed ids. An error leaves the index half-updated: the caller marks it stale.
  [[nodiscard]] result<std::vector<record_id>> apply(const snapshot &next,
                                                     const history::tx_record &tx) {
    const schema_registry &reg = manager->schemas();
    auto prev = snapshot_at(next.store(), index.at_revision);
    if (!prev) {
      return std::unexpected(std::move(prev.error()));
    }
    auto deltas = prev->diff(next);
    if (!deltas) {
      return std::unexpected(std::move(deltas.error()));
    }
    std::vector<record_id> changed;
    changed.reserve(deltas->size());
    for (const record_delta &d : *deltas) {
      changed.push_back(d.id);
      if (!d.after || d.after->kind != entry_kind::record) {
        index.graph.erase_record(d.id);
        index.types.erase(d.id);
        index.pins.erase(d.id);
        continue;
      }
      auto rec = next.get(d.id);
      if (!rec) {
        return std::unexpected(std::move(rec.error()));
      }
      index.types.insert_or_assign(d.id, rec->type);
      index.pins.erase(d.id);
      if (is_derived(reg, rec->type)) {
        if (auto pin = pin_from_tx(tx, rec->object)) {
          index.pins.insert_or_assign(d.id, std::move(*pin));
        }
      }
      const auto pin = index.pins.find(d.id);
      const std::vector<record_id> inputs =
          input_ids(pin == index.pins.end() ? nullptr : &pin->second);
      const record_view rv{
          .id = d.id, .type = rec->type, .body = &rec->body, .provenance_inputs = inputs};
      auto edges = edges_of(options, rv, reg);
      if (!edges) {
        return std::unexpected(std::move(edges.error()));
      }
      if (auto ok = index.graph.set_record(d.id, std::move(*edges)); !ok) {
        return std::unexpected(std::move(ok.error()));
      }
    }
    index.at_revision = next.revision_digest();
    stats.deltas += deltas->size();
    return changed;
  }

  void remember(const record_id &tx, impact_report report) {
    if (options.remembered == 0) {
      return;
    }
    if (reports.insert_or_assign(tx, std::move(report)).second) {
      report_order.push_back(tx);
    }
    while (report_order.size() > options.remembered) {
      reports.erase(report_order.front());
      report_order.pop_front();
    }
  }
};

impact_engine::impact_engine(attach_key /*key*/, std::unique_ptr<state> s) noexcept
    : state_(std::move(s)) {}

impact_engine::~impact_engine() = default;

result<std::shared_ptr<impact_engine>>
impact_engine::attach(transaction_manager &manager, const snapshot &head, impact_options options) {
  if (&head.store() != &manager.store()) {
    return make_error(errc::invalid_argument,
                      "impact_engine::attach: the head snapshot reads another store");
  }
  auto built = rebuild(head, manager.schemas(), options);
  if (!built) {
    return std::unexpected(std::move(built.error()));
  }
  auto s = std::make_unique<state>();
  s->manager = &manager;
  s->options = std::move(options);
  s->index = std::move(*built);
  s->stats.rebuilds = 1;
  auto engine = std::make_shared<impact_engine>(attach_key{}, std::move(s));
  manager.add_observer(engine);
  return engine;
}

result<impact_index> impact_engine::rebuild(const snapshot &head, const schema_registry &reg,
                                            const impact_options &options) {
  impact_index out;
  out.at_revision = head.revision_digest();
  std::set<record_id> derived;
  auto typed = head.for_each_entry([&](const snapshot_entry &e) -> result<void> {
    out.types.insert_or_assign(e.id, e.type);
    if (is_derived(reg, e.type)) {
      derived.insert(e.id);
    }
    return {};
  });
  if (!typed) {
    return std::unexpected(std::move(typed.error()));
  }
  auto pins = resolve_pins(head, std::move(derived));
  if (!pins) {
    return std::unexpected(std::move(pins.error()));
  }
  out.pins = std::move(*pins);
  auto walked = head.for_each_entry([&](const snapshot_entry &e) -> result<void> {
    const auto pin = out.pins.find(e.id);
    const std::vector<record_id> inputs = input_ids(pin == out.pins.end() ? nullptr : &pin->second);
    const record_view rv{.id = e.id, .type = e.type, .body = &e.body, .provenance_inputs = inputs};
    auto edges = edges_of(options, rv, reg);
    if (!edges) {
      return std::unexpected(std::move(edges.error()));
    }
    return out.graph.set_record(e.id, std::move(*edges));
  });
  if (!walked) {
    return std::unexpected(std::move(walked.error()));
  }
  return out;
}

result<void> impact_engine::on_commit(const commit_event &event, receipt_builder &receipt) {
  state &s = *state_;
  const std::scoped_lock lock(s.mutex);
  const auto fail = [&s](const error &e, std::string_view what) -> result<void> {
    s.stale = true;
    ++s.stats.failures;
    return std::unexpected(with_context(e, what));
  };
  auto head = s.manager->head(s.options.ref);
  if (!head) {
    return fail(head.error(), "reading the head");
  }
  if (head->revision_digest() != event.revision) {
    return {}; // a commit on another ref
  }
  std::vector<record_id> changed;
  if (s.stale || s.index.at_revision != event.revision) {
    auto view = read_revision(head->store(), event.revision);
    if (!view) {
      return fail(view.error(), "reading the revision");
    }
    std::optional<digest> parent;
    if (!view->revision.parents.empty()) {
      parent = view->revision.parents.front();
    }
    if (!s.stale && parent == s.index.at_revision) {
      auto applied = s.apply(*head, view->tx);
      if (!applied) {
        return fail(applied.error(), "applying the commit");
      }
      changed = std::move(*applied);
      ++s.stats.incremental;
    } else {
      // A jump the diff of one commit does not explain (a reset, a missed commit, an earlier
      // failure): rebuild, and report what changed since the indexed revision when it is known.
      std::optional<std::vector<record_id>> since;
      if (!s.stale) {
        auto prev = snapshot_at(head->store(), s.index.at_revision);
        if (!prev) {
          return fail(prev.error(), "opening the indexed revision");
        }
        auto deltas = prev->diff(*head);
        if (!deltas) {
          return fail(deltas.error(), "diffing the indexed revision");
        }
        since.emplace();
        for (const record_delta &d : *deltas) {
          since->push_back(d.id);
        }
      }
      auto built = rebuild(*head, s.manager->schemas(), s.options);
      if (!built) {
        return fail(built.error(), "rebuilding");
      }
      s.index = std::move(*built);
      s.stale = false;
      ++s.stats.rebuilds;
      if (since) {
        changed = std::move(*since);
      } else {
        changed = event.changed;
      }
    }
  } else {
    changed = event.changed; // a query already rebuilt over this commit
  }
  auto set = s.index.graph.reverse_closure(changed, closure_of(s.options, event.revision));
  if (!set) {
    return fail(set.error(), "computing the closure");
  }
  impact_report report = make_report(s.index, *set, s.options.summary_ids);
  auto v = to_value(report);
  if (!v) {
    return fail(v.error(), "encoding the report");
  }
  s.remember(event.tx.id(), std::move(report));
  // A name taken by another observer is that observer's report: the index itself is fine.
  return receipt.attach(s.options.attachment, std::move(*v));
}

result<impact_set> impact_engine::impact(std::span<const record_id> changed) const {
  state &s = *state_;
  const std::scoped_lock lock(s.mutex);
  if (auto ok = s.ensure_fresh(); !ok) {
    return std::unexpected(std::move(ok.error()));
  }
  return s.index.graph.reverse_closure(changed, closure_of(s.options, s.index.at_revision));
}

result<impact_report> impact_engine::report(std::span<const record_id> changed) const {
  state &s = *state_;
  const std::scoped_lock lock(s.mutex);
  if (auto ok = s.ensure_fresh(); !ok) {
    return std::unexpected(std::move(ok.error()));
  }
  auto set = s.index.graph.reverse_closure(changed, closure_of(s.options, s.index.at_revision));
  if (!set) {
    return std::unexpected(std::move(set.error()));
  }
  return make_report(s.index, *set, s.options.summary_ids);
}

result<bool> impact_engine::is_current(const record_id &derived, const snapshot &head) const {
  state &s = *state_;
  auto entry = head.entry(derived);
  if (!entry) {
    return std::unexpected(std::move(entry.error()));
  }
  if (!entry->has_value() || (*entry)->kind != entry_kind::record) {
    return make_error(errc::not_found,
                      std::format("derived record {} is not live", derived.to_string()));
  }
  auto rec = head.get(derived);
  if (!rec) {
    return std::unexpected(std::move(rec.error()));
  }
  if (!is_derived(s.manager->schemas(), rec->type)) {
    return make_error(errc::invalid_argument,
                      std::format("record {} is not of a derived type", derived.to_string()));
  }
  std::optional<derivation_pin> pin;
  {
    const std::scoped_lock lock(s.mutex);
    if (!s.stale) {
      if (const auto it = s.index.pins.find(derived);
          it != s.index.pins.end() && it->second.record_object == rec->object) {
        pin = it->second;
      }
    }
  }
  if (!pin) {
    // Not the version the index pins: resolve this one from the history of `head`.
    if (!head.revision_digest()) {
      return make_error(
          errc::invalid_argument,
          std::format("no pin of {} and the head has no revision to read history from",
                      derived.to_string()));
    }
    auto pins = resolve_pins(head, {derived});
    if (!pins) {
      return std::unexpected(std::move(pins.error()));
    }
    if (const auto it = pins->find(derived); it != pins->end()) {
      pin = std::move(it->second);
    }
  }
  if (!pin) {
    return make_error(
        errc::provenance_incomplete,
        std::format("derived record {} carries no complete provenance", derived.to_string()));
  }
  return fabric::is_current(pin->inputs, head);
}

std::optional<impact_report> impact_engine::committed(const record_id &tx) const {
  state &s = *state_;
  const std::scoped_lock lock(s.mutex);
  const auto it = s.reports.find(tx);
  if (it == s.reports.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<impact_report> impact_engine::committed(const event_envelope &e) const {
  if (!e.causality.tx) {
    return std::nullopt;
  }
  return committed(*e.causality.tx);
}

result<impact_index> impact_engine::index() const {
  state &s = *state_;
  const std::scoped_lock lock(s.mutex);
  if (auto ok = s.ensure_fresh(); !ok) {
    return std::unexpected(std::move(ok.error()));
  }
  return s.index;
}

impact_stats impact_engine::stats() const {
  state &s = *state_;
  const std::scoped_lock lock(s.mutex);
  return s.stats;
}

bool impact_engine::needs_rebuild() const {
  state &s = *state_;
  const std::scoped_lock lock(s.mutex);
  return s.stale;
}

} // namespace sl::fabric
