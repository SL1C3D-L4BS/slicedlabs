// FAB-INV-LC-05 (SL-ARCH-001 ARCH1-R04, ARCH1-R17; ADR-0011): a record whose id class differs from
// its type's record class is rejected on commit (R-INV-ID-CLASS, id_class_mismatch, stage
// invariants, from the identity validator), whether it is created so or retyped into a type of
// another record class, and the dry run reports the same; a matching class commits. The stable-id
// helpers hand out ids of the right class only. Seeded over the memory backend: every pair of id
// class and record type is accepted exactly when the classes match.

#include "sl/fabric/clock.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/identity.hpp"
#include "sl/fabric/memory_backend.hpp"
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
#include <memory>
#include <optional>
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

std::unique_ptr<workspace> scene_ws() {
  return must(workspace::create_with(std::make_shared<memory_backend>(),
                                     must(tg::scene_options(std::make_shared<manual_clock>(1)))));
}

bool reports_class(std::span<const diagnostic> d, const record_id &id) {
  return std::ranges::any_of(d, [&](const diagnostic &x) {
    return x.rule == "R-INV-ID-CLASS" && x.stage == validator_stage::invariants && x.record == id &&
           x.message.starts_with("id_class_mismatch");
  });
}

// A create of `type` (with a conforming body) under `id`.
patch_op create_as(const record_id &id, const type_id &type, const record_id &mesh) {
  if (type == tg::entity_type()) {
    return op_create(id, type, must(tg::sample_entity("x")));
  }
  if (type == tg::scene_type()) {
    patch_op op = must(tg::create_scene(id, "s", {}));
    return op;
  }
  if (type == tg::mesh_type()) {
    patch_op op = must(tg::create_mesh("m", 3));
    op.target = id;
    return op;
  }
  patch_op op = must(tg::create_cooked("c", mesh));
  op.target = id;
  return op;
}

} // namespace

TEST_CASE("FAB-INV-LC-05: a record under an id of another class is rejected on commit") {
  auto ws = scene_ws();
  transaction_manager &m = ws->manager();
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  const std::optional<digest> head = must(m.head()).revision_digest();

  // An Entity (record class entity) under an asset id, then under an instance id.
  for (const record_id &wrong :
       {tg::asset_named("hero"), well_known(id_class::instance, 4), tg::derived_named("hero")}) {
    CAPTURE(wrong.to_string());
    tx_options o;
    o.origin = editor;
    auto t = must(m.begin(o));
    must_ok(t.stage(op_create(wrong, tg::entity_type(), must(tg::sample_entity("hero")))));
    CHECK(reports_class(must(t.validate()), wrong));
    CHECK(t.commit().error().code == errc::validation_failed);
    CHECK(reports_class(t.diagnostics(), wrong));
    CHECK(must(m.head()).revision_digest() == head);
  }
  // The right class commits.
  must(tg::commit_ops(m, editor, {must(tg::create_entity("hero"))}));

  // Retyping the entity into Mesh (record class asset) is the same confusion.
  {
    tx_options o;
    o.origin = editor;
    auto t = must(m.begin(o));
    const patch_op mesh = must(tg::create_mesh("hero", 3));
    must_ok(t.stage(
        op_retype(tg::entity_named("hero"), tg::mesh_type(), mesh.new_value.value_or(value{}))));
    CHECK(t.commit().error().code == errc::validation_failed);
    CHECK(reports_class(t.diagnostics(), tg::entity_named("hero")));
  }
  // Retyping into Scene (record class entity) keeps the class and commits.
  {
    tx_options o;
    o.origin = editor;
    auto t = must(m.begin(o));
    const patch_op scene = must(tg::create_scene(tg::entity_named("hero"), "hero", {}));
    must_ok(t.stage(
        op_retype(tg::entity_named("hero"), tg::scene_type(), scene.new_value.value_or(value{}))));
    must(t.commit());
  }
}

TEST_CASE("FAB-INV-LC-05: the stable-id helpers hand out ids of the right class") {
  const record_id level = tg::entity_named("level");
  CHECK(must(instance_id_for(level, tg::entity_named("e"))).id().cls() == id_class::instance);
  CHECK(must(derived_id_for(tg::complete_provenance(level))).id().cls() == id_class::derived);
  for (std::uint8_t n = 1; n <= 15; ++n) {
    const auto c = static_cast<id_class>(n);
    CAPTURE(to_string(c));
    auto id = imported_id_for(c, "sys", "ext");
    CHECK(id.has_value() == names_record(c));
    if (id) {
      CHECK(id->cls() == c);
      CHECK(id->scheme() == mint_scheme::imported);
    } else {
      CHECK(id.error().code == errc::id_class_mismatch);
    }
  }
  // A typed id of another class does not convert (ARCH1-R04: an id never changes class).
  CHECK(instance_id::from(level).error().code == errc::id_class_mismatch);
  CHECK(derived_id::from(level).error().code == errc::id_class_mismatch);
}

TEST_CASE("FAB-INV-LC-05: every class and record type pair commits exactly when they match") {
  const std::vector<type_id> types = {tg::entity_type(), tg::scene_type(), tg::mesh_type(),
                                      tg::cooked_type()};
  // Every (class, type) pair, each with a few seeded random ids of that class.
  const std::size_t per_pair = std::max<std::size_t>(1, tg::sweep_scaled(2));
  std::size_t accepted = 0;
  std::size_t rejected = 0;
  auto ws = scene_ws();
  transaction_manager &m = ws->manager();
  const record_id mesh = tg::asset_named("source");
  must(tg::commit_ops(m, tg::tx_origin_of(tg::tx_editor()), {must(tg::create_mesh("source", 3))}));
  const schema_registry &schemas = m.schemas();
  tg::value_gen values(17);
  for (std::size_t i = 0; i < 15 * types.size() * per_pair; ++i) {
    const auto c = static_cast<id_class>(1 + (i / (types.size() * per_pair)));
    const type_id type = types[(i / per_pair) % types.size()];
    if (!names_record(c)) {
      continue; // refused earlier, at the structural stage (R-STR-TARGET)
    }
    const record_id id = values.random_id(c);
    const type_desc *t = schemas.find(type);
    REQUIRE(t != nullptr);
    CAPTURE(to_string(c));
    CAPTURE(t->name);
    const bool cooked = type == tg::cooked_type();
    tx_options o;
    o.origin = tg::tx_origin_of(tg::tx_editor(), cooked ? "asset" : "content");
    if (cooked) {
      o.origin.provenance = tg::complete_provenance(mesh);
      o.origin.epistemic = epistemic_class::calculated;
    }
    auto tx = must(m.begin(o));
    must_ok(tx.stage(create_as(id, type, mesh)));
    const auto r = tx.commit();
    const bool match = c == t->record_class;
    CHECK(reports_class(tx.diagnostics(), id) == !match);
    CHECK(r.has_value() == match);
    (match ? accepted : rejected) += 1;
  }
  CHECK(accepted == 4 * per_pair); // entity: Entity, Scene; asset: Mesh; derived: CookedMesh
  CHECK(rejected == ((11 * types.size()) - 4) * per_pair);
}
