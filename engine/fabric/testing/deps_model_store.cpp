// The in-memory record store and random graph helpers of the dependency tests (WO-044).

#include "deps_model_store.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <set>
#include <utility>

#include "test_schemas.hpp"

namespace sl::fabric::testing {

result<void> model_store::put(const record_id &id, type_id type, value body,
                              std::vector<record_id> provenance_inputs) {
  if (!is_valid(id.cls()) || !is_valid(id.scheme())) {
    return make_error(errc::invalid_argument, "model_store::put needs a valid record id");
  }
  records_.insert_or_assign(
      id, entry{.type = type, .body = std::move(body), .inputs = std::move(provenance_inputs)});
  return {};
}

void model_store::erase(const record_id &id) { records_.erase(id); }

result<void> model_store::for_each(const visitor &fn) const {
  for (const auto &[id, e] : records_) {
    const record_view view{
        .id = id, .type = e.type, .body = &e.body, .provenance_inputs = e.inputs};
    if (auto r = fn(view); !r) {
      return r;
    }
  }
  return {};
}

std::vector<record_id> graph_ids(std::size_t count) {
  static constexpr std::array<id_class, 4> classes = {id_class::asset, id_class::entity,
                                                      id_class::instance, id_class::derived};
  std::vector<record_id> ids;
  ids.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    ids.push_back(test_id(classes.at(i % 4), std::format("graph/n{}", i)));
  }
  std::ranges::sort(ids);
  return ids;
}

std::vector<edge> random_edges(rng &r, const std::vector<record_id> &ids, std::size_t count,
                               edge_kind_mask kinds) {
  std::vector<edge_kind> allowed;
  for (std::uint8_t k = 1; k <= 5; ++k) {
    if (contains(kinds, static_cast<edge_kind>(k))) {
      allowed.push_back(static_cast<edge_kind>(k));
    }
  }
  std::set<edge> out;
  if (ids.empty() || allowed.empty()) {
    return {};
  }
  for (std::size_t i = 0; i < count; ++i) {
    const edge_kind kind = allowed[r.below(allowed.size())];
    out.insert(edge{.from = ids[r.below(ids.size())],
                    .to = ids[r.below(ids.size())],
                    .kind = kind,
                    .strength = kind == edge_kind::containment ? ref_strength::owned
                                                               : ref_strength::strong});
  }
  return {out.begin(), out.end()};
}

result<dependency_graph> graph_of(const std::vector<record_id> &ids,
                                  const std::vector<edge> &edges) {
  std::map<record_id, std::vector<edge>> by_owner;
  for (const record_id &id : ids) {
    by_owner[id];
  }
  for (const edge &e : edges) {
    by_owner[e.from].push_back(e);
  }
  dependency_graph g;
  for (auto &[id, mine] : by_owner) {
    if (auto r = g.set_record(id, std::move(mine)); !r) {
      return std::unexpected(std::move(r.error()));
    }
  }
  return g;
}

} // namespace sl::fabric::testing
