// FAB-INV-DEP-03 (SL-ARCH-001 ARCH1-R58): the reverse closure equals a brute-force oracle and is
// ordered by distance, then id; the summary groups the affected ids by class.

#include "sl/fabric/deps.hpp"

#include "../../../engine/fabric/testing/deps_model_store.hpp"
#include "../../../engine/fabric/testing/test_schemas.hpp"
#include "../../../engine/fabric/testing/value_gen.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
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
  return edge{.from = from, .to = to, .kind = k, .strength = ref_strength::strong};
}

closure_options with_kinds(edge_kind_mask kinds) {
  closure_options o;
  o.kinds = kinds;
  return o;
}

closure_options with_distance(std::uint32_t d) {
  closure_options o;
  o.max_distance = d;
  return o;
}

closure_options with_nodes(std::size_t n, std::optional<digest> rev = std::nullopt) {
  closure_options o;
  o.max_nodes = n;
  o.at_revision = rev;
  return o;
}

/// The oracle: distances by relaxing over the whole edge list until nothing changes (no queue, no
/// adjacency, none of the production data structures).
impact_set oracle(const std::vector<edge> &edges, const std::vector<record_id> &changed,
                  const closure_options &opt) {
  std::map<record_id, std::uint32_t> dist;
  for (const record_id &c : changed) {
    dist[c] = 0;
  }
  for (bool moved = true; moved;) {
    moved = false;
    for (const edge &e : edges) {
      if (!contains(opt.kinds, e.kind)) {
        continue;
      }
      const auto to = dist.find(e.to);
      if (to == dist.end()) {
        continue;
      }
      const auto from = dist.find(e.from);
      if (from == dist.end() || from->second > to->second + 1) {
        dist[e.from] = to->second + 1;
        moved = true;
      }
    }
  }
  std::vector<impact_node> nodes;
  for (const auto &[id, d] : dist) {
    if (opt.max_distance && d > *opt.max_distance) {
      continue;
    }
    impact_node n{
        .id = id, .cls = id.cls(), .distance = d, .via = {}, .via_kind = edge_kind::reference};
    if (d > 0) {
      std::optional<std::pair<record_id, edge_kind>> best;
      for (const edge &e : edges) {
        const auto to = dist.find(e.to);
        if (e.from == id && contains(opt.kinds, e.kind) && to != dist.end() &&
            to->second == d - 1) {
          const std::pair<record_id, edge_kind> cand{e.to, e.kind};
          if (!best || cand < *best) {
            best = cand;
          }
        }
      }
      if (best) {
        n.via = best->first;
        n.via_kind = best->second;
      }
    }
    nodes.push_back(n);
  }
  std::ranges::sort(nodes, [](const impact_node &a, const impact_node &b) {
    return a.distance != b.distance ? a.distance < b.distance : a.id < b.id;
  });
  impact_set out;
  out.at_revision = opt.at_revision;
  if (opt.max_nodes != 0 && nodes.size() > opt.max_nodes) {
    nodes.resize(opt.max_nodes);
    out.truncated = true;
  }
  out.nodes = std::move(nodes);
  return out;
}

} // namespace

TEST_CASE("FAB-INV-DEP-03: a small closure is ordered by distance then id, with its via") {
  // 0 <- 1 <- 3, 0 <- 2 <- 3, 4 <- 3; changing 0 reaches 1, 2 at distance 1, 3 at 2, 4 at 3.
  const auto ids = tg::graph_ids(6);
  const dependency_graph g = must(tg::graph_of(
      ids, {mk(ids[1], ids[0], edge_kind::reference), mk(ids[2], ids[0], edge_kind::derives_from),
            mk(ids[3], ids[1], edge_kind::containment), mk(ids[3], ids[2], edge_kind::reference),
            mk(ids[4], ids[3], edge_kind::builds)}));
  const std::vector<record_id> changed = {ids[0]};
  const impact_set s = must(g.reverse_closure(changed));
  REQUIRE(s.nodes.size() == 5);
  CHECK(s.nodes[0] == impact_node{ids[0], ids[0].cls(), 0, record_id{}, edge_kind::reference});
  CHECK(s.nodes[1] == impact_node{ids[1], ids[1].cls(), 1, ids[0], edge_kind::reference});
  CHECK(s.nodes[2] == impact_node{ids[2], ids[2].cls(), 1, ids[0], edge_kind::derives_from});
  // 3 is reached through 1 (containment) and 2 (reference): the smaller via wins.
  CHECK(s.nodes[3] == impact_node{ids[3], ids[3].cls(), 2, ids[1], edge_kind::containment});
  CHECK(s.nodes[4] == impact_node{ids[4], ids[4].cls(), 3, ids[3], edge_kind::builds});
  CHECK_FALSE(s.truncated);
  CHECK_FALSE(s.at_revision.has_value());
  CHECK(s.visited == 5);
  CHECK(s.edges_examined == 5);
  // ids[5] is unrelated.
  CHECK(std::ranges::none_of(s.nodes, [&](const impact_node &n) { return n.id == ids[5]; }));
}

TEST_CASE("FAB-INV-DEP-03: cycles, unknown ids, duplicates, options and bad input") {
  const auto ids = tg::graph_ids(4);
  const dependency_graph g = must(tg::graph_of(ids, {mk(ids[0], ids[1], edge_kind::reference),
                                                     mk(ids[1], ids[0], edge_kind::reference),
                                                     mk(ids[2], ids[1], edge_kind::validates)}));
  // A reference cycle terminates: changing 0 reaches 1 and then 2.
  const impact_set cyc = must(g.reverse_closure(std::vector<record_id>{ids[0], ids[0]}));
  CHECK(cyc.nodes.size() == 3);

  // An id the graph does not know is still in the set; its dependents are not.
  const record_id gone = tg::test_id(id_class::entity, "closure/gone");
  const impact_set lone = must(g.reverse_closure(std::vector<record_id>{gone}));
  REQUIRE(lone.nodes.size() == 1);
  CHECK(lone.nodes[0].id == gone);

  // Kind filter: only reference edges.
  const impact_set refs = must(
      g.reverse_closure(std::vector<record_id>{ids[1]}, with_kinds(mask_of(edge_kind::reference))));
  CHECK(refs.nodes.size() == 2); // 1 and 0, not 2
  // Distance bound.
  const impact_set d0 = must(g.reverse_closure(std::vector<record_id>{ids[1]}, with_distance(0)));
  CHECK(d0.nodes.size() == 1);
  CHECK(d0.visited == 0);
  const impact_set d1 = must(g.reverse_closure(std::vector<record_id>{ids[1]}, with_distance(1)));
  CHECK(d1.nodes.size() == 3);
  // Node bound and the revision pass-through.
  digest rev;
  rev.bytes[0] = 0x42;
  const impact_set capped =
      must(g.reverse_closure(std::vector<record_id>{ids[1]}, with_nodes(2, rev)));
  CHECK(capped.nodes.size() == 2);
  CHECK(capped.truncated);
  CHECK(capped.at_revision == rev);
  const impact_set exact = must(g.reverse_closure(std::vector<record_id>{ids[1]}, with_nodes(3)));
  CHECK(exact.nodes.size() == 3);
  CHECK_FALSE(exact.truncated);

  // The empty change set and invalid ids.
  CHECK(must(g.reverse_closure({})).nodes.empty());
  const auto null = g.reverse_closure(std::vector<record_id>{record_id{}});
  REQUIRE_FALSE(null.has_value());
  CHECK(null.error().code == sl::errc::invalid_argument);
}

TEST_CASE("FAB-INV-DEP-03: 300 seeded graphs: the closure equals a brute-force oracle") {
  for (std::uint64_t seed = 0; seed < 300; ++seed) {
    tg::rng r((seed * 104729) + 3);
    const auto ids = tg::graph_ids(3 + r.below(18));
    const std::vector<edge> edges =
        tg::random_edges(r, ids, r.below((ids.size() * 3) + 1), all_edge_kinds);
    const dependency_graph g = must(tg::graph_of(ids, edges));

    std::vector<record_id> changed;
    for (std::uint64_t i = 1 + r.below(3); i > 0; --i) {
      changed.push_back(r.chance(10) ? tg::test_id(id_class::entity, "seeded/outside")
                                     : ids[r.below(ids.size())]);
    }
    closure_options opt;
    opt.kinds = static_cast<edge_kind_mask>(1 + r.below(all_edge_kinds));
    if (r.chance(30)) {
      opt.max_distance = static_cast<std::uint32_t>(r.below(4));
    }
    if (r.chance(30)) {
      opt.max_nodes = 1 + r.below(8);
    }
    if (r.chance(50)) {
      digest d;
      d.bytes[0] = static_cast<std::uint8_t>(seed);
      opt.at_revision = d;
    }

    std::vector<record_id> unique = changed;
    std::ranges::sort(unique);
    auto [dup_first, dup_last] = std::ranges::unique(unique);
    unique.erase(dup_first, dup_last);
    const impact_set want = oracle(edges, unique, opt);
    const impact_set got = must(g.reverse_closure(changed, opt));
    CAPTURE(seed);
    CHECK(got.nodes == want.nodes);
    CHECK(got.truncated == want.truncated);
    CHECK(got.at_revision == want.at_revision);

    // Ordered by (distance, id).
    CHECK(std::ranges::is_sorted(got.nodes, [](const impact_node &a, const impact_node &b) {
      return a.distance != b.distance ? a.distance < b.distance : a.id < b.id;
    }));
    // Work is bounded by the closure: every edge looked at ends in a node whose dependents we
    // expanded, and every expanded node is in the set.
    CHECK(got.visited <= got.nodes.size());
    std::set<record_id> in_set;
    for (const impact_node &n : got.nodes) {
      in_set.insert(n.id);
    }
    std::size_t bound = 0;
    for (const edge &e : edges) {
      bound += (contains(opt.kinds, e.kind) && in_set.contains(e.to)) ? 1U : 0U;
    }
    CHECK(got.edges_examined <= bound);
  }
}

TEST_CASE("FAB-INV-DEP-03: incremental updates give the graph and the closure of a rebuild") {
  for (std::uint64_t seed = 0; seed < 60; ++seed) {
    tg::rng r(seed + 91);
    const auto ids = tg::graph_ids(12);
    std::vector<edge> now = tg::random_edges(r, ids, 20, all_edge_kinds);
    dependency_graph incremental = must(tg::graph_of(ids, now));
    std::set<record_id> present(ids.begin(), ids.end());
    // Rewrite or erase one owner at a time.
    for (int step = 0; step < 6; ++step) {
      const record_id owner = ids[r.below(ids.size())];
      std::erase_if(now, [&](const edge &e) { return e.from == owner; });
      if (r.chance(30)) {
        incremental.erase_record(owner);
        present.erase(owner);
      } else {
        std::vector<edge> fresh = tg::random_edges(r, ids, 3, all_edge_kinds);
        std::erase_if(fresh, [&](const edge &e) { return e.from != owner; });
        now.insert(now.end(), fresh.begin(), fresh.end());
        REQUIRE(incremental.set_record(owner, fresh).has_value());
        present.insert(owner);
      }
    }
    const dependency_graph rebuilt =
        must(tg::graph_of(std::vector<record_id>(present.begin(), present.end()), now));
    CAPTURE(seed);
    CHECK(incremental == rebuilt);
    CHECK(incremental.edges() == rebuilt.edges());
    const std::vector<record_id> changed = {ids[r.below(ids.size())]};
    CHECK(must(incremental.reverse_closure(changed)) == must(rebuilt.reverse_closure(changed)));
    CHECK(incremental.condense().components == rebuilt.condense().components);
  }
}

TEST_CASE("FAB-INV-DEP-03: the summary groups the affected ids by class, in set order") {
  const auto ids = tg::graph_ids(9); // classes cycle asset, entity, instance, derived
  std::vector<edge> edges;
  for (std::size_t i = 1; i < ids.size(); ++i) {
    edges.push_back(mk(ids[i], ids[0], edge_kind::reference));
  }
  const dependency_graph g = must(tg::graph_of(ids, edges));
  const impact_set s = must(g.reverse_closure(std::vector<record_id>{ids[0]}));
  const impact_summary all = summarize(s, 64);
  CHECK(all.total == 9);
  CHECK_FALSE(all.truncated);
  std::map<id_class, std::uint64_t> counts;
  for (const record_id &id : ids) {
    ++counts[id.cls()];
  }
  REQUIRE(all.classes.size() == counts.size());
  id_class previous = id_class::none;
  for (const class_impact &c : all.classes) {
    CHECK(c.cls > previous);
    previous = c.cls;
    CHECK(c.count == counts[c.cls]);
    CHECK(c.first.size() == c.count);
    // First ids come in set order: the changed record first, then ascending id (all distance 1).
    CHECK(std::ranges::is_sorted(c.first));
  }
  const impact_summary two = summarize(s, 2);
  for (const class_impact &c : two.classes) {
    CHECK(c.count == counts[c.cls]);
    CHECK(c.first.size() == std::min<std::size_t>(2, c.count));
  }
  CHECK(two.total == 9);
  const impact_summary none = summarize(impact_set{}, 4);
  CHECK(none.classes.empty());
  CHECK(none.total == 0);

  impact_set cut = s;
  cut.truncated = true;
  CHECK(summarize(cut).truncated);
}
