// RT-INV-LIFE-04 (SL-ARCH-003 ARCH3-R026, ADR-0028): suspend and resume at end_frame give the
// final state hash of an uninterrupted run and no tick accrues while suspended. A run suspended
// at frame 10 sees a 10 s clock jump while Suspended: run_frame is invalid_state, the logical
// tick does not move, the first frame after resume has delta 0 and no tick, and the run reaches
// the same tick with the hash of a run that never suspended. In SL_RUNTIME_FAULTS builds RFP9
// (suspend.request) fires at each of its visits over three frames (every phase end and every
// boundary slot): each request suspends at an end_frame and the resumed run ends on the same hash.

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/ecs/query.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/faults.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/replay.hpp"
#include "sl/runtime/runtime.hpp"
#include "sl/runtime/snapshot.hpp"

#include "../../../engine/runtime/testing/faults_arm.hpp"
#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace {

namespace rt = sl::runtime;
namespace ecs = sl::runtime::ecs;
namespace det = sl::runtime::det;
namespace rtt = sl::runtime::testing;

constexpr rt::field_flags kept = rt::field_flags::snapshot | rt::field_flags::hashed;
constexpr rt::time::tick target_tick = 40;

struct counter {
  std::uint64_t value = 0;
};

struct step_body {
  ecs::query<ecs::write<counter>> q;
  void operator()(ecs::system_context &ctx) noexcept {
    ctx.for_each(q, [](ecs::entity /*e*/, counter &c) noexcept { c.value = (c.value * 31U) + 7U; });
  }
};

struct rig {
  explicit rig(std::uint32_t id) : plat(id) {
    rt::runtime_config cfg;
    cfg.instance_id = id;
    cfg.workers = 2;
    cfg.rate = {.num = 50, .den = 1};
    auto made = rt::runtime::create(plat.get(), cfg);
    REQUIRE(made.has_value());
    r = std::move(*made);
    rt::type_builder<counter> b("life.counter");
    SL_RT_FIELD(b, counter, value, kept);
    auto t = r->types().add(b.build());
    REQUIRE(t.has_value());
    REQUIRE(r->components().add<counter>("life.counter", *t).has_value());
    auto q = ecs::query<ecs::write<counter>>::make(r->components());
    REQUIRE(q.has_value());
    ecs::access_set access;
    ecs::add_query_access(access, q->access());
    const ecs::component_access needed = q->access();
    REQUIRE(r->add_system(ecs::system_desc{.name = "life.step",
                                           .ph = rt::phase::tick,
                                           .access = std::move(access),
                                           .before = {},
                                           .after = {},
                                           .fn = step_body{.q = std::move(*q)},
                                           .chunked = false,
                                           .queries = {needed}})
                .has_value());
    REQUIRE(r->start().has_value());
    for (std::uint32_t i = 0; i < 64; ++i) {
      REQUIRE(r->world().create_with(counter{.value = i + 1U}).has_value());
    }
  }

  sl::result<rt::frame_stats> frame() {
    plat.clock().advance(std::chrono::milliseconds(20));
    return r->run_frame();
  }

  [[nodiscard]] rt::time::tick now() const { return r->frames().current().now; }

  [[nodiscard]] sl::fabric::digest hash() const {
    auto h = rt::state_hash(r->world(), r->components(), r->types(),
                            std::span<const det::snapshot_contributor *const>{});
    REQUIRE(h.has_value());
    return *h;
  }

  /// Runs frames until the logical tick reaches `t`; a suspended runtime sleeps 10 s and resumes.
  void run_to(rt::time::tick t, std::uint32_t &suspensions) {
    for (int guard = 0; guard < 1000 && now() < t; ++guard) {
      if (r->state() == rt::rt_state::suspended) {
        ++suspensions;
        const rt::time::tick frozen = now();
        plat.clock().advance(std::chrono::seconds(10));
        const auto refused = r->run_frame();
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == sl::errc::invalid_state);
        CHECK(now() == frozen);
        REQUIRE(r->resume().has_value());
        const auto first = frame();
        REQUIRE(first.has_value());
        CHECK(first->ticks_run == 0);
        CHECK(r->frames().current().delta.count() == 0);
        CHECK(now() == frozen);
        continue;
      }
      REQUIRE(frame().has_value());
    }
    REQUIRE(now() == t);
  }

  rtt::scripted_platform plat;
  std::unique_ptr<rt::runtime> r;
};

sl::fabric::digest uninterrupted() {
  rig g(401);
  std::uint32_t suspensions = 0;
  g.run_to(target_tick, suspensions);
  CHECK(suspensions == 0);
  const sl::fabric::digest h = g.hash();
  REQUIRE(g.r->shutdown().has_value());
  return h;
}

} // namespace

TEST_CASE("RT-INV-LIFE-04: a suspended and resumed run ends on the uninterrupted hash") {
  const sl::fabric::digest expected = uninterrupted();
  rig g(402);
  std::uint32_t suspensions = 0;
  g.run_to(10, suspensions);
  g.r->request_suspend();
  CHECK(g.r->state() == rt::rt_state::running); // takes effect at end_frame
  REQUIRE(g.frame().has_value());
  CHECK(g.r->state() == rt::rt_state::suspended);
  const rt::time::tick at = g.now();
  g.run_to(target_tick, suspensions);
  CHECK(suspensions == 1);
  CHECK(at == 11);
  CHECK(g.hash() == expected);
  // a suspended runtime shuts down directly
  g.r->request_suspend();
  REQUIRE(g.frame().has_value());
  CHECK(g.r->state() == rt::rt_state::suspended);
  REQUIRE(g.r->shutdown().has_value());
}

TEST_CASE("RT-INV-LIFE-04: RFP9 at every visit suspends at an end_frame and changes no hash") {
  if (!rt::faults_compiled) {
    MESSAGE("failpoints compiled out");
    return;
  }
  const sl::fabric::digest expected = uninterrupted();
  // one frame visits RFP9 at 7 phase ends (one tick) and 9 boundary slots; three frames' worth
  for (std::uint64_t k = 1; k <= 48; ++k) {
    INFO("RFP9 visit " << k);
    rig g(410);
    std::uint32_t suspensions = 0;
    g.run_to(5, suspensions);
    {
      const rtt::scoped_arm arm(rt::failpoint::rfp9, k);
      REQUIRE(arm.status().has_value());
      for (int i = 0; i < 4 && g.r->state() == rt::rt_state::running; ++i) {
        REQUIRE(g.frame().has_value());
      }
    }
    g.run_to(target_tick, suspensions);
    CHECK(suspensions == 1);
    CHECK(g.hash() == expected);
    REQUIRE(g.r->shutdown().has_value());
  }
}
