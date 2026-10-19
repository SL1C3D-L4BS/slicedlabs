// The dependency graph store (WO-044): build from a record_enumerator, owner-keyed contributions
// with reference counts, sorted adjacency in both directions.

#include <algorithm>
#include <format>
#include <iterator>
#include <utility>
#include <vector>

#include "sl/fabric/deps.hpp"

namespace sl::fabric {

namespace {

bool valid_id(const record_id &id) noexcept { return is_valid(id.cls()) && is_valid(id.scheme()); }

bool valid_strength(ref_strength s) noexcept {
  return s == ref_strength::weak || s == ref_strength::strong || s == ref_strength::owned;
}

} // namespace

bool operator==(const dependency_graph &a, const dependency_graph &b) noexcept {
  return a.nodes_ == b.nodes_;
}

void dependency_graph::acquire(const edge &e) {
  auto [it, fresh] = counts_.try_emplace(e, 0);
  ++it->second;
  if (!fresh) {
    return;
  }
  auto &out = nodes_[e.from].out;
  out.insert(std::ranges::lower_bound(out, e), e);
  auto &in = nodes_[e.to].in;
  in.insert(std::ranges::lower_bound(in, e), e);
}

void dependency_graph::release(const edge &e) {
  const auto it = counts_.find(e);
  if (it == counts_.end()) {
    return;
  }
  if (--it->second > 0) {
    return;
  }
  counts_.erase(it);
  auto &out = nodes_[e.from].out;
  out.erase(std::ranges::lower_bound(out, e));
  auto &in = nodes_[e.to].in;
  in.erase(std::ranges::lower_bound(in, e));
}

void dependency_graph::drop_if_unused(const record_id &id) {
  const auto it = nodes_.find(id);
  if (it != nodes_.end() && !it->second.present && it->second.out.empty() &&
      it->second.in.empty()) {
    nodes_.erase(it);
  }
}

result<void> dependency_graph::set_record(const record_id &owner, std::vector<edge> edges) {
  if (!valid_id(owner)) {
    return make_error(errc::invalid_argument, "set_record needs a valid owner id");
  }
  for (const edge &e : edges) {
    if (!valid_id(e.from) || !valid_id(e.to) || !is_valid(e.kind) || !valid_strength(e.strength)) {
      return make_error(errc::invalid_argument,
                        std::format("invalid edge {} -> {} from owner {}", e.from.to_string(),
                                    e.to.to_string(), owner.to_string()));
    }
  }
  std::ranges::sort(edges);
  auto [dup_first, dup_last] = std::ranges::unique(edges);
  edges.erase(dup_first, dup_last);

  std::vector<edge> old = std::move(owners_[owner]);
  std::vector<edge> dropped;
  std::vector<edge> added;
  std::ranges::set_difference(old, edges, std::back_inserter(dropped));
  std::ranges::set_difference(edges, old, std::back_inserter(added));
  for (const edge &e : added) {
    acquire(e);
  }
  for (const edge &e : dropped) {
    release(e);
  }
  nodes_[owner].present = true;
  for (const edge &e : dropped) {
    drop_if_unused(e.from);
    drop_if_unused(e.to);
  }
  owners_[owner] = std::move(edges);
  return {};
}

void dependency_graph::erase_record(const record_id &owner) {
  const auto it = owners_.find(owner);
  if (it != owners_.end()) {
    const std::vector<edge> old = std::move(it->second);
    owners_.erase(it);
    for (const edge &e : old) {
      release(e);
    }
    for (const edge &e : old) {
      drop_if_unused(e.from);
      drop_if_unused(e.to);
    }
  }
  const auto n = nodes_.find(owner);
  if (n != nodes_.end()) {
    n->second.present = false;
    drop_if_unused(owner);
  }
}

bool dependency_graph::contains_record(const record_id &id) const noexcept {
  const auto it = nodes_.find(id);
  return it != nodes_.end() && it->second.present;
}

std::vector<record_id> dependency_graph::nodes() const {
  std::vector<record_id> ids;
  ids.reserve(nodes_.size());
  for (const auto &[id, n] : nodes_) {
    ids.push_back(id);
  }
  return ids;
}

std::vector<edge> dependency_graph::edges() const {
  std::vector<edge> all;
  all.reserve(counts_.size());
  for (const auto &[e, count] : counts_) {
    all.push_back(e);
  }
  return all;
}

std::span<const edge> dependency_graph::out(const record_id &id) const noexcept {
  const auto it = nodes_.find(id);
  return it == nodes_.end() ? std::span<const edge>{} : std::span<const edge>(it->second.out);
}

std::span<const edge> dependency_graph::in(const record_id &id) const noexcept {
  const auto it = nodes_.find(id);
  return it == nodes_.end() ? std::span<const edge>{} : std::span<const edge>(it->second.in);
}

result<dependency_graph> dependency_graph::build(const record_enumerator &records,
                                                 const schema_registry &reg) {
  std::vector<std::pair<record_id, std::vector<edge>>> collected;
  const auto visit = [&](const record_view &rv) -> result<void> {
    if (rv.body == nullptr) {
      return make_error(errc::invalid_argument,
                        std::format("record {} has no body", rv.id.to_string()));
    }
    const type_desc *t = reg.find(rv.type);
    if (t == nullptr) {
      return make_error(errc::schema_unknown, std::format("record {} has the unregistered type {}",
                                                          rv.id.to_string(), rv.type.to_string()));
    }
    auto edges = extract_edges(rv.id, *rv.body, *t, reg);
    if (!edges) {
      return std::unexpected(std::move(edges.error()));
    }
    if (!rv.provenance_inputs.empty()) {
      if (t->state_class != state_class::derived) {
        return make_error(errc::invalid_argument,
                          std::format("record {} has provenance inputs but type {} is not derived",
                                      rv.id.to_string(), t->name));
      }
      std::vector<edge> prov = provenance_edges(rv.id, rv.provenance_inputs);
      edges->insert(edges->end(), prov.begin(), prov.end());
    }
    if (t->domain == "fabric.edge") {
      const auto named = edge_from_record(*rv.body);
      if (!named) {
        return std::unexpected(named.error());
      }
      edges->push_back(*named);
    }
    collected.emplace_back(rv.id, std::move(*edges));
    return {};
  };
  if (auto r = records.for_each(visit); !r) {
    return std::unexpected(std::move(r.error()));
  }
  std::ranges::sort(collected, {}, &std::pair<record_id, std::vector<edge>>::first);
  for (std::size_t i = 1; i < collected.size(); ++i) {
    if (collected[i].first == collected[i - 1].first) {
      return make_error(errc::already_exists, std::format("record {} is enumerated twice",
                                                          collected[i].first.to_string()));
    }
  }
  dependency_graph g;
  for (auto &[id, edges] : collected) {
    if (auto r = g.set_record(id, std::move(edges)); !r) {
      return std::unexpected(std::move(r.error()));
    }
  }
  return g;
}

} // namespace sl::fabric
