// FAB-INV-DEP-02 (SL-ARCH-001 ARCH1-R56): a containment, derives_from or builds cycle is
// dependency_cycle with the witness path; reference cycles are legal and condense.

#include "sl/fabric/deps.hpp"

#include "../../../engine/fabric/testing/crash_harness.hpp"
#include "../../../engine/fabric/testing/deps_model_store.hpp"
#include "../../../engine/fabric/testing/value_gen.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;
namespace tg = sl::fabric::testing;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

edge mk(const record_id &from, const record_id &to, edge_kind k) {
  return edge{.from = from,
              .to = to,
              .kind = k,
              .strength = k == edge_kind::containment ? ref_strength::owned : ref_strength::strong};
}

dependency_graph graph_with(const std::vector<record_id> &ids, const std::vector<edge> &edges) {
  return must(tg::graph_of(ids, edges));
}

std::string chain(const std::vector<record_id> &hops) {
  std::string out;
  for (const record_id &id : hops) {
    out += out.empty() ? "" : " -> ";
    out += id.to_string();
  }
  return out;
}

// ---- brute-force oracle over an edge list -------------------------------------------------------

using adjacency = std::vector<std::vector<bool>>;

adjacency matrix(const std::vector<record_id> &ids, const std::vector<edge> &edges,
                 edge_kind_mask mask) {
  adjacency m(ids.size(), std::vector<bool>(ids.size(), false));
  for (const edge &e : edges) {
    if (contains(mask, e.kind)) {
      const auto f = std::ranges::lower_bound(ids, e.from) - ids.begin();
      const auto t = std::ranges::lower_bound(ids, e.to) - ids.begin();
      m[static_cast<std::size_t>(f)][static_cast<std::size_t>(t)] = true;
    }
  }
  return m;
}

/// Warshall's transitive closure: reach[i][j] when a path of one or more edges leads from i to j.
adjacency reach_of(adjacency m) {
  const std::size_t n = m.size();
  for (std::size_t k = 0; k < n; ++k) {
    for (std::size_t i = 0; i < n; ++i) {
      for (std::size_t j = 0; j < n; ++j) {
        if (m[i][k] && m[k][j]) {
          m[i][j] = true;
        }
      }
    }
  }
  return m;
}

bool oracle_cyclic(const adjacency &reach) {
  for (std::size_t i = 0; i < reach.size(); ++i) {
    if (reach[i][i]) {
      return true;
    }
  }
  return false;
}

} // namespace

TEST_CASE("FAB-INV-DEP-02: a containment cycle is dependency_cycle with the witness path") {
  const auto ids = tg::graph_ids(4);
  const dependency_graph g = graph_with(ids, {mk(ids[0], ids[1], edge_kind::containment),
                                              mk(ids[1], ids[2], edge_kind::containment),
                                              mk(ids[2], ids[0], edge_kind::containment),
                                              mk(ids[2], ids[3], edge_kind::containment)});
  const auto r = g.check_acyclic();
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == sl::errc::dependency_cycle);
  CHECK(r.error().message == "containment cycle: " + chain({ids[0], ids[1], ids[2], ids[0]}));

  const auto cycle = g.find_cycle(mask_of(edge_kind::containment));
  REQUIRE(cycle.has_value());
  CHECK(cycle.value_or(std::vector<edge>{}).size() == 3);
}

TEST_CASE("FAB-INV-DEP-02: derives_from and builds cycles are refused the same way") {
  const auto ids = tg::graph_ids(3);
  for (const edge_kind k : {edge_kind::derives_from, edge_kind::builds}) {
    const dependency_graph g = graph_with(ids, {mk(ids[1], ids[2], k), mk(ids[2], ids[1], k)});
    const auto r = g.check_acyclic();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == sl::errc::dependency_cycle);
    CHECK(r.error().message ==
          std::format("{} cycle: {}", to_string(k), chain({ids[1], ids[2], ids[1]})));
  }
}

TEST_CASE("FAB-INV-DEP-02: a self loop is a cycle of one edge, except for references") {
  const auto ids = tg::graph_ids(2);
  const dependency_graph self = graph_with(ids, {mk(ids[1], ids[1], edge_kind::containment)});
  const auto r = self.check_acyclic();
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().message == "containment cycle: " + chain({ids[1], ids[1]}));

  const dependency_graph ref = graph_with(ids, {mk(ids[1], ids[1], edge_kind::reference)});
  CHECK(ref.check_acyclic().has_value());
  const condensation c = ref.condense();
  REQUIRE(c.components.size() == 2);
  CHECK(c.components[c.component_of.at(ids[1])].cyclic);
  CHECK_FALSE(c.components[c.component_of.at(ids[0])].cyclic);
}

TEST_CASE("FAB-INV-DEP-02: the witness is the shortest cycle through the smallest member") {
  // 0 -> 1 -> 2 -> 3 -> 0 (length 4) and 0 -> 4 -> 0 (length 2): the witness is the short one.
  const auto ids = tg::graph_ids(5);
  const dependency_graph g = graph_with(
      ids, {mk(ids[0], ids[1], edge_kind::builds), mk(ids[1], ids[2], edge_kind::builds),
            mk(ids[2], ids[3], edge_kind::builds), mk(ids[3], ids[0], edge_kind::builds),
            mk(ids[0], ids[4], edge_kind::builds), mk(ids[4], ids[0], edge_kind::builds)});
  const auto r = g.check_acyclic();
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().message == "builds cycle: " + chain({ids[0], ids[4], ids[0]}));
}

TEST_CASE("FAB-INV-DEP-02: each kind is judged on its own") {
  // A contains B and B derives from A: two kinds, no cycle of either.
  const auto ids = tg::graph_ids(2);
  const dependency_graph g = graph_with(ids, {mk(ids[0], ids[1], edge_kind::containment),
                                              mk(ids[1], ids[0], edge_kind::derives_from)});
  CHECK(g.check_acyclic().has_value());
  // The union is a cycle, and find_cycle over the union says so.
  CHECK(g.find_cycle(acyclic_edge_kinds).has_value());
}

TEST_CASE("FAB-INV-DEP-02: reference cycles are allowed and condense to one component") {
  const auto ids = tg::graph_ids(6);
  const dependency_graph g = graph_with(
      ids, {mk(ids[0], ids[1], edge_kind::reference), mk(ids[1], ids[2], edge_kind::reference),
            mk(ids[2], ids[0], edge_kind::reference), mk(ids[2], ids[3], edge_kind::reference),
            mk(ids[3], ids[4], edge_kind::reference), mk(ids[4], ids[3], edge_kind::reference)});
  CHECK(g.check_acyclic().has_value());
  CHECK(g.check_acyclic(all_edge_kinds).error().code == sl::errc::dependency_cycle);
  const condensation c = g.condense();
  REQUIRE(c.components.size() == 3);
  CHECK(c.components[0].members == std::vector<record_id>{ids[0], ids[1], ids[2]});
  CHECK(c.components[0].cyclic);
  CHECK(c.components[1].members == std::vector<record_id>{ids[3], ids[4]});
  CHECK(c.components[2].members == std::vector<record_id>{ids[5]});
  CHECK_FALSE(c.components[2].cyclic);
  CHECK(c.edges == std::vector<std::pair<std::size_t, std::size_t>>{{0, 1}});
  // Dependencies come first: {3,4}, then {0,1,2}; the lone node has no edge and sorts by index.
  CHECK(c.order == std::vector<std::size_t>{1, 0, 2});
}

TEST_CASE("FAB-INV-DEP-02: an acyclic graph passes and the empty graph condenses to nothing") {
  const dependency_graph empty;
  CHECK(empty.check_acyclic().has_value());
  CHECK_FALSE(empty.find_cycle(all_edge_kinds).has_value());
  const condensation c = empty.condense();
  CHECK(c.components.empty());
  CHECK(c.order.empty());
}

TEST_CASE(
    "FAB-INV-DEP-02: a chain of 20000 records neither overflows the stack nor reports a cycle") {
  const auto ids = tg::graph_ids(20000);
  std::vector<edge> edges;
  for (std::size_t i = 0; i + 1 < ids.size(); ++i) {
    edges.push_back(mk(ids[i], ids[i + 1], edge_kind::containment));
  }
  dependency_graph g = graph_with(ids, edges);
  CHECK(g.check_acyclic().has_value());
  const condensation c = g.condense();
  CHECK(c.components.size() == ids.size());
  CHECK(c.order.size() == ids.size());
  // Close the chain: the witness is the whole ring starting at the smallest id.
  REQUIRE(
      g.set_record(ids.back(), {mk(ids.back(), ids.front(), edge_kind::containment)}).has_value());
  const auto cycle = g.find_cycle(mask_of(edge_kind::containment));
  REQUIRE(cycle.has_value());
  const std::vector<edge> ring = cycle.value_or(std::vector<edge>{});
  CHECK(ring.size() == ids.size());
  CHECK(ring.front().from == ids.front());
}

TEST_CASE("FAB-INV-DEP-02: 300 seeded graphs (30 under the sanitizers) agree with a "
          "transitive-closure oracle") {
  // SL_FABRIC_SWEEP_SCALE (0.1 under the sanitizer presets) keeps the unit budget (WO-064).
  const std::uint64_t seeds = tg::scaled(300, 30);
  for (std::uint64_t seed = 0; seed < seeds; ++seed) {
    tg::rng r((seed * 7919) + 1);
    const auto ids = tg::graph_ids(2 + r.below(13));
    const std::size_t count = r.below((ids.size() * 2) + 1);
    const std::vector<edge> edges = tg::random_edges(r, ids, count, all_edge_kinds);
    const dependency_graph g = graph_with(ids, edges);

    for (std::uint32_t mask = 1; mask <= all_edge_kinds;
         mask += static_cast<std::uint32_t>(seed % 5) + 1U) {
      const adjacency reach = reach_of(matrix(ids, edges, mask));
      const auto cycle = g.find_cycle(mask);
      CAPTURE(seed);
      CAPTURE(mask);
      CHECK(cycle.has_value() == oracle_cyclic(reach));
      if (cycle) {
        // A real, closed path of masked edges that starts at its smallest id.
        const std::set<edge> have(edges.begin(), edges.end());
        record_id smallest = cycle->front().from;
        for (std::size_t i = 0; i < cycle->size(); ++i) {
          const edge &e = (*cycle)[i];
          CHECK(have.contains(e));
          CHECK(contains(mask, e.kind));
          CHECK(e.to == (*cycle)[(i + 1) % cycle->size()].from);
          smallest = std::min(smallest, e.from);
        }
        CHECK(cycle->front().from == smallest);
      }
    }

    // The default check names the first cyclic kind among containment, derives_from, builds.
    std::optional<edge_kind> first_bad;
    for (const edge_kind k : {edge_kind::containment, edge_kind::derives_from, edge_kind::builds}) {
      if (!first_bad && oracle_cyclic(reach_of(matrix(ids, edges, mask_of(k))))) {
        first_bad = k;
      }
    }
    const auto verdict = g.check_acyclic();
    CHECK(verdict.has_value() == !first_bad.has_value());
    if (first_bad && !verdict) {
      CHECK(verdict.error().code == sl::errc::dependency_cycle);
      CHECK(verdict.error().message.starts_with(std::format("{} cycle: ", to_string(*first_bad))));
    }

    // Condensation: same component iff mutual reachability; a DAG; dependencies first.
    const adjacency reach = reach_of(matrix(ids, edges, all_edge_kinds));
    const condensation c = g.condense();
    std::size_t counted = 0;
    for (const auto &comp : c.components) {
      counted += comp.members.size();
      CHECK(std::ranges::is_sorted(comp.members));
      const auto first = static_cast<std::size_t>(
          std::ranges::lower_bound(ids, comp.members.front()) - ids.begin());
      CHECK(comp.cyclic == (comp.members.size() > 1 || reach[first][first]));
    }
    CHECK(counted == ids.size());
    CHECK(std::ranges::is_sorted(c.components, {},
                                 [](const scc_component &s) { return s.members.front(); }));
    for (std::size_t i = 0; i < ids.size(); ++i) {
      for (std::size_t j = 0; j < ids.size(); ++j) {
        const bool same = i == j || (reach[i][j] && reach[j][i]);
        CHECK(same == (c.component_of.at(ids[i]) == c.component_of.at(ids[j])));
      }
    }
    std::vector<std::size_t> position(c.components.size());
    REQUIRE(c.order.size() == c.components.size());
    for (std::size_t i = 0; i < c.order.size(); ++i) {
      position[c.order[i]] = i;
    }
    std::set<std::pair<std::size_t, std::size_t>> dag;
    for (const edge &e : edges) {
      const std::size_t a = c.component_of.at(e.from);
      const std::size_t b = c.component_of.at(e.to);
      if (a != b) {
        dag.emplace(a, b);
        CHECK(position[b] < position[a]);
      }
    }
    CHECK(std::set<std::pair<std::size_t, std::size_t>>(c.edges.begin(), c.edges.end()) == dag);
    CHECK(c.edges.size() == dag.size());
  }
}

TEST_CASE("FAB-INV-DEP-02: the witness does not depend on the order the graph was built in") {
  for (std::uint64_t seed = 0; seed < 40; ++seed) {
    tg::rng r(seed + 5000);
    const auto ids = tg::graph_ids(8);
    std::vector<edge> edges = tg::random_edges(r, ids, 14, mask_of(edge_kind::builds));
    const dependency_graph a = graph_with(ids, edges);
    std::ranges::reverse(edges);
    std::vector<record_id> backwards = ids;
    std::ranges::reverse(backwards);
    const dependency_graph b = graph_with(backwards, edges);
    CHECK(a == b);
    CHECK(a.find_cycle(all_edge_kinds) == b.find_cycle(all_edge_kinds));
    CHECK(a.condense().order == b.condense().order);
  }
}
