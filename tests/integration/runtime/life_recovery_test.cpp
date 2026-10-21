// RT-INV-LIFE-06 (SL-ARCH-003 ARCH3-R029, ADR-0028): restore_snapshot recovery ends with the
// fault-free hash, more than max_recoveries faults in a window stop with recovery_failed, and the
// default policy is stop. A world of counters steps every tick; a system fails on request. Each
// recovery restores the newest snapshot at or before the fault tick; the logical tick is not
// rewound (WO-089), so the run is compared by applied ticks: after every frame the world hashes
// equal to a fault-free run after as many applied ticks. Faults beyond max_recoveries inside
// recovery_window_ticks end in recovery_failed (state Stopping); faults spaced wider than the
// window all recover; with no snapshot the restore is refused (recovery_failed); the default
// configuration stops (runtime_faulted). The fault sweep scales with SL_RUNTIME_STRESS_SCALE.

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/ecs/query.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/replay.hpp"
#include "sl/runtime/runtime.hpp"
#include "sl/runtime/snapshot.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <span>
#include <utility>

namespace {

namespace rt = sl::runtime;
namespace ecs = sl::runtime::ecs;
namespace det = sl::runtime::det;
namespace rtt = sl::runtime::testing;

constexpr rt::field_flags kept = rt::field_flags::snapshot | rt::field_flags::hashed;

std::uint32_t scaled(double base) {
  double scale = 1.0;
  if (const char *env = std::getenv("SL_RUNTIME_STRESS_SCALE"); env != nullptr && *env != '\0') {
    scale = std::strtod(env, nullptr);
  }
  return std::max<std::uint32_t>(1, static_cast<std::uint32_t>(base * scale));
}

struct counter {
  std::uint64_t value = 0;
};

struct control {
  std::atomic<bool> fail{false};
};

struct step_body {
  ecs::query<ecs::write<counter>> q;
  void operator()(ecs::system_context &ctx) noexcept {
    ctx.for_each(q, [](ecs::entity /*e*/, counter &c) noexcept {
      c.value = (c.value * 6364136223846793005ULL) + 1442695040888963407ULL;
    });
  }
};

struct faulty_body {
  control *ctl = nullptr;
  void operator()(ecs::system_context &ctx) const noexcept {
    if (ctl->fail.exchange(false)) {
      ctx.fail(sl::errc::job_failed);
    }
  }
};

rt::runtime_config restoring(std::uint32_t id) {
  rt::runtime_config c;
  c.instance_id = id;
  c.workers = 3;
  c.rate = {.num = 50, .den = 1};
  c.recovery = rt::recovery_policy::restore_snapshot;
  c.snapshot_every = 5;
  return c;
}

struct rig {
  explicit rig(const rt::runtime_config &cfg) : plat(cfg.instance_id) {
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
    REQUIRE(r->add_system(ecs::system_desc{.name = "life.faulty",
                                           .ph = rt::phase::tick,
                                           .access = {},
                                           .before = {},
                                           .after = {},
                                           .fn = faulty_body{.ctl = &ctl},
                                           .chunked = false,
                                           .queries = {}})
                .has_value());
    REQUIRE(r->start().has_value());
    for (std::uint32_t i = 0; i < 512; ++i) {
      REQUIRE(r->world().create_with(counter{.value = std::uint64_t{i} * 977U}).has_value());
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

  rtt::scripted_platform plat;
  control ctl;
  std::unique_ptr<rt::runtime> r;
};

/// A fault-free run's hash after every applied tick count.
std::map<rt::time::tick, sl::fabric::digest> fault_free(rt::time::tick ticks) {
  rig g(restoring(600));
  std::map<rt::time::tick, sl::fabric::digest> out;
  while (g.now() < ticks) {
    REQUIRE(g.frame().has_value());
    out[g.now()] = g.hash();
  }
  REQUIRE(g.r->shutdown().has_value());
  return out;
}

} // namespace

TEST_CASE("RT-INV-LIFE-06: restore_snapshot recovery ends with the fault-free hash") {
  const std::uint32_t faults = scaled(6);
  const rt::time::tick ticks = 40 + (static_cast<rt::time::tick>(faults) * 12);
  const auto reference = fault_free(ticks);
  for (std::uint64_t seed = 1; seed <= 3; ++seed) {
    INFO("seed " << seed);
    rt::runtime_config cfg = restoring(601);
    cfg.max_recoveries = faults;
    rig g(cfg);
    // logical tick -> applied ticks, to map a restored snapshot tick back to applied ticks
    std::map<rt::time::tick, rt::time::tick> applied_at{{0, 0}};
    rt::time::tick applied = 0;
    std::uint32_t injected = 0;
    std::uint64_t next_fault = 8 + seed;
    for (int guard = 0; guard < 4000 && applied < ticks; ++guard) {
      const bool inject = injected < faults && g.now() >= next_fault;
      if (inject) {
        g.ctl.fail.store(true);
        ++injected;
        next_fault = g.now() + 6 + (((seed * 7) + (std::uint64_t{injected} * 3)) % 9);
      }
      const auto ran = g.frame();
      REQUIRE(ran.has_value());
      if (inject) {
        REQUIRE(g.r->frame_faulted());
        REQUIRE(g.r->last_fault().has_value());
        const rt::fault_report fault = g.r->last_fault().value_or(rt::fault_report{});
        REQUIRE(fault.restored_tick.has_value());
        const rt::time::tick restored = fault.restored_tick.value_or(0);
        REQUIRE(applied_at.contains(restored));
        applied = applied_at.at(restored);
      } else {
        applied += ran->ticks_run;
      }
      applied_at[g.now()] = applied;
      INFO("applied " << applied << " tick " << g.now());
      REQUIRE(reference.contains(applied));
      CHECK(g.hash() == reference.at(applied));
    }
    CHECK(injected == faults);
    CHECK(g.r->recoveries() == faults);
    CHECK(g.r->state() == rt::rt_state::running);
    REQUIRE(g.r->shutdown().has_value());
  }
}

TEST_CASE("RT-INV-LIFE-06: more than max_recoveries faults in the window stop with "
          "recovery_failed") {
  rt::runtime_config cfg = restoring(610);
  cfg.max_recoveries = 2;
  cfg.recovery_window_ticks = 600;
  cfg.snapshot_every = 1;
  rig g(cfg);
  for (int i = 0; i < 5; ++i) {
    REQUIRE(g.frame().has_value());
  }
  for (std::uint32_t k = 1; k <= 2; ++k) {
    g.ctl.fail.store(true);
    REQUIRE(g.frame().has_value());
    CHECK(g.r->recoveries() == k);
    REQUIRE(g.frame().has_value());
  }
  g.ctl.fail.store(true);
  const auto ran = g.frame();
  REQUIRE_FALSE(ran.has_value());
  CHECK(ran.error().code == sl::errc::recovery_failed);
  CHECK(g.r->state() == rt::rt_state::stopping);
  CHECK(g.r->recoveries() == 2);
  REQUIRE(g.r->last_fault().has_value());
  const rt::fault_report fault = g.r->last_fault().value_or(rt::fault_report{});
  CHECK(fault.action == "stop");
  CHECK_FALSE(fault.restored_tick.has_value());
  REQUIRE(g.r->shutdown().has_value());
}

TEST_CASE("RT-INV-LIFE-06: faults spaced wider than the window all recover") {
  rt::runtime_config cfg = restoring(620);
  cfg.max_recoveries = 1;
  cfg.recovery_window_ticks = 5;
  cfg.snapshot_every = 1;
  rig g(cfg);
  const std::uint32_t faults = scaled(5);
  for (std::uint32_t k = 1; k <= faults; ++k) {
    for (int i = 0; i < 7; ++i) {
      REQUIRE(g.frame().has_value());
    }
    g.ctl.fail.store(true);
    REQUIRE(g.frame().has_value());
    CHECK(g.r->recoveries() == k);
  }
  CHECK(g.r->state() == rt::rt_state::running);
  REQUIRE(g.r->shutdown().has_value());
}

TEST_CASE("RT-INV-LIFE-06: the default policy is stop, and a restore without snapshot fails") {
  const rt::runtime_config defaults;
  CHECK(defaults.recovery == rt::recovery_policy::stop);
  CHECK(defaults.max_recoveries == 3);
  CHECK(defaults.recovery_window_ticks == 600);
  {
    rt::runtime_config cfg;
    cfg.instance_id = 630;
    cfg.workers = 2;
    cfg.rate = {.num = 50, .den = 1};
    rig g(cfg);
    for (int i = 0; i < 3; ++i) {
      REQUIRE(g.frame().has_value());
    }
    g.ctl.fail.store(true);
    const auto ran = g.frame();
    REQUIRE_FALSE(ran.has_value());
    CHECK(ran.error().code == sl::errc::runtime_faulted);
    CHECK(g.r->state() == rt::rt_state::stopping);
    CHECK(g.r->recoveries() == 0);
    REQUIRE(g.r->last_fault().has_value());
    CHECK(g.r->last_fault().value_or(rt::fault_report{}).action == "stop");
    REQUIRE(g.r->shutdown().has_value());
  }
  {
    rt::runtime_config cfg = restoring(631);
    cfg.snapshot_every = 0;
    rig g(cfg);
    REQUIRE(g.frame().has_value());
    REQUIRE(g.frame().has_value());
    g.ctl.fail.store(true);
    const auto ran = g.frame();
    REQUIRE_FALSE(ran.has_value());
    CHECK(ran.error().code == sl::errc::recovery_failed);
    CHECK(g.r->state() == rt::rt_state::stopping);
    REQUIRE(g.r->shutdown().has_value());
  }
}
