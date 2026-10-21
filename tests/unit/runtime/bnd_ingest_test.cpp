// RT-INV-BND-02 (SL-ARCH-003 ARCH3-R153, ADR-0027): ingest spawns exactly one entity per
// document-bound record in scope, the entity_map round trips record <-> entity, the bound fields
// carry the record's values, records of unbound types are skipped and counted, a second ingest of
// the same projection spawns nothing, and ingest refuses to run off the owner thread.

#include "sl/core/error.hpp"
#include "sl/fabric/id.hpp"
#include "sl/runtime/bridge.hpp"
#include "sl/runtime/ecs/entity.hpp"

#include "../../../engine/runtime/testing/fabric_host.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace {

namespace link = sl::runtime::link;
namespace rtt = sl::runtime::testing;
namespace ecs = sl::runtime::ecs;
using rtt::fabric_host;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

rtt::rig_options scene(bool bind_marker) {
  rtt::rig_options o;
  o.host.levels = 2;
  o.host.bodies = 10;
  o.host.markers = 3;
  o.bind_marker = bind_marker;
  return o;
}

} // namespace

TEST_CASE("RT-INV-BND-02: every bound record in scope gets exactly one entity") {
  const auto rig = must(rtt::link_rig::make(scene(false)));
  const link::link_stats first = must(rig->ingest());
  CHECK(first.ingested == 10);
  CHECK(first.skipped_unbound == 3 + 2); // the markers and the levels
  CHECK(rig->link->entities().size() == 10);
  CHECK(rig->link->stats().ingested == 10);
  CHECK(rig->link->current_generation() == rig->host->projection().generation());

  const ecs::world &w = *rig->stack->world;
  for (std::uint32_t i = 0; i < 10; ++i) {
    const sl::fabric::record_id record = fabric_host::body_id(i);
    const ecs::entity e = must(rig->entity_of(record));
    CHECK(w.is_alive(e));
    CHECK(must(rig->link->entities().key_of(e)) == record);
    const rtt::bnd_body *body = must(w.get<rtt::bnd_body>(e));
    const rtt::bnd_body want = fabric_host::seed_body(i);
    CHECK(body->mass == want.mass);
    CHECK(body->count == want.count);
    CHECK(body->alive == want.alive);
    CHECK(body->heat == want.heat);
    CHECK(body->spare == 0U); // runtime-only bytes start at zero
  }
  for (std::uint32_t i = 0; i < 3; ++i) {
    CHECK_FALSE(rig->entity_of(fabric_host::marker_id(i)).has_value());
  }
  CHECK_FALSE(rig->entity_of(fabric_host::level_id(0)).has_value());

  // The same projection again: everything is mapped, nothing spawns.
  const link::link_stats again = must(rig->ingest());
  CHECK(again.ingested == 0);
  CHECK(rig->link->entities().size() == 10);
  CHECK(rig->link->stats().ingested == 10);
}

TEST_CASE("RT-INV-BND-02: binding a second type ingests its records too") {
  const auto rig = must(rtt::link_rig::make(scene(true)));
  const link::link_stats st = must(rig->ingest());
  CHECK(st.ingested == 13);
  CHECK(st.skipped_unbound == 2);
  const ecs::world &w = *rig->stack->world;
  for (std::uint32_t i = 0; i < 3; ++i) {
    const ecs::entity e = must(rig->entity_of(fabric_host::marker_id(i)));
    CHECK(must(w.get<rtt::bnd_marker>(e))->level == i);
    CHECK_FALSE(w.get<rtt::bnd_body>(e).has_value());
  }
}

TEST_CASE("RT-INV-BND-02: ingest off the owner thread is wrong_thread") {
  const auto rig = must(rtt::link_rig::make(scene(false)));
  const link::document_source source = must(link::document_source::make(rig->host->projection()));
  sl::result<link::link_stats> off = link::link_stats{};
  std::thread other([&] { off = rig->link->ingest(source); });
  other.join();
  REQUIRE_FALSE(off.has_value());
  CHECK(off.error().code == sl::errc::wrong_thread);
  CHECK(rig->link->entities().size() == 0);
  CHECK(must(rig->link->ingest(source)).ingested == 10);
}
