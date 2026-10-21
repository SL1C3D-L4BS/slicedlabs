// RT-INV-BND-05 (SL-ARCH-003 ARCH3-R154, ADR-0027): echo suppression. One runtime write goes
// around the loop (observe, propose, commit through testing::fabric_host, rebase onto the new
// head) and then, with no further runtime writes, 300 frames (scaled by SL_RUNTIME_STRESS_SCALE,
// at least 30) propose zero patches: the rebase's document writes stamp no changed tick and the
// values the link reads back equal the projection it holds.

#include "sl/core/error.hpp"
#include "sl/runtime/bridge.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"

#include "../../../engine/runtime/testing/fabric_host.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
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

void must_ok(const sl::result<void> &r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
}

std::uint32_t scaled(double base, std::uint32_t floor) {
  double scale = 1.0;
  if (const char *env = std::getenv("SL_RUNTIME_STRESS_SCALE"); env != nullptr && *env != '\0') {
    scale = std::strtod(env, nullptr);
  }
  return std::max(floor, static_cast<std::uint32_t>(base * scale));
}

} // namespace

TEST_CASE("RT-INV-BND-05: after one round trip, idle frames propose nothing") {
  rtt::rig_options o;
  o.host.levels = 2;
  o.host.bodies = 16;
  const auto rig = must(rtt::link_rig::make(o));
  static_cast<void>(must(rig->ingest()));
  must_ok(rig->link->observe_component(rig->body_component));
  must_ok(rig->build());
  static_cast<void>(must(rig->step())); // the ingested rows are read once and suppressed
  CHECK(rig->link->stats().proposed_patches == 0);
  CHECK(rig->host->commits() == 0);

  // The round trip: one runtime write, committed and rebased within the frame.
  ecs::world &w = *rig->stack->world;
  const ecs::entity e = must(rig->entity_of(fabric_host::body_id(3)));
  rtt::bnd_body body = *must(w.get<rtt::bnd_body>(e));
  body.mass = 64.0F;
  body.count = 99;
  must_ok(w.set(e, body));
  static_cast<void>(must(rig->step()));
  const link::link_stats trip = rig->link->stats();
  CHECK(trip.proposed_patches == 1);
  CHECK(trip.proposed_changes == 2);
  CHECK(trip.rejected == 0);
  CHECK(rig->host->commits() == 1);
  CHECK(rig->link->current_generation() == rig->host->projection().generation());
  CHECK(rig->link->projections_held() == 1);
  CHECK(must(w.get<rtt::bnd_body>(e))->mass == 64.0F);

  const std::uint32_t frames = scaled(300, 30);
  for (std::uint32_t f = 0; f < frames; ++f) {
    static_cast<void>(must(rig->step()));
  }
  const link::link_stats idle = rig->link->stats();
  CHECK(idle.proposed_patches == trip.proposed_patches);
  CHECK(idle.proposed_changes == trip.proposed_changes);
  CHECK(idle.rejected == 0);
  CHECK(rig->host->commits() == 1);
  CHECK(must(rig->entity_of(fabric_host::body_id(3))) == e);
}
