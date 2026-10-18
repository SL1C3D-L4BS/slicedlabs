// FAB-INV-LC-03 (SL-ARCH-001 ARCH1-R06, ARCH1-R07; ADR-0011): instance ids derive from (parent
// instance or document id, source entity id), never from position: instance_id_for is pure,
// separates parents (the same prefab placed twice, or nested, gives distinct ids) and refuses
// parents and sources of other classes. Re-instantiating a prefab whose entities were reordered,
// or that gained an entity, lands on the instance ids already in the project, so the overrides
// held on those instances (a field set on the instance, an Override record holding a strong ref
// to it) survive; an instance the user deleted stays deleted (its id is tombstoned, a re-create is
// id_reuse). Seeded over the memory backend: random prefabs, random reorders and additions, and
// two independent workspaces agree on every instance id and record object id.

#include "sl/fabric/clock.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/identity.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/test_schemas.hpp"
#include "../../../engine/fabric/testing/tx_fixture.hpp"
#include "../../../engine/fabric/testing/undo_fixture.hpp"
#include "../../../engine/fabric/testing/value_gen.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <set>
#include <span>
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

void must_ok(const sl::result<void> &r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
}

constexpr std::string_view instance_type_name = "content.scene.Instance";
constexpr std::string_view override_type_name = "content.scene.Override";

field_desc make_field(std::uint32_t no, std::string_view name, std::string_view type,
                      presence p = presence::required) {
  field_desc f;
  f.no = no;
  f.name = std::string(name);
  f.type = type_id_of(type);
  f.presence = p;
  return f;
}

type_desc record_type(std::string_view name, id_class c, std::vector<field_desc> fields) {
  type_desc t;
  t.name = std::string(name);
  t.id = type_id_of(name);
  t.kind = type_kind::struct_;
  t.domain = "content.scene";
  t.record_class = c;
  t.state_class = state_class::document;
  t.fields = std::move(fields);
  return t;
}

// The test schemas plus content.scene.Instance (instance) {1 source id, 2 note string optional}
// and content.scene.Override (entity) {1 target ref strong -> Instance, 2 text string}.
workspace_options instance_options(std::uint64_t seed = 1) {
  workspace_options o = must(tg::scene_options(std::make_shared<manual_clock>(1), seed));
  schema_registry reg;
  must_ok(tg::add_test_schemas(reg));
  must_ok(reg.add(record_type(
      instance_type_name, id_class::instance,
      {make_field(1, "source", "id"), make_field(2, "note", "string", presence::optional)})));
  field_desc target = make_field(1, "target", "ref");
  target.ref =
      ref_spec{.strength = ref_strength::strong, .targets = {type_id_of(instance_type_name)}};
  must_ok(reg.add(record_type(override_type_name, id_class::entity,
                              {target, make_field(2, "text", "string")})));
  must_ok(reg.freeze());
  domain_registry domains;
  for (const char *name : {"content.scene", "content.mesh"}) {
    must_ok(domains.claim({.name = name,
                           .owner = subsystem::content,
                           .state_classes = {state_class::document},
                           .types = {}}));
  }
  must_ok(domains.claim({.name = "asset.cooked",
                         .owner = subsystem::asset,
                         .state_classes = {state_class::derived},
                         .types = {}}));
  must_ok(domains.bind_types(reg));
  o.schemas = std::move(reg);
  o.domains = std::move(domains);
  return o;
}

// The ops that (re)instantiate `sources` under `parent` over `head`: a create for every instance
// id that is unborn; nothing for a live one (its record and the overrides on it stay) nor for a
// tombstoned one (the user deleted that instance, and an id is never reused).
std::vector<patch_op> instantiate(const snapshot &head, const record_id &parent,
                                  std::span<const record_id> sources) {
  std::vector<patch_op> ops;
  for (const record_id &source : sources) {
    const record_id id = must(instance_id_for(parent, source)).id();
    if (must(lifetime_of(head, id)) != id_lifetime::unborn) {
      continue;
    }
    ops.push_back(op_create(id, type_id_of(instance_type_name),
                            must(value::struct_({field{.no = 1, .v = value::id(source)}}))));
  }
  return ops;
}

patch_op create_override(const record_id &id, const record_id &instance, std::string_view text) {
  return op_create(id, type_id_of(override_type_name),
                   must(value::struct_({field{.no = 1, .v = value::id(instance)},
                                        field{.no = 2, .v = must(value::string(text))}})));
}

record_id inst(const record_id &parent, const record_id &source) {
  return must(instance_id_for(parent, source)).id();
}

} // namespace

TEST_CASE("FAB-INV-LC-03: instance_id_for is pure, separates parents and checks classes") {
  const record_id level = tg::entity_named("level");
  const record_id crate = tg::asset_named("crate");
  const record_id e0 = tg::entity_named("crate/e0");
  const record_id e1 = tg::entity_named("crate/e1");

  const instance_id a = must(instance_id_for(level, e0));
  CHECK(a.id().cls() == id_class::instance);
  CHECK(a.id().scheme() == mint_scheme::derived);
  CHECK(a.id() == derive(id_class::instance, level, e0.to_string()));
  CHECK(must(instance_id_for(level, e0)) == a);
  CHECK(inst(level, e1) != a.id());
  // The same source under another parent, and nested under an instance of a prefab asset.
  const record_id placed = inst(level, crate);
  CHECK(inst(tg::entity_named("level2"), e0) != a.id());
  CHECK(inst(placed, e0) != a.id());
  CHECK(inst(placed, e0) == inst(inst(level, crate), e0));
  CHECK(inst(placed, e0) != inst(inst(tg::entity_named("level2"), crate), e0));

  CHECK(instance_id_for(record_id{}, e0).error().code == errc::invalid_argument);
  CHECK(instance_id_for(level, record_id{}).error().code == errc::invalid_argument);
  for (id_class bad : {id_class::tx, id_class::derived, id_class::type, id_class::element,
                       id_class::import, id_class::project}) {
    CAPTURE(to_string(bad));
    const record_id other = well_known(bad, 3);
    CHECK(instance_id_for(other, e0).error().code == errc::id_class_mismatch);
    CHECK(instance_id_for(level, other).error().code == errc::id_class_mismatch);
  }
}

TEST_CASE("FAB-INV-LC-03: re-instantiation after a sibling reorder keeps ids and overrides") {
  auto ws = must(workspace::create_memory(instance_options()));
  transaction_manager &m = ws->manager();
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  const record_id level = tg::entity_named("level");
  const record_id a = tg::entity_named("crate/a");
  const record_id b = tg::entity_named("crate/b");
  const record_id c = tg::entity_named("crate/c");
  const record_id d = tg::entity_named("crate/d");

  const std::vector<record_id> prefab = {a, b, c};
  must(tg::commit_ops(m, editor, instantiate(must(m.head()), level, prefab)));
  for (const record_id &s : prefab) {
    CHECK(must(m.head().and_then([&](const snapshot &h) { return h.contains(inst(level, s)); })));
  }
  // Overrides on instance b: a field on the instance and an Override record referring to it.
  const record_id ovr = tg::entity_named("override/b");
  must(tg::commit_ops(m, editor,
                      {op_set(inst(level, b), {path_segment::field(2)}, must(value::string("red"))),
                       create_override(ovr, inst(level, b), "scale 2")}));
  const snapshot_entry b_before = must(must(m.head()).get(inst(level, b)));

  // The prefab's entities reordered: nothing to do.
  const std::vector<record_id> reordered = {c, a, b};
  CHECK(instantiate(must(m.head()), level, reordered).empty());
  // Reordered and grown: only the new entity is instantiated.
  const std::vector<record_id> grown = {c, d, a, b};
  const std::vector<patch_op> ops = instantiate(must(m.head()), level, grown);
  REQUIRE(ops.size() == 1);
  CHECK(ops.front().target == inst(level, d));
  must(tg::commit_ops(m, editor, ops));
  const snapshot head = must(m.head());
  CHECK(must(head.get(inst(level, b))) == b_before);
  CHECK(must(head.get(ovr)).body.find_field(1)->as_record_id().value_or(record_id{}) ==
        inst(level, b));
  CHECK(must(head.members(level)).empty()); // instances here belong to no document

  // Deleting an instance that carries an override is blocked; deleting a plain one retires it,
  // and neither re-instantiation nor a raw create brings the id back.
  CHECK(tg::commit_ops(m, editor, {op_delete(inst(level, b))}).error().code ==
        errc::validation_failed);
  must(tg::commit_ops(m, editor, {op_delete(inst(level, a))}));
  CHECK(instantiate(must(m.head()), level, grown).empty());
  tx_options o;
  o.origin = editor;
  auto t = must(m.begin(o));
  must_ok(t.stage(op_create(inst(level, a), type_id_of(instance_type_name),
                            must(value::struct_({field{.no = 1, .v = value::id(a)}})))));
  CHECK(t.commit().error().code == errc::validation_failed);
  CHECK(std::ranges::any_of(t.diagnostics(),
                            [](const diagnostic &x) { return x.rule == "R-INV-ID-REUSE"; }));
}

TEST_CASE("FAB-INV-LC-03: seeded prefabs keep instance ids across reorders and workspaces") {
  const std::size_t seeds = tg::sweep_scaled(20);
  std::size_t kept = 0;
  for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
    CAPTURE(seed);
    tg::rng random(seed);
    tg::value_gen values(seed);
    std::vector<record_id> prefab;
    for (std::size_t i = 0, n = 2 + random.below(6); i < n; ++i) {
      prefab.push_back(values.random_id(random.chance(50) ? id_class::entity : id_class::asset));
    }
    const record_id parent =
        values.random_id(random.chance(50) ? id_class::entity : id_class::instance);
    // Two workspaces with different id sources instantiate the same prefab.
    auto ws1 = must(workspace::create_memory(instance_options(seed)));
    auto ws2 = must(workspace::create_memory(instance_options(seed + 1000)));
    const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
    must(tg::commit_ops(ws1->manager(), editor,
                        instantiate(must(ws1->manager().head()), parent, prefab)));
    // Overrides on every instance in workspace 1.
    std::vector<patch_op> notes;
    notes.reserve(prefab.size());
    for (const record_id &s : prefab) {
      notes.push_back(
          op_set(inst(parent, s), {path_segment::field(2)}, must(value::string(s.to_string()))));
    }
    must(tg::commit_ops(ws1->manager(), editor, notes));
    std::vector<record_id> shuffled = prefab;
    for (std::size_t i = shuffled.size(); i > 1; --i) {
      std::swap(shuffled[i - 1], shuffled[random.below(i)]);
    }
    must(tg::commit_ops(ws2->manager(), editor,
                        instantiate(must(ws2->manager().head()), parent, shuffled)));
    // Re-instantiation in a few shuffled orders, plus one new source each round.
    for (int round = 0; round < 3; ++round) {
      for (std::size_t i = shuffled.size(); i > 1; --i) {
        std::swap(shuffled[i - 1], shuffled[random.below(i)]);
      }
      shuffled.push_back(values.random_id(id_class::entity));
      const std::vector<patch_op> ops = instantiate(must(ws1->manager().head()), parent, shuffled);
      REQUIRE(ops.size() == 1);
      CHECK(ops.front().target == inst(parent, shuffled.back()));
      must(tg::commit_ops(ws1->manager(), editor, ops));
    }
    const snapshot h1 = must(ws1->manager().head());
    const snapshot h2 = must(ws2->manager().head());
    for (const record_id &s : prefab) {
      const snapshot_entry e1 = must(h1.get(inst(parent, s)));
      const snapshot_entry e2 = must(h2.get(inst(parent, s)));
      CHECK(e1.body.find_field(2)->as_string() == s.to_string()); // the override survived
      CHECK(e1.body.find_field(1)->as_record_id().value_or(record_id{}) == s);
      CHECK(e2.body.find_field(1)->as_record_id().value_or(record_id{}) == s);
      ++kept;
    }
    std::set<record_id> ids;
    for (const record_id &s : shuffled) {
      ids.insert(inst(parent, s));
    }
    CHECK(ids.size() == shuffled.size());
    CHECK(must(h1.entry_count()) == shuffled.size());
  }
  CHECK(kept >= seeds * 2);
}
