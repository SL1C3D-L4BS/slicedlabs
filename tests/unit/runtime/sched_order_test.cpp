// RT-INV-SCHED-03 (SL-ARCH-003 ARCH3-R123): conflicting systems run in constraint order, then
// registration order. Five systems writing one component, registered a, b, c, d, e with
// "b after d" and "e before a", order as Kahn's algorithm with the earliest-registered ready
// system first: c, d, b, e, a (system_index 0..4), and every frame runs them in that order on 4
// workers. Systems without constraints keep registration order; a constraint between systems
// that do not conflict sets only their system_index. The indices are the same on 1 and 4 workers
// and across builds. A before/after name that is no system of the phase is system_unknown; add
// refuses a used name, a 257th system of a phase and any add after build.

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/frame.hpp"

#include "../../../engine/runtime/testing/sim_fixture.hpp"

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace ecs = sl::runtime::ecs;
namespace rtt = sl::runtime::testing;
using sl::errc;

constexpr std::size_t max_systems = 8;

struct order_state {
  std::atomic<std::uint32_t> seq{0};
  std::array<std::atomic<std::uint32_t>, max_systems> at{};
};

struct order_body {
  order_state *s = nullptr;
  std::uint32_t self = 0;

  void operator()(ecs::system_context & /*ctx*/) const noexcept {
    s->at.at(self).store(s->seq.fetch_add(1));
  }
};

ecs::access_set writer(const rtt::sched_rig &r, std::uint32_t kind) {
  ecs::access_set a;
  a.components.writes.push_back(r.ids.ids.at(kind));
  return a;
}

std::uint32_t add(rtt::sched_rig &r, order_state &s, std::string_view name, ecs::access_set access,
                  std::vector<std::string_view> before, std::vector<std::string_view> after,
                  rt::phase ph = rt::phase::tick) {
  auto id = r.systems->add(ecs::system_desc{
      .name = name,
      .ph = ph,
      .access = std::move(access),
      .before = std::move(before),
      .after = std::move(after),
      .fn = order_body{.s = &s, .self = static_cast<std::uint32_t>(r.systems->size())},
      .chunked = false,
      .queries = {}});
  REQUIRE(id.has_value());
  return id.value_or(0);
}

std::unique_ptr<rtt::sched_rig> rig(std::uint32_t workers) {
  auto made = rtt::sched_rig::make(workers);
  REQUIRE(made.has_value());
  return std::move(*made);
}

// The five conflicting systems of the first case.
void add_chain(rtt::sched_rig &r, order_state &s) {
  add(r, s, "a", writer(r, 0), {}, {});
  add(r, s, "b", writer(r, 0), {}, {"d"});
  add(r, s, "c", writer(r, 0), {}, {});
  add(r, s, "d", writer(r, 0), {}, {});
  add(r, s, "e", writer(r, 0), {"a"}, {});
}

std::vector<std::uint16_t> indices(const rtt::sched_rig &r) {
  std::vector<std::uint16_t> out;
  for (std::uint32_t id = 0; id < r.systems->size(); ++id) {
    auto index = r.systems->system_index(id);
    REQUIRE(index.has_value());
    out.push_back(index.value_or(0));
  }
  return out;
}

} // namespace

TEST_CASE("RT-INV-SCHED-03: conflicting systems run in constraint order, then registration order") {
  auto r = rig(4);
  order_state s;
  add_chain(*r, s);
  REQUIRE(r->build().has_value());
  CHECK(indices(*r) == std::vector<std::uint16_t>{4, 2, 0, 1, 3});
  const std::span<const std::uint32_t> order = r->systems->order(rt::phase::tick);
  CHECK(std::vector<std::uint32_t>(order.begin(), order.end()) ==
        std::vector<std::uint32_t>{2, 3, 1, 4, 0});
  for (std::uint32_t f = 0; f < 20; ++f) {
    auto st = r->step();
    REQUIRE(st.has_value());
    if (st->ticks_run == 0) {
      continue;
    }
    for (std::size_t i = 1; i < order.size(); ++i) {
      CHECK(s.at.at(order[i - 1]).load() < s.at.at(order[i]).load());
    }
  }
  CHECK(s.seq.load() > 0);
  CHECK(r->systems->failures() == 0);
}

TEST_CASE("RT-INV-SCHED-03: without constraints the index is the registration order") {
  auto r = rig(2);
  order_state s;
  add(*r, s, "p", writer(*r, 0), {}, {});
  add(*r, s, "q", writer(*r, 1), {}, {});
  add(*r, s, "u", writer(*r, 2), {}, {});
  // v does not conflict with p: the constraint moves only its index.
  add(*r, s, "v", writer(*r, 3), {"p"}, {});
  REQUIRE(r->build().has_value());
  // p waits for v; q, u and v are ready first and go in registration order.
  CHECK(indices(*r) == std::vector<std::uint16_t>{3, 0, 1, 2});
  for (std::uint32_t id = 0; id < 4; ++id) {
    CHECK(r->systems->edges(id).empty());
  }
}

TEST_CASE("RT-INV-SCHED-03: system_index is deterministic across worker counts and builds") {
  std::vector<std::vector<std::uint16_t>> seen;
  for (const std::uint32_t workers : {1U, 4U, 1U, 4U}) {
    auto r = rig(workers);
    order_state s;
    add_chain(*r, s);
    add(*r, s, "x", writer(*r, 1), {}, {"c2"}, rt::phase::post_tick);
    add(*r, s, "c2", writer(*r, 1), {}, {}, rt::phase::post_tick);
    REQUIRE(r->build().has_value());
    seen.push_back(indices(*r));
  }
  for (const std::vector<std::uint16_t> &v : seen) {
    CHECK(v == seen.front());
  }
}

TEST_CASE("RT-INV-SCHED-03: an unknown before or after name is system_unknown") {
  {
    auto r = rig(1);
    order_state s;
    add(*r, s, "a", writer(*r, 0), {}, {"ghost"});
    auto built = r->systems->build(*r->frames, *r->world, *r->queue);
    REQUIRE_FALSE(built.has_value());
    CHECK(built.error().code == errc::system_unknown);
    CHECK(built.error().message.find("ghost") != std::string::npos);
    CHECK_FALSE(r->systems->built());
  }
  {
    auto r = rig(1);
    order_state s;
    add(*r, s, "early", writer(*r, 0), {}, {}, rt::phase::pre_tick);
    add(*r, s, "late", writer(*r, 0), {"early"}, {}, rt::phase::post_tick);
    auto built = r->systems->build(*r->frames, *r->world, *r->queue);
    REQUIRE_FALSE(built.has_value());
    CHECK(built.error().code == errc::system_unknown); // another phase's system
  }
}

TEST_CASE("RT-INV-SCHED-03: add refuses a used name, a 257th system and adds after build") {
  auto r = rig(1);
  order_state s;
  add(*r, s, "one", writer(*r, 0), {}, {});
  auto again = r->systems->add(ecs::system_desc{.name = "one",
                                                .ph = rt::phase::tick,
                                                .access = {},
                                                .before = {},
                                                .after = {},
                                                .fn = order_body{.s = &s, .self = 0},
                                                .chunked = false,
                                                .queries = {}});
  REQUIRE_FALSE(again.has_value());
  CHECK(again.error().code == errc::already_exists);
  auto wrong_phase = r->systems->add(ecs::system_desc{.name = "boundary",
                                                      .ph = rt::phase::end_frame,
                                                      .access = {},
                                                      .before = {},
                                                      .after = {},
                                                      .fn = order_body{.s = &s, .self = 0},
                                                      .chunked = false,
                                                      .queries = {}});
  REQUIRE_FALSE(wrong_phase.has_value());
  CHECK(wrong_phase.error().code == errc::invalid_argument);
  for (std::uint32_t i = 1; i < ecs::max_systems_per_phase; ++i) {
    const std::string name = "fill." + std::to_string(i);
    auto id = r->systems->add(ecs::system_desc{.name = name,
                                               .ph = rt::phase::observe,
                                               .access = {},
                                               .before = {},
                                               .after = {},
                                               .fn = order_body{.s = &s, .self = 0},
                                               .chunked = false,
                                               .queries = {}});
    REQUIRE(id.has_value());
  }
  auto last = r->systems->add(ecs::system_desc{.name = "fill.last",
                                               .ph = rt::phase::observe,
                                               .access = {},
                                               .before = {},
                                               .after = {},
                                               .fn = order_body{.s = &s, .self = 0},
                                               .chunked = false,
                                               .queries = {}});
  REQUIRE(last.has_value()); // the 256th of observe
  auto over = r->systems->add(ecs::system_desc{.name = "fill.over",
                                               .ph = rt::phase::observe,
                                               .access = {},
                                               .before = {},
                                               .after = {},
                                               .fn = order_body{.s = &s, .self = 0},
                                               .chunked = false,
                                               .queries = {}});
  REQUIRE_FALSE(over.has_value());
  CHECK(over.error().code == errc::invalid_argument);
  REQUIRE(r->build().has_value());
  auto late = r->systems->add(ecs::system_desc{.name = "late",
                                               .ph = rt::phase::tick,
                                               .access = {},
                                               .before = {},
                                               .after = {},
                                               .fn = order_body{.s = &s, .self = 0},
                                               .chunked = false,
                                               .queries = {}});
  REQUIRE_FALSE(late.has_value());
  CHECK(late.error().code == errc::schedule_frozen);
}

TEST_CASE("RT-INV-SCHED-03: each system's last run lives in last_run_ticks under phase x 256 + "
          "index") {
  auto r = rig(4);
  order_state s;
  add_chain(*r, s);
  REQUIRE(r->build().has_value());
  CHECK(r->systems->last_runs().entries().empty());
  bool ran = false;
  for (std::uint32_t f = 0; f < 20 && !ran; ++f) {
    auto st = r->step();
    REQUIRE(st.has_value());
    ran = st->ticks_run > 0;
  }
  REQUIRE(ran);
  // the order is unchanged and every system recorded its own write tick, one apart by index
  CHECK(indices(*r) == std::vector<std::uint16_t>{4, 2, 0, 1, 3});
  const auto entries = r->systems->last_runs().entries();
  REQUIRE(entries.size() == 5);
  const auto base = ecs::last_run_key(static_cast<std::uint8_t>(rt::phase::tick), 0);
  for (std::uint16_t i = 0; i < 5; ++i) {
    INFO("system_index " << i);
    CHECK(entries.at(i).system == base + i);
    CHECK(entries.at(i).tick == entries.at(0).tick + i);
  }
}
