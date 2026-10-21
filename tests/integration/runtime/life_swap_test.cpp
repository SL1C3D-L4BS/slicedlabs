// RT-INV-LIFE-07 (SL-ARCH-003 ARCH3-R031, ADR-0028): a swap applies only at end_frame, restart
// classes return restart_required, and a failing health check restores the pre-swap snapshot so
// the next 60 ticks hash equal to a run without the swap. A request made between frames (from
// another thread) runs nothing until the next end_frame, where apply runs in the swap slot (after
// the snapshot slot, before lifecycle_requests) outside every phase; a passing health check
// commits after health_ticks ticks. component_layout, allocator, scheduler and runtime_abi end at
// once as restart_required and change nothing. A data swap whose health check fails at check n
// (swept, scaled by SL_RUNTIME_STRESS_SCALE) rolls back to the pre-swap world: from there the next
// 60 applied ticks hash equal to a run that never swapped. A failing apply rolls back too.

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/ecs/query.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/replay.hpp"
#include "sl/runtime/runtime.hpp"
#include "sl/runtime/snapshot.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace ecs = sl::runtime::ecs;
namespace det = sl::runtime::det;
namespace rtt = sl::runtime::testing;

constexpr rt::field_flags kept = rt::field_flags::snapshot | rt::field_flags::hashed;
constexpr rt::time::tick swap_at = 10;

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

struct step_body {
  ecs::query<ecs::write<counter>> q;
  void operator()(ecs::system_context &ctx) noexcept {
    ctx.for_each(q, [](ecs::entity /*e*/, counter &c) noexcept {
      c.value = (c.value * 6364136223846793005ULL) + 1442695040888963407ULL;
    });
  }
};

struct rig {
  explicit rig(std::uint32_t id) : plat(id) {
    rt::runtime_config cfg;
    cfg.instance_id = id;
    cfg.workers = 3;
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
    r->frames().add_boundary_hook(
        rt::boundary_slot::snapshot,
        [this](const rt::boundary_context & /*bc*/) noexcept { trace.emplace_back("snapshot"); });
    r->frames().add_boundary_hook(
        rt::boundary_slot::lifecycle_requests,
        [this](const rt::boundary_context & /*bc*/) noexcept { trace.emplace_back("lifecycle"); });
    for (std::uint32_t i = 0; i < 256; ++i) {
      REQUIRE(r->world().create_with(counter{.value = std::uint64_t{i} * 31U}).has_value());
    }
  }

  sl::result<rt::frame_stats> frame() {
    plat.clock().advance(std::chrono::milliseconds(20));
    trace.clear();
    return r->run_frame();
  }

  void run_to(rt::time::tick t) {
    while (now() < t) {
      REQUIRE(frame().has_value());
    }
  }

  [[nodiscard]] rt::time::tick now() const { return r->frames().current().now; }

  [[nodiscard]] sl::fabric::digest hash() const {
    auto h = rt::state_hash(r->world(), r->components(), r->types(),
                            std::span<const det::snapshot_contributor *const>{});
    REQUIRE(h.has_value());
    return *h;
  }

  rtt::scripted_platform plat;
  std::vector<std::string> trace;
  std::unique_ptr<rt::runtime> r;
};

/// A run without any swap: the hash after every applied tick count.
std::map<rt::time::tick, sl::fabric::digest> no_swap(rt::time::tick ticks) {
  rig g(700);
  std::map<rt::time::tick, sl::fabric::digest> out;
  while (g.now() < ticks) {
    REQUIRE(g.frame().has_value());
    out[g.now()] = g.hash();
  }
  REQUIRE(g.r->shutdown().has_value());
  return out;
}

/// A data swap whose apply adds an entity (the world visibly changes).
rt::swap_request data_swap(int &applied, std::vector<std::string> *trace) {
  rt::swap_request req;
  req.cls = rt::swap_class::data;
  req.apply = [&applied, trace](rt::runtime &r) -> sl::result<void> {
    ++applied;
    if (trace != nullptr) {
      trace->emplace_back(r.world().phase_active() ? "apply-in-phase" : "apply");
    }
    auto e = r.world().create_with(counter{.value = 4242});
    if (!e) {
      return std::unexpected(e.error());
    }
    return {};
  };
  return req;
}

} // namespace

TEST_CASE("RT-INV-LIFE-07: a swap applies only at end_frame and commits after health_ticks") {
  rig g(701);
  g.run_to(swap_at);
  int applied = 0;
  int checks = 0;
  rt::swap_request req = data_swap(applied, &g.trace);
  req.health_ticks = 60;
  req.health_check = [&checks](rt::runtime & /*r*/, rt::time::tick /*t*/) {
    ++checks;
    return true;
  };
  std::thread([&] { g.r->request_swap(std::move(req)); }).join();
  CHECK(g.r->swap_status() == rt::swap_state::pending);
  CHECK(applied == 0); // nothing between frames
  REQUIRE(g.frame().has_value());
  CHECK(applied == 1);
  CHECK(g.trace == std::vector<std::string>{"snapshot", "apply", "lifecycle"});
  CHECK(g.r->swap_status() == rt::swap_state::pending);
  const rt::time::tick applied_tick = g.now();
  while (g.r->swap_status() == rt::swap_state::pending && g.now() < applied_tick + 100) {
    REQUIRE(g.frame().has_value());
  }
  CHECK(g.r->swap_status() == rt::swap_state::committed);
  CHECK(g.r->swap_code() == sl::errc::ok);
  CHECK(g.now() == applied_tick + 60);
  CHECK(checks == 60);
  CHECK(applied == 1);
  REQUIRE(g.r->shutdown().has_value());
}

TEST_CASE("RT-INV-LIFE-07: restart classes return restart_required and change nothing") {
  const auto reference = no_swap(swap_at + 12);
  rig g(702);
  g.run_to(swap_at);
  const std::array<rt::swap_class, 4> restart{rt::swap_class::component_layout,
                                              rt::swap_class::allocator, rt::swap_class::scheduler,
                                              rt::swap_class::runtime_abi};
  int applied = 0;
  for (const rt::swap_class cls : restart) {
    INFO("class " << static_cast<int>(cls));
    rt::swap_request req = data_swap(applied, nullptr);
    req.cls = cls;
    g.r->request_swap(std::move(req));
    REQUIRE(g.frame().has_value());
    CHECK(g.r->swap_status() == rt::swap_state::restart_required);
    CHECK(g.r->swap_code() == sl::errc::restart_required);
    CHECK(g.r->state() == rt::rt_state::running);
    CHECK(g.hash() == reference.at(g.now()));
  }
  CHECK(applied == 0);
  REQUIRE(g.r->shutdown().has_value());
}

TEST_CASE("RT-INV-LIFE-07: a failing health check restores the pre-swap snapshot") {
  const std::uint32_t sweep = scaled(4);
  const auto reference = no_swap(swap_at + 70 + sweep);
  for (std::uint32_t fail_at = 1; fail_at <= sweep; ++fail_at) {
    INFO("health check fails at call " << fail_at);
    rig g(710 + fail_at);
    g.run_to(swap_at);
    int applied = 0;
    std::uint32_t checks = 0;
    rt::swap_request req = data_swap(applied, nullptr);
    req.health_ticks = 60;
    req.health_check = [&checks, fail_at](rt::runtime & /*r*/, rt::time::tick /*t*/) {
      return ++checks < fail_at;
    };
    g.r->request_swap(std::move(req));
    REQUIRE(g.frame().has_value());
    const rt::time::tick pre_swap = g.now(); // the pre-swap snapshot's tick
    CHECK(applied == 1);
    CHECK(g.hash() != reference.at(pre_swap)); // the swap changed the world
    while (g.r->swap_status() == rt::swap_state::pending) {
      REQUIRE(g.frame().has_value());
    }
    CHECK(g.r->swap_status() == rt::swap_state::rolled_back);
    CHECK(g.r->swap_code() == sl::errc::swap_rejected);
    CHECK(checks == fail_at);
    // back to the pre-swap world; the next 60 ticks follow the run without the swap
    rt::time::tick ticks = pre_swap;
    CHECK(g.hash() == reference.at(ticks));
    while (ticks < pre_swap + 60) {
      const auto ran = g.frame();
      REQUIRE(ran.has_value());
      ticks += ran->ticks_run;
      INFO("applied " << ticks);
      CHECK(g.hash() == reference.at(ticks));
    }
    CHECK(g.r->state() == rt::rt_state::running);
    REQUIRE(g.r->shutdown().has_value());
  }
}

TEST_CASE("RT-INV-LIFE-07: a failing apply rolls back with its error") {
  const auto reference = no_swap(swap_at + 4);
  rig g(720);
  g.run_to(swap_at);
  rt::swap_request req;
  req.cls = rt::swap_class::gameplay_function;
  req.apply = [](rt::runtime &r) -> sl::result<void> {
    [[maybe_unused]] const auto made = r.world().create_with(counter{.value = 1});
    return sl::make_error(sl::errc::invalid_argument, "life: refused apply");
  };
  g.r->request_swap(std::move(req));
  REQUIRE(g.frame().has_value());
  CHECK(g.r->swap_status() == rt::swap_state::rolled_back);
  CHECK(g.r->swap_code() == sl::errc::invalid_argument);
  CHECK(g.hash() == reference.at(g.now()));
  REQUIRE(g.r->shutdown().has_value());
}
