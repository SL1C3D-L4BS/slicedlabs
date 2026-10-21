// RT-INV-BND-03 (SL-ARCH-003 ARCH3-R157, ADR-0027): a posted projection is applied at the next
// projection_rebase slot as a diff against the current one: a changed record is rewritten in place
// (same handle, runtime-only bytes kept, no changed tick stamped), a retired record is despawned
// and unmapped, a new record is spawned and mapped, every other entity keeps its handle, and a
// projection no newer than the current one is refused as projection_stale.

#include "sl/core/error.hpp"
#include "sl/fabric/boundary.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/value.hpp"
#include "sl/runtime/bridge.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"

#include "../../../engine/runtime/testing/fabric_host.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace link = sl::runtime::link;
namespace rtt = sl::runtime::testing;
namespace ecs = sl::runtime::ecs;
namespace fab = sl::fabric;
using rtt::fabric_host;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

void must_ok(const sl::result<void> &r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
}

/// The changed tick of `e`'s `id` column (nullopt when no archetype holds it).
std::optional<std::uint32_t> changed_tick_of(const ecs::world &w, ecs::component_id id,
                                             ecs::entity e) {
  for (const ecs::archetype *a : w.archetypes()) {
    if (!a->contains(id)) {
      continue;
    }
    for (std::uint32_t c = 0; c < a->chunk_count(); ++c) {
      const auto rows = a->entities(c);
      for (std::size_t r = 0; r < rows.size(); ++r) {
        // cppcheck-suppress useStlAlgorithm ; the row index, not the element, is used
        if (rows[r] == e) {
          return a->changed_ticks(id, c)[r];
        }
      }
    }
  }
  return std::nullopt;
}

} // namespace

TEST_CASE("RT-INV-BND-03: rebase applies only the diff and keeps the handles of the rest") {
  rtt::rig_options o;
  o.host.levels = 2;
  o.host.bodies = 10;
  const auto rig = must(rtt::link_rig::make(o));
  static_cast<void>(must(rig->ingest()));
  must_ok(rig->build());
  ecs::world &w = *rig->stack->world;

  std::vector<ecs::entity> before;
  before.reserve(10);
  for (std::uint32_t i = 0; i < 10; ++i) {
    before.push_back(must(rig->entity_of(fabric_host::body_id(i))));
  }
  // Runtime-only bytes, written without a stamp.
  must(w.get<rtt::bnd_body>(before[1]))->spare = 77;
  const std::optional<std::uint32_t> tick_before =
      changed_tick_of(w, rig->body_component, before[1]);
  REQUIRE(tick_before.has_value());
  const fab::document_projection old = rig->host->projection();

  // One edit: body 1's mass, retire body 2, create body 10.
  std::vector<fab::patch_op> ops;
  ops.push_back(fabric_host::set_body_field(1, 1, must(fab::value::f32(42.5F))));
  std::vector<fab::patch_op> retire = fabric_host::retire_body_ops(0, 2);
  ops.insert(ops.end(), std::make_move_iterator(retire.begin()),
             std::make_move_iterator(retire.end()));
  const rtt::bnd_body fresh{.mass = 9.0F, .count = 4, .alive = true, .heat = -2, .spare = 0};
  std::vector<fab::patch_op> create = must(fabric_host::create_body_ops(0, 10, fresh));
  ops.insert(ops.end(), std::make_move_iterator(create.begin()),
             std::make_move_iterator(create.end()));
  static_cast<void>(must(rig->host->edit(ops)));
  CHECK(rig->link->projections_held() == 2);
  // Nothing changes before the slot.
  CHECK(must(w.get<rtt::bnd_body>(before[1]))->mass == fabric_host::seed_body(1).mass);

  static_cast<void>(must(rig->step()));
  const link::link_stats st = rig->link->stats();
  CHECK(st.rebase_updated == 1);
  CHECK(st.rebase_retired == 1);
  CHECK(st.rebase_created == 1);
  CHECK(rig->link->current_generation() == rig->host->projection().generation());

  for (std::uint32_t i = 0; i < 10; ++i) {
    if (i == 2) {
      continue;
    }
    CHECK(must(rig->entity_of(fabric_host::body_id(i))) == before[i]);
    CHECK(w.is_alive(before[i]));
  }
  CHECK_FALSE(w.is_alive(before[2]));
  CHECK_FALSE(rig->entity_of(fabric_host::body_id(2)).has_value());

  const rtt::bnd_body *one = must(w.get<rtt::bnd_body>(before[1]));
  CHECK(one->mass == 42.5F);
  CHECK(one->count == fabric_host::seed_body(1).count);
  CHECK(one->spare == 77U);
  CHECK(changed_tick_of(w, rig->body_component, before[1]) == tick_before);

  const ecs::entity ten = must(rig->entity_of(fabric_host::body_id(10)));
  const rtt::bnd_body *created = must(w.get<rtt::bnd_body>(ten));
  CHECK(created->mass == 9.0F);
  CHECK(created->count == 4U);
  CHECK(created->alive);
  CHECK(created->heat == -2);
  CHECK(rig->link->entities().size() == 10);

  // The projection the link already moved past.
  const sl::result<void> stale = rig->link->post_projection(old);
  REQUIRE_FALSE(stale.has_value());
  CHECK(stale.error().code == sl::errc::projection_stale);
  const sl::result<void> same = rig->link->post_projection(rig->host->projection());
  REQUIRE_FALSE(same.has_value());
  CHECK(same.error().code == sl::errc::projection_stale);
  CHECK(rig->link->projections_held() == 1);
}

TEST_CASE("RT-INV-BND-03: an unchanged projection rebases to nothing") {
  rtt::rig_options o;
  o.host.levels = 1;
  o.host.bodies = 4;
  const auto rig = must(rtt::link_rig::make(o));
  static_cast<void>(must(rig->ingest()));
  must_ok(rig->build());
  const ecs::entity e0 = must(rig->entity_of(fabric_host::body_id(0)));
  // A commit outside the bound fields' records: a new level name only.
  std::vector<fab::patch_op> ops;
  ops.push_back(fab::op_set(fabric_host::level_id(0), {fab::path_segment::field(1)},
                            must(fab::value::string("renamed"))));
  static_cast<void>(must(rig->host->edit(ops)));
  static_cast<void>(must(rig->step()));
  const link::link_stats st = rig->link->stats();
  CHECK(st.rebase_updated == 0);
  CHECK(st.rebase_retired == 0);
  CHECK(st.rebase_created == 0);
  CHECK(must(rig->entity_of(fabric_host::body_id(0))) == e0);
  CHECK(rig->link->projections_held() == 1);
}
