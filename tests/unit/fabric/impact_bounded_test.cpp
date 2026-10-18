// FAB-INV-IMP-02 (SL-ARCH-001 ARCH1-R58; master plan section 2.4): visited nodes never exceed the
// closure plus its frontier edges. Over seeded random graphs of asset, entity, instance and
// derived records (committed through a transaction manager, read by an attached impact_engine),
// every impact set is ordered by (distance, id) without duplicates, expands no node it does not
// return (visited <= nodes), examines no edge that does not end in a returned node, honours
// max_distance and max_nodes (truncated exactly when the cut drops nodes, and then a prefix of
// the unbounded set), equals a brute-force reverse search, and is the same set whatever order
// the records were committed in. Reports group the set by id class with counts and at most
// summary_ids ids per class.

#include "sl/fabric/deps.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/impact.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/test_schemas.hpp"
#include "../../../engine/fabric/testing/tx_fixture.hpp"
#include "../../../engine/fabric/testing/undo_fixture.hpp"
#include "../../../engine/fabric/testing/value_gen.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;
namespace tg = sl::fabric::testing;
using sl::errc;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

constexpr std::array<id_class, 4> classes{id_class::asset, id_class::entity, id_class::instance,
                                          id_class::derived};

id_class class_at(std::size_t i) {
  switch (i % 4) {
  case 0:
    return id_class::asset;
  case 1:
    return id_class::entity;
  case 2:
    return id_class::instance;
  default:
    return id_class::derived;
  }
}

// Field 1 of a body is the list of ids the record depends on.
sl::result<std::vector<edge>> list_edges(const record_view &rv, const schema_registry & /*reg*/) {
  std::vector<edge> out;
  if (const value *deps = rv.body->find_field(1); deps != nullptr) {
    for (const value &item : deps->items()) {
      auto to = item.as_record_id();
      if (!to) {
        return std::unexpected(to.error());
      }
      out.push_back(edge{.from = rv.id, .to = *to, .kind = edge_kind::reference});
    }
  }
  return out;
}

value body_of(const std::vector<record_id> &deps) {
  std::vector<value> items;
  items.reserve(deps.size());
  for (const record_id &d : deps) {
    items.push_back(value::id(d));
  }
  return must(value::struct_({field{.no = 1, .v = must(value::list(std::move(items)))}}));
}

struct graph_spec {
  std::vector<record_id> ids;
  std::map<record_id, std::vector<record_id>> deps;
};

graph_spec random_graph(tg::rng &r, std::size_t n, std::size_t edges) {
  graph_spec g;
  for (std::size_t i = 0; i < n; ++i) {
    g.ids.push_back(tg::test_id(class_at(i), "n" + std::to_string(i)));
  }
  for (std::size_t e = 0; e < edges; ++e) {
    const record_id &from = g.ids[r.below(n)];
    const record_id &to = g.ids[r.below(n)];
    std::vector<record_id> &d = g.deps[from];
    if (std::ranges::find(d, to) == d.end()) {
      d.push_back(to);
    }
  }
  return g;
}

// One record type per id class (the identity validator checks the record class).
type_id type_for(id_class c) { return type_id_of("imp.rec." + std::string(to_string(c))); }

sl::result<workspace_options> graph_options() {
  auto o = tg::gen_options(std::make_shared<manual_clock>(1), 9);
  if (!o) {
    return o;
  }
  for (const id_class c : classes) {
    type_desc t;
    t.name = "imp.rec." + std::string(to_string(c));
    t.id = type_id_of(t.name);
    t.kind = type_kind::struct_;
    t.domain = "gen";
    t.record_class = c;
    t.state_class = state_class::document;
    if (auto ok = o->schemas.add(std::move(t)); !ok) {
      return std::unexpected(ok.error());
    }
  }
  return o;
}

struct env {
  std::unique_ptr<workspace> ws;
};

// Commits the graph in `parts` transactions, the records in `ids` order.
env commit_graph(const graph_spec &g, const std::vector<record_id> &order, std::size_t parts) {
  env out{.ws = must(workspace::create_memory(must(graph_options())))};
  const tx_origin origin = tg::tx_origin_of(tg::tx_editor());
  const std::size_t per = (order.size() + parts - 1) / parts;
  for (std::size_t start = 0; start < order.size(); start += per) {
    std::vector<patch_op> ops;
    for (std::size_t i = start; i < std::min(order.size(), start + per); ++i) {
      const auto it = g.deps.find(order[i]);
      ops.push_back(op_create(order[i], type_for(order[i].cls()),
                              body_of(it == g.deps.end() ? std::vector<record_id>{} : it->second)));
    }
    must(tg::commit_ops(out.ws->manager(), origin, ops));
  }
  return out;
}

// Brute force: the distance of every record that reaches a changed record.
std::map<record_id, std::uint32_t> brute_closure(const graph_spec &g,
                                                 const std::vector<record_id> &changed) {
  std::map<record_id, std::uint32_t> dist;
  std::deque<record_id> queue;
  for (const record_id &c : changed) {
    if (dist.emplace(c, 0).second) {
      queue.push_back(c);
    }
  }
  while (!queue.empty()) {
    const record_id cur = queue.front();
    queue.pop_front();
    for (const auto &[from, deps] : g.deps) {
      if (std::ranges::find(deps, cur) != deps.end() && !dist.contains(from)) {
        dist.emplace(from, dist.at(cur) + 1);
        queue.push_back(from);
      }
    }
  }
  return dist;
}

void check_shape(const impact_set &s, const dependency_graph &graph) {
  std::set<record_id> seen;
  std::size_t frontier_edges = 0;
  for (std::size_t i = 0; i < s.nodes.size(); ++i) {
    const impact_node &n = s.nodes[i];
    CHECK(seen.insert(n.id).second);
    CHECK(n.cls == n.id.cls());
    if (i > 0) {
      const impact_node &p = s.nodes[i - 1];
      CHECK((p.distance < n.distance || (p.distance == n.distance && p.id < n.id)));
    }
    frontier_edges += graph.in(n.id).size();
  }
  // The ARCH1-R58 bound: closure plus frontier edges.
  CHECK(s.visited <= s.nodes.size());
  CHECK(s.edges_examined <= frontier_edges);
}

void check_report(const impact_report &r, const impact_set &s, std::size_t summary_ids) {
  CHECK(r.summary.total == s.nodes.size());
  CHECK(r.summary.truncated == s.truncated);
  CHECK(r.visited == s.visited);
  CHECK(r.edges_examined == s.edges_examined);
  std::uint64_t sum = 0;
  for (std::size_t i = 0; i < r.summary.classes.size(); ++i) {
    const class_impact &c = r.summary.classes[i];
    if (i > 0) {
      CHECK(r.summary.classes[i - 1].cls < c.cls);
    }
    std::vector<record_id> expect;
    std::uint64_t count = 0;
    for (const impact_node &n : s.nodes) {
      if (n.cls == c.cls) {
        ++count;
        if (expect.size() < summary_ids) {
          expect.push_back(n.id);
        }
      }
    }
    CHECK(c.count == count);
    CHECK(c.first == expect);
    sum += c.count;
  }
  CHECK(sum == r.summary.total);
}

} // namespace

TEST_CASE("FAB-INV-IMP-02: visited nodes never exceed closure plus frontier edges") {
  const std::size_t seeds = tg::sweep_scaled(6);
  std::size_t sets = 0;
  for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
    CAPTURE(seed);
    tg::rng r(seed * 2654435761U);
    const std::size_t n = 24 + static_cast<std::size_t>(r.below(24));
    const graph_spec g = random_graph(r, n, n + static_cast<std::size_t>(r.below(2 * n)));
    env e = commit_graph(g, g.ids, 1);
    transaction_manager &m = e.ws->manager();

    // The same records committed in reverse order over three transactions.
    std::vector<record_id> reversed = g.ids;
    std::ranges::reverse(reversed);
    env other = commit_graph(g, reversed, 3);

    impact_options base;
    base.edges = list_edges;
    auto unbounded = must(impact_engine::attach(m, must(m.head()), base));
    auto mirror =
        must(impact_engine::attach(other.ws->manager(), must(other.ws->manager().head()), base));
    std::vector<std::pair<impact_options, std::shared_ptr<impact_engine>>> bounded;
    for (const std::uint32_t d : {0U, 1U, 2U}) {
      impact_options o = base;
      o.max_distance = d;
      o.attachment = "test.impact.d" + std::to_string(d);
      bounded.emplace_back(o, must(impact_engine::attach(m, must(m.head()), o)));
    }
    for (const std::size_t cap : {std::size_t{1}, std::size_t{5}, std::size_t{17}}) {
      impact_options o = base;
      o.max_nodes = cap;
      o.attachment = "test.impact.n" + std::to_string(cap);
      bounded.emplace_back(o, must(impact_engine::attach(m, must(m.head()), o)));
    }
    const dependency_graph graph = must(unbounded->index()).graph;
    CHECK(graph == must(mirror->index()).graph);

    for (int round = 0; round < 25; ++round) {
      CAPTURE(round);
      std::vector<record_id> changed;
      const std::size_t k = 1 + static_cast<std::size_t>(r.below(3));
      changed.reserve(k + 1);
      for (std::size_t i = 0; i < k; ++i) {
        changed.push_back(g.ids[r.below(n)]);
      }
      if (r.chance(20)) {
        changed.push_back(tg::test_id(id_class::asset, "absent")); // a deleted record
      }
      const impact_set full = must(unbounded->impact(changed));
      check_shape(full, graph);
      CHECK_FALSE(full.truncated);
      CHECK(full.at_revision == must(m.head()).revision_digest());

      // Equal to a brute-force reverse search, and deterministic across commit orders.
      const auto brute = brute_closure(g, changed);
      REQUIRE(full.nodes.size() == brute.size());
      for (const impact_node &node : full.nodes) {
        CHECK(brute.at(node.id) == node.distance);
      }
      impact_set same = must(mirror->impact(changed));
      same.at_revision = full.at_revision;
      CHECK(same == full);
      CHECK(must(unbounded->impact(changed)) == full);
      check_report(must(unbounded->report(changed)), full, default_summary_ids);

      for (const auto &[o, engine] : bounded) {
        const impact_set s = must(engine->impact(changed));
        check_shape(s, graph);
        if (o.max_distance) {
          std::vector<impact_node> expect;
          for (const impact_node &node : full.nodes) {
            if (node.distance <= *o.max_distance) {
              expect.push_back(node);
            }
          }
          CHECK(s.nodes == expect);
          CHECK_FALSE(s.truncated);
        } else {
          CHECK(s.truncated == (full.nodes.size() > o.max_nodes));
          CHECK(s.nodes.size() == std::min(full.nodes.size(), o.max_nodes));
          for (std::size_t i = 0; i < s.nodes.size(); ++i) {
            CHECK(s.nodes[i].id == full.nodes[i].id);
            CHECK(s.nodes[i].distance == full.nodes[i].distance);
          }
        }
        check_report(must(engine->report(changed)), s, default_summary_ids);
      }
      ++sets;
    }
  }
  CHECK(sets == seeds * 25);
}

TEST_CASE("FAB-INV-IMP-02: reports list at most summary_ids ids per class, with exact counts") {
  graph_spec g;
  const record_id hub = tg::test_id(id_class::asset, "hub");
  g.ids.push_back(hub);
  for (int i = 0; i < 150; ++i) {
    const record_id id =
        tg::test_id(i % 2 == 0 ? id_class::entity : id_class::instance, "leaf" + std::to_string(i));
    g.ids.push_back(id);
    g.deps[id] = {hub};
  }
  env e = commit_graph(g, g.ids, 2);
  transaction_manager &m = e.ws->manager();
  impact_options o;
  o.edges = list_edges;
  auto engine = must(impact_engine::attach(m, must(m.head()), o));
  const std::vector<record_id> changed{hub};
  const impact_set s = must(engine->impact(changed));
  REQUIRE(s.nodes.size() == 151);
  const impact_report r = must(engine->report(changed));
  check_report(r, s, default_summary_ids);
  REQUIRE(r.summary.classes.size() == 3);
  CHECK(r.summary.classes[0].cls == id_class::asset);
  CHECK(r.summary.classes[1].cls == id_class::entity);
  CHECK(r.summary.classes[1].count == 75);
  CHECK(r.summary.classes[1].first.size() == default_summary_ids);
  CHECK(r.summary.classes[2].cls == id_class::instance);
  CHECK(r.summary.classes[2].count == 75);
  CHECK(r.summary.classes[2].first.size() == default_summary_ids);
  // visited: the hub and its 150 dependents, examined: the 150 edges into the hub.
  CHECK(s.visited == 151);
  CHECK(s.edges_examined == 150);

  // A smaller listing bound keeps the counts.
  impact_options few = o;
  few.summary_ids = 4;
  few.attachment = "test.impact.few";
  auto small = must(impact_engine::attach(m, must(m.head()), few));
  const impact_report rs = must(small->report(changed));
  check_report(rs, s, 4);
  CHECK(rs.summary.classes[1].first.size() == 4);
  CHECK(rs.summary.classes[1].count == 75);

  CHECK(engine->impact(std::vector<record_id>{record_id{}}).error().code == errc::invalid_argument);
}
