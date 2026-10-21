// RT-INV-BND-06 (SL-ARCH-003 ARCH3-R158, ADR-0027): the link holds at most two projections (the
// current one and one pending), exactly one once a rebase has run, and reports their summed body
// bytes as the external fabric.projection memory: after ingest the current projection's, while a
// post is pending both, after the rebase the new current projection's only.

#include "sl/core/error.hpp"
#include "sl/fabric/boundary.hpp"
#include "sl/fabric/deps.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/runtime/bridge.hpp"

#include "../../../engine/runtime/testing/fabric_host.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace link = sl::runtime::link;
namespace rtt = sl::runtime::testing;
namespace fab = sl::fabric;
using rtt::fabric_host;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

void must_ok(const sl::result<void> &r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
}

/// The body bytes of every record of `p`, summed here rather than by projection_bytes.
std::uint64_t summed(const fab::document_projection &p) {
  std::uint64_t total = 0;
  must_ok(p.for_each([&total](const fab::record_view &r) -> sl::result<void> {
    total += r.body == nullptr ? 0 : link::body_bytes(*r.body);
    return {};
  }));
  return total;
}

} // namespace

TEST_CASE("RT-INV-BND-06: one projection after a rebase, its body bytes reported external") {
  rtt::rig_options o;
  o.host.levels = 2;
  o.host.bodies = 6;
  o.host.markers = 2;
  const auto rig = must(rtt::link_rig::make(o));
  CHECK(rig->link->projections_held() == 0);
  CHECK(rig->external_projection_bytes() == 0);

  static_cast<void>(must(rig->ingest()));
  const fab::document_projection first = rig->host->projection();
  const std::uint64_t first_bytes = summed(first);
  CHECK(first_bytes > 0);
  CHECK(link::projection_bytes(first) == first_bytes);
  CHECK(rig->link->projections_held() == 1);
  CHECK(rig->external_projection_bytes() == first_bytes);
  CHECK(rig->link->stats().projection_bytes == first_bytes);
  must_ok(rig->build());

  // A new body makes the next projection larger.
  const rtt::bnd_body fresh{.mass = 1.5F, .count = 2, .alive = true, .heat = 0, .spare = 0};
  static_cast<void>(must(rig->host->edit(must(fabric_host::create_body_ops(1, 6, fresh)))));
  const fab::document_projection second = rig->host->projection();
  const std::uint64_t second_bytes = summed(second);
  CHECK(second_bytes > first_bytes);
  CHECK(rig->link->projections_held() == 2);
  // The report is taken on the owner thread at the slots: a post (any thread) does not move it.
  CHECK(rig->external_projection_bytes() == first_bytes);

  static_cast<void>(must(rig->step()));
  CHECK(rig->link->projections_held() == 1);
  CHECK(rig->link->current_generation() == second.generation());
  CHECK(rig->external_projection_bytes() == second_bytes);
  CHECK(rig->link->stats().projection_bytes == second_bytes);

  // Idle frames hold the one projection and report the same bytes.
  for (int f = 0; f < 3; ++f) {
    static_cast<void>(must(rig->step()));
  }
  CHECK(rig->link->projections_held() == 1);
  CHECK(rig->external_projection_bytes() == second_bytes);
}

TEST_CASE("RT-INV-BND-06: a later post replaces the pending projection") {
  rtt::rig_options o;
  o.host.levels = 1;
  o.host.bodies = 2;
  const auto rig = must(rtt::link_rig::make(o));
  static_cast<void>(must(rig->ingest()));
  const std::uint64_t first_bytes = summed(rig->host->projection());
  must_ok(rig->build());
  const rtt::bnd_body v{.mass = 3.0F, .count = 1, .alive = false, .heat = 5, .spare = 0};
  static_cast<void>(must(rig->host->edit(must(fabric_host::create_body_ops(0, 2, v)))));
  static_cast<void>(must(rig->host->edit(must(fabric_host::create_body_ops(0, 3, v)))));
  // Still two: the second post replaced the first pending one.
  CHECK(rig->link->projections_held() == 2);
  const fab::document_projection head = rig->host->projection();
  CHECK(rig->external_projection_bytes() == first_bytes);
  static_cast<void>(must(rig->step()));
  CHECK(rig->link->projections_held() == 1);
  CHECK(rig->external_projection_bytes() == summed(head));
  CHECK(rig->link->stats().rebase_created == 2);
}
