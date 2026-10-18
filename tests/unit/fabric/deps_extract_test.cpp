// FAB-INV-DEP-01 (SL-ARCH-001 ARCH1-R55): extracted edges equal the strong and owned refs plus the
// provenance inputs and the fabric.edge records; weak refs make no edge.

#include "sl/fabric/deps.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"

#include "../../../engine/fabric/testing/deps_model_store.hpp"
#include "../../../engine/fabric/testing/test_schemas.hpp"
#include "../../../engine/fabric/testing/value_gen.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;
namespace tt = sl::fabric::testing::test_types;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

record_id rid(id_class c, std::string_view key) { return testing::test_id(c, key); }

// ---- a schema with a ref behind every container shape -----------------------------------------

constexpr std::string_view n_ref_list = "content.t.RefList";
constexpr std::string_view n_ref_map = "content.t.RefMap";
constexpr std::string_view n_ref_set = "content.t.RefSet";
constexpr std::string_view n_ref_tag = "content.t.RefTag";
constexpr std::string_view n_inner = "content.t.Inner";
constexpr std::string_view n_inner_list = "content.t.InnerList";
constexpr std::string_view n_choice = "content.t.Choice";
constexpr std::string_view n_node = "content.t.Node";
constexpr std::string_view n_edge = "fabric.edge.Edge";
constexpr std::string_view n_other = "content.t.Plain";

type_desc make(std::string_view name, type_kind kind, std::string_view domain) {
  type_desc t;
  t.name = std::string(name);
  t.id = type_id_of(name);
  t.kind = kind;
  t.domain = std::string(domain);
  return t;
}

type_desc over(std::string_view name, type_kind kind, std::string_view element) {
  type_desc t = make(name, kind, "content.t");
  t.element = type_id_of(element);
  return t;
}

field_desc fld(std::uint32_t no, std::string_view name, std::string_view type,
               presence p = presence::required) {
  field_desc f;
  f.no = no;
  f.name = std::string(name);
  f.type = type_id_of(type);
  f.presence = p;
  return f;
}

field_desc rfld(std::uint32_t no, std::string_view name, std::string_view type, ref_strength s,
                presence p = presence::optional) {
  field_desc f = fld(no, name, type, p);
  f.ref = ref_spec{.strength = s, .targets = {type_id_of(n_node)}};
  return f;
}

schema_registry make_registry() {
  schema_registry reg;
  REQUIRE(testing::add_test_schemas(reg).has_value());
  const auto add = [&](type_desc t) { REQUIRE(reg.add(std::move(t)).has_value()); };
  add(over(n_ref_list, type_kind::list, "ref"));
  type_desc map = over(n_ref_map, type_kind::map, "ref");
  map.key = type_id_of("string");
  add(std::move(map));
  add(over(n_ref_set, type_kind::oset, "ref"));
  add(over(n_ref_tag, type_kind::tagged, "ref"));

  type_desc inner = make(n_inner, type_kind::struct_, "content.t");
  inner.fields = {rfld(1, "target", "ref", ref_strength::strong, presence::required),
                  fld(2, "note", "string", presence::optional)};
  add(std::move(inner));
  add(over(n_inner_list, type_kind::list, n_inner));

  type_desc choice = make(n_choice, type_kind::variant, "content.t");
  choice.cases = {case_desc{.no = 1, .name = "inner", .type = type_id_of(n_inner), .doc = ""},
                  case_desc{.no = 2, .name = "count", .type = type_id_of("u32"), .doc = ""}};
  add(std::move(choice));

  type_desc node = make(n_node, type_kind::struct_, "content.t");
  node.record_class = id_class::entity;
  node.state_class = state_class::document;
  node.fields = {rfld(1, "strong_list", n_ref_list, ref_strength::strong),
                 rfld(2, "owned_map", n_ref_map, ref_strength::owned),
                 rfld(3, "weak_set", n_ref_set, ref_strength::weak),
                 rfld(4, "tagged", n_ref_tag, ref_strength::strong),
                 fld(5, "inners", n_inner_list, presence::optional),
                 fld(6, "choice", n_choice, presence::optional),
                 rfld(7, "single", "ref", ref_strength::owned),
                 rfld(8, "weak_single", "ref", ref_strength::weak)};
  add(std::move(node));

  type_desc plain = make(n_other, type_kind::struct_, "content.t");
  plain.record_class = id_class::entity;
  plain.state_class = state_class::document;
  plain.fields = {fld(1, "name", "string")};
  add(std::move(plain));

  type_desc edge_rec = make(n_edge, type_kind::struct_, "fabric.edge");
  edge_rec.record_class = id_class::operation;
  edge_rec.state_class = state_class::document;
  edge_rec.fields = {fld(1, "from", "id"), fld(2, "to", "id"), fld(3, "kind", "u8")};
  add(std::move(edge_rec));

  REQUIRE(reg.freeze().has_value());
  return reg;
}

const schema_registry &registry() {
  static const schema_registry reg = make_registry();
  return reg;
}

const type_desc &type_named(std::string_view name) {
  const type_desc *t = registry().find(name);
  REQUIRE(t != nullptr);
  return *t;
}

value ref_of(const record_id &id) { return value::id(id); }

struct built {
  value body;
  std::vector<edge> expected;
};

/// A Node whose fields hold the given target counts; `pool` supplies the targets. The expected
/// edges are what the strengths say, worked out here and not by the code under test.
built make_node(const record_id &self, const std::vector<record_id> &pool,
                sl::fabric::testing::rng &r) {
  std::set<edge> expected;
  const auto pick = [&] { return pool[r.below(pool.size())]; };
  const auto add = [&](const record_id &to, edge_kind k, ref_strength s) {
    expected.insert(edge{.from = self, .to = to, .kind = k, .strength = s});
  };
  std::vector<field> fields;
  const auto push = [&](std::uint32_t no, value v) {
    fields.push_back(field{.no = no, .v = std::move(v)});
  };

  std::vector<value> list;
  for (std::uint64_t i = r.below(4); i > 0; --i) {
    const record_id t = pick();
    list.push_back(ref_of(t));
    add(t, edge_kind::reference, ref_strength::strong);
  }
  push(1, must(value::list(std::move(list))));

  std::vector<map_entry> map;
  for (std::uint64_t i = r.below(4); i > 0; --i) {
    const record_id t = pick();
    map.push_back(map_entry{.key = must(value::string(std::format("k{}", i))), .v = ref_of(t)});
    add(t, edge_kind::containment, ref_strength::owned);
  }
  if (!map.empty() || r.chance(50)) {
    push(2, must(value::map(std::move(map))));
  }

  std::vector<oset_entry> weak;
  for (std::uint64_t i = r.below(4); i > 0; --i) {
    weak.push_back(oset_entry{.element = rid(id_class::element, std::format("w{}", i)).bytes(),
                              .order_key = {static_cast<std::uint8_t>(0x10 * i)},
                              .v = ref_of(pick())});
  }
  if (!weak.empty() || r.chance(50)) {
    push(3, must(value::oset(std::move(weak))));
  }

  if (r.chance(60)) {
    const record_id t = pick();
    push(4, ref_of(t));
    add(t, edge_kind::reference, ref_strength::strong);
  }

  if (r.chance(60)) {
    std::vector<value> inners;
    for (std::uint64_t i = r.below(3); i > 0; --i) {
      const record_id t = pick();
      inners.push_back(must(value::struct_({field{.no = 1, .v = ref_of(t)}})));
      add(t, edge_kind::reference, ref_strength::strong);
    }
    push(5, must(value::list(std::move(inners))));
  }

  if (r.chance(60)) {
    const record_id t = pick();
    const value inner = must(value::struct_({field{.no = 1, .v = ref_of(t)}}));
    push(6, must(value::variant(1, inner)));
    add(t, edge_kind::reference, ref_strength::strong);
  } else if (r.chance(50)) {
    push(6, must(value::variant(2, value::uint(7))));
  }

  const record_id single = pick();
  push(7, ref_of(single));
  add(single, edge_kind::containment, ref_strength::owned);

  if (r.chance(50)) {
    push(8, ref_of(pick())); // weak: never an edge
  }
  return built{.body = must(value::struct_(std::move(fields))),
               .expected = {expected.begin(), expected.end()}};
}

std::vector<record_id> pool_of(std::size_t n) {
  std::vector<record_id> pool;
  pool.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    pool.push_back(rid(id_class::entity, std::format("pool/{}", i)));
  }
  return pool;
}

} // namespace

TEST_CASE("FAB-INV-DEP-01: strong and owned refs behind every container shape make edges, weak "
          "refs none") {
  // 200 seeded nodes: refs in a list, a map, an oset, a tagged ref, a struct in a list, a struct in
  // a variant and a single ref; the extracted edges equal the independently worked out set.
  const std::vector<record_id> pool = pool_of(6);
  const type_desc &node = type_named(n_node);
  for (std::uint64_t seed = 0; seed < 200; ++seed) {
    sl::fabric::testing::rng r(seed);
    const record_id self = rid(id_class::entity, std::format("self/{}", seed));
    const built b = make_node(self, pool, r);
    std::vector<diagnostic_lite> diags;
    REQUIRE_MESSAGE(registry().check(b.body, node.id, diags).has_value(),
                    "the generated node conforms (seed " << seed << ")");
    const auto edges = extract_edges(self, b.body, node, registry());
    REQUIRE_MESSAGE(edges.has_value(), (edges ? std::string() : edges.error().message));
    CHECK_MESSAGE(*edges == b.expected, "seed " << seed);
    CHECK(std::ranges::is_sorted(*edges));
  }
}

TEST_CASE("FAB-INV-DEP-01: a node with only weak refs has no edge") {
  const record_id self = rid(id_class::entity, "weak/self");
  const record_id other = rid(id_class::entity, "weak/other");
  // The Entity of the test schemas holds only a weak parent ref.
  const value entity = must(testing::sample_entity("weak"));
  const type_desc &ent = type_named(tt::entity);
  CHECK(must(extract_edges(self, entity, ent, registry())).empty());

  const value with_parent = must(entity.with({path_segment::field(3)}, ref_of(other)));
  std::vector<diagnostic_lite> diags;
  REQUIRE(registry().check(with_parent, ent.id, diags).has_value());
  CHECK(must(extract_edges(self, with_parent, ent, registry())).empty());
}

TEST_CASE("FAB-INV-DEP-01: the test schemas give containment from a scene and reference from a "
          "renderer") {
  const record_id scene_id = rid(id_class::entity, "scene/main");
  const type_desc &scene = type_named(tt::scene);
  const value s = must(testing::sample_scene("main", 3));
  const auto edges = must(extract_edges(scene_id, s, scene, registry()));
  std::vector<edge> expected;
  expected.reserve(3);
  for (unsigned i = 0; i < 3; ++i) {
    expected.push_back(edge{.from = scene_id,
                            .to = rid(id_class::entity, std::format("main/e{}", i)),
                            .kind = edge_kind::containment,
                            .strength = ref_strength::owned});
  }
  std::ranges::sort(expected);
  CHECK(edges == expected);

  // An entity with a mesh renderer component refers to its mesh.
  const record_id mesh = rid(id_class::asset, "mesh/rock");
  const record_id ent_id = rid(id_class::entity, "ent/rock");
  const value renderer = must(value::struct_({field{.no = 1, .v = ref_of(mesh)}}));
  const value ent = must(testing::sample_entity("rock"));
  const value comp = must(value::variant(2, renderer));
  const value with_renderer = must(ent.insert_element(
      {path_segment::field(4)}, rid(id_class::element, "rock/renderer").bytes(), {0xC0}, comp));
  const auto ref_edges =
      must(extract_edges(ent_id, with_renderer, type_named(tt::entity), registry()));
  REQUIRE(ref_edges.size() == 1);
  CHECK(ref_edges[0] == edge{.from = ent_id,
                             .to = mesh,
                             .kind = edge_kind::reference,
                             .strength = ref_strength::strong});
}

TEST_CASE("FAB-INV-DEP-01: a value that does not fit its type is type_mismatch naming the path") {
  const record_id self = rid(id_class::entity, "bad/self");
  const type_desc &node = type_named(n_node);
  const record_id t = rid(id_class::entity, "bad/t");

  const value undeclared =
      must(value::struct_({field{.no = 7, .v = ref_of(t)}, field{.no = 99, .v = value::uint(1)}}));
  const auto a = extract_edges(self, undeclared, node, registry());
  REQUIRE_FALSE(a.has_value());
  CHECK(a.error().code == sl::errc::type_mismatch);
  CHECK(a.error().message.starts_with("/99"));

  const value null_ref = must(value::struct_({field{.no = 7, .v = value::id(id_bytes{})}}));
  const auto b = extract_edges(self, null_ref, node, registry());
  REQUIRE_FALSE(b.has_value());
  CHECK(b.error().code == sl::errc::type_mismatch);
  CHECK(b.error().message.starts_with("/7"));

  const value not_id = must(value::struct_({field{.no = 7, .v = value::uint(5)}}));
  CHECK(extract_edges(self, not_id, node, registry()).error().code == sl::errc::type_mismatch);

  const auto not_struct = extract_edges(self, value::uint(1), node, registry());
  REQUIRE_FALSE(not_struct.has_value());
  CHECK(not_struct.error().code == sl::errc::type_mismatch);
}

TEST_CASE("FAB-INV-DEP-01: provenance inputs are derives_from edges, sorted and unique") {
  const record_id derived = rid(id_class::derived, "cooked/a");
  const record_id in1 = rid(id_class::asset, "in/1");
  const record_id in2 = rid(id_class::asset, "in/2");
  const std::vector<record_id> inputs = {in2, in1, in2};
  const std::vector<edge> edges = provenance_edges(derived, inputs);
  const std::vector<edge> expected = [&] {
    std::vector<edge> e = {edge{.from = derived,
                                .to = in1,
                                .kind = edge_kind::derives_from,
                                .strength = ref_strength::strong},
                           edge{.from = derived,
                                .to = in2,
                                .kind = edge_kind::derives_from,
                                .strength = ref_strength::strong}};
    std::ranges::sort(e);
    return e;
  }();
  CHECK(edges == expected);
  CHECK(provenance_edges(derived, {}).empty());
}

TEST_CASE("FAB-INV-DEP-01: an edge record names its edge, any of the five kinds") {
  const record_id from = rid(id_class::derived, "edge/from");
  const record_id to = rid(id_class::asset, "edge/to");
  for (std::uint64_t k = 1; k <= 5; ++k) {
    const value body =
        must(value::struct_({field{.no = 1, .v = ref_of(from)}, field{.no = 2, .v = ref_of(to)},
                             field{.no = 3, .v = value::uint(k)}}));
    const edge e = must(edge_from_record(body));
    CHECK(e == edge{.from = from,
                    .to = to,
                    .kind = static_cast<edge_kind>(k),
                    .strength = ref_strength::strong});
  }
  const auto missing = edge_from_record(must(value::struct_({field{.no = 1, .v = ref_of(from)}})));
  CHECK(missing.error().code == sl::errc::type_mismatch);
  const auto bad_kind = edge_from_record(
      must(value::struct_({field{.no = 1, .v = ref_of(from)}, field{.no = 2, .v = ref_of(to)},
                           field{.no = 3, .v = value::uint(6)}})));
  CHECK(bad_kind.error().code == sl::errc::invalid_argument);
  const auto bad_id = edge_from_record(
      must(value::struct_({field{.no = 1, .v = value::id(id_bytes{})},
                           field{.no = 2, .v = ref_of(to)}, field{.no = 3, .v = value::uint(1)}})));
  CHECK(bad_id.error().code == sl::errc::id_parse);
  CHECK(edge_from_record(value::uint(1)).error().code == sl::errc::type_mismatch);
}

TEST_CASE("FAB-INV-DEP-01: the graph of a project has exactly the four sources of edges") {
  testing::model_store store;
  const record_id mesh = rid(id_class::asset, "proj/mesh");
  const record_id scene = rid(id_class::entity, "proj/scene");
  const record_id cooked = rid(id_class::derived, "proj/cooked");
  const record_id input = rid(id_class::asset, "proj/texture");
  const record_id build_target = rid(id_class::derived, "proj/target");
  const record_id test_rec = rid(id_class::derived, "proj/test");
  const record_id plain = rid(id_class::entity, "proj/plain");

  const value mesh_body = must(value::struct_(
      {field{.no = 1,
             .v = must(value::struct_({field{.no = 1, .v = must(value::string("sys"))},
                                       field{.no = 2, .v = must(value::string("loc"))}}))},
       field{.no = 2, .v = value::uint(3)}}));
  REQUIRE(store.put(mesh, type_id_of(tt::mesh), mesh_body).has_value());
  REQUIRE(
      store.put(scene, type_id_of(tt::scene), must(testing::sample_scene("proj", 2))).has_value());
  const value cooked_body = must(value::struct_(
      {field{.no = 1, .v = ref_of(mesh)}, field{.no = 2, .v = value::digest(digest{})}}));
  REQUIRE(store.put(cooked, type_id_of(tt::cooked_mesh), cooked_body, {input, mesh}).has_value());
  REQUIRE(store
              .put(plain, type_id_of(n_other),
                   must(value::struct_({field{.no = 1, .v = must(value::string("p"))}})))
              .has_value());
  const auto edge_body = [&](const record_id &f, const record_id &t, std::uint64_t k) {
    return must(value::struct_({field{.no = 1, .v = ref_of(f)}, field{.no = 2, .v = ref_of(t)},
                                field{.no = 3, .v = value::uint(k)}}));
  };
  REQUIRE(store
              .put(rid(id_class::operation, "proj/edge1"), type_id_of(n_edge),
                   edge_body(build_target, cooked, 4))
              .has_value());
  REQUIRE(store
              .put(rid(id_class::operation, "proj/edge2"), type_id_of(n_edge),
                   edge_body(test_rec, scene, 5))
              .has_value());

  const dependency_graph g = must(dependency_graph::build(store, registry()));
  std::vector<edge> expected = {edge{.from = scene,
                                     .to = rid(id_class::entity, "proj/e0"),
                                     .kind = edge_kind::containment,
                                     .strength = ref_strength::owned},
                                edge{.from = scene,
                                     .to = rid(id_class::entity, "proj/e1"),
                                     .kind = edge_kind::containment,
                                     .strength = ref_strength::owned},
                                edge{.from = cooked,
                                     .to = mesh,
                                     .kind = edge_kind::reference,
                                     .strength = ref_strength::strong},
                                edge{.from = cooked,
                                     .to = mesh,
                                     .kind = edge_kind::derives_from,
                                     .strength = ref_strength::strong},
                                edge{.from = cooked,
                                     .to = input,
                                     .kind = edge_kind::derives_from,
                                     .strength = ref_strength::strong},
                                edge{.from = build_target,
                                     .to = cooked,
                                     .kind = edge_kind::builds,
                                     .strength = ref_strength::strong},
                                edge{.from = test_rec,
                                     .to = scene,
                                     .kind = edge_kind::validates,
                                     .strength = ref_strength::strong}};
  std::ranges::sort(expected);
  CHECK(g.edges() == expected);
  CHECK(g.edge_count() == expected.size());
  CHECK(g.out(plain).empty());
  CHECK(g.contains_record(plain));
  CHECK_FALSE(g.contains_record(build_target)); // named by an edge record, not a record itself
  // Both directions agree and the in-list of the mesh holds both edges that end there.
  CHECK(g.in(mesh).size() == 2);
  CHECK(g.in(mesh)[0].from == cooked);
}

TEST_CASE(
    "FAB-INV-DEP-01: build refuses an unknown type, provenance on a document and bad bodies") {
  const record_id id = rid(id_class::entity, "refuse/r");
  const value body = must(value::struct_({field{.no = 1, .v = must(value::string("p"))}}));
  {
    testing::model_store store;
    REQUIRE(store.put(id, type_id_of("content.t.Nope"), body).has_value());
    const auto g = dependency_graph::build(store, registry());
    REQUIRE_FALSE(g.has_value());
    CHECK(g.error().code == sl::errc::schema_unknown);
  }
  {
    testing::model_store store;
    REQUIRE(
        store.put(id, type_id_of(n_other), body, {rid(id_class::asset, "refuse/in")}).has_value());
    const auto g = dependency_graph::build(store, registry());
    REQUIRE_FALSE(g.has_value());
    CHECK(g.error().code == sl::errc::invalid_argument);
  }
  {
    testing::model_store store;
    REQUIRE(store.put(id, type_id_of(n_other), value::uint(3)).has_value());
    const auto g = dependency_graph::build(store, registry());
    REQUIRE_FALSE(g.has_value());
    CHECK(g.error().code == sl::errc::type_mismatch);
  }
  {
    testing::model_store store;
    const value bad_edge = must(value::struct_({field{.no = 1, .v = value::uint(1)}}));
    REQUIRE(store.put(id, type_id_of(n_edge), bad_edge).has_value());
    CHECK(dependency_graph::build(store, registry()).error().code == sl::errc::type_mismatch);
  }
}

TEST_CASE("FAB-INV-DEP-01: an edge several records contribute stays until the last drops it") {
  const record_id a = rid(id_class::entity, "own/a");
  const record_id b = rid(id_class::entity, "own/b");
  const record_id edge_rec = rid(id_class::operation, "own/rec");
  const edge e{.from = a, .to = b, .kind = edge_kind::reference, .strength = ref_strength::strong};

  dependency_graph g;
  REQUIRE(g.set_record(a, {e}).has_value());
  REQUIRE(g.set_record(edge_rec, {e}).has_value());
  CHECK(g.edge_count() == 1);
  g.erase_record(a);
  CHECK(g.edge_count() == 1);
  CHECK(g.out(a).size() == 1);
  CHECK_FALSE(g.contains_record(a));
  g.erase_record(edge_rec);
  CHECK(g.edge_count() == 0);
  CHECK(g.node_count() == 0); // nothing present and nothing connected

  // Invalid input is refused whole.
  CHECK(g.set_record(a, {edge{.from = a,
                              .to = record_id{},
                              .kind = edge_kind::reference,
                              .strength = ref_strength::strong}})
            .error()
            .code == sl::errc::invalid_argument);
  CHECK(g.set_record(record_id{}, {}).error().code == sl::errc::invalid_argument);
  CHECK(g.node_count() == 0);
}
