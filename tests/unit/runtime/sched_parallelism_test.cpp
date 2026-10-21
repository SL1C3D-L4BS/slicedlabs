// RT-INV-SCHED-02 (SL-ARCH-003 ARCH3-R122): systems that do not conflict overlap when 2 or more
// workers exist. Two observe systems writing different components (and reading the same one) on
// 2 workers, and four systems writing different resources on 4 workers, each wait for all the
// others to arrive (up to a deadline): every frame they all meet, on different threads, and the
// scheduler gave them no edge. A conflicting pair gets one edge and never meets.

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/frame.hpp"

#include "../../../engine/runtime/testing/sim_fixture.hpp"

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace ecs = sl::runtime::ecs;
namespace rtt = sl::runtime::testing;

constexpr std::size_t max_meet = 4;

struct meet_state {
  std::atomic<std::uint32_t> arrived{0};
  std::atomic<std::uint32_t> met{0};
  std::array<std::atomic<std::thread::id>, max_meet> threads{};
  std::uint32_t expected = 2;
  std::chrono::milliseconds patience{500};
};

struct meet_body {
  meet_state *m = nullptr;
  std::uint32_t self = 0;

  void operator()(ecs::system_context & /*ctx*/) const noexcept {
    m->threads.at(self).store(std::this_thread::get_id());
    m->arrived.fetch_add(1);
    const auto deadline = std::chrono::steady_clock::now() + m->patience;
    while (m->arrived.load() < m->expected && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    if (m->arrived.load() >= m->expected) {
      m->met.fetch_add(1);
    }
  }
};

ecs::access_set component_writer(const rtt::sched_rig &r, std::uint32_t write_kind,
                                 std::uint32_t read_kind) {
  ecs::access_set a;
  a.components.writes.push_back(r.ids.ids.at(write_kind));
  a.components.reads.push_back(r.ids.ids.at(read_kind));
  return a;
}

ecs::access_set resource_writer(const rtt::sched_rig &r, std::uint32_t key) {
  ecs::access_set a;
  a.components.reads.push_back(r.ids.ids.at(0));
  a.resource_writes.push_back(ecs::resource_key{key});
  return a;
}

void add_meet(rtt::sched_rig &r, meet_state &m, std::uint32_t self, ecs::access_set access) {
  const std::string name = "meet." + std::to_string(self);
  auto id = r.systems->add(ecs::system_desc{.name = name,
                                            .ph = rt::phase::observe,
                                            .access = std::move(access),
                                            .before = {},
                                            .after = {},
                                            .fn = meet_body{.m = &m, .self = self},
                                            .chunked = false,
                                            .queries = {}});
  REQUIRE(id.has_value());
}

// Runs `frames` frames; returns how many frames ran every meeter on a distinct thread.
std::uint32_t run_meets(rtt::sched_rig &r, meet_state &m, std::uint32_t frames) {
  std::uint32_t distinct = 0;
  for (std::uint32_t f = 0; f < frames; ++f) {
    m.arrived.store(0);
    auto s = r.step();
    REQUIRE(s.has_value());
    CHECK(s->phases[static_cast<std::size_t>(rt::phase::observe)].jobs == m.expected);
    std::set<std::thread::id> seen;
    for (std::uint32_t i = 0; i < m.expected; ++i) {
      seen.insert(m.threads.at(i).load());
    }
    distinct += seen.size() == m.expected ? 1U : 0U;
  }
  return distinct;
}

} // namespace

TEST_CASE("RT-INV-SCHED-02: systems writing different components overlap on 2 workers") {
  auto made = rtt::sched_rig::make(2);
  REQUIRE(made.has_value());
  rtt::sched_rig &r = **made;
  meet_state m;
  add_meet(r, m, 0, component_writer(r, 0, 6)); // writes position, reads aligned
  add_meet(r, m, 1, component_writer(r, 1, 6)); // writes velocity, reads aligned
  REQUIRE(r.build().has_value());
  CHECK(r.systems->edges(0).empty());
  CHECK(r.systems->edges(1).empty());
  constexpr std::uint32_t frames = 5;
  CHECK(run_meets(r, m, frames) == frames);
  CHECK(m.met.load() == 2 * frames);
  CHECK(r.systems->failures() == 0);
}

TEST_CASE("RT-INV-SCHED-02: four systems writing different resources overlap on 4 workers") {
  auto made = rtt::sched_rig::make(4);
  REQUIRE(made.has_value());
  rtt::sched_rig &r = **made;
  meet_state m;
  m.expected = 4;
  for (std::uint32_t i = 0; i < 4; ++i) {
    add_meet(r, m, i, resource_writer(r, i + 1));
  }
  REQUIRE(r.build().has_value());
  for (std::uint32_t i = 0; i < 4; ++i) {
    CHECK(r.systems->edges(i).empty());
  }
  constexpr std::uint32_t frames = 3;
  CHECK(run_meets(r, m, frames) == frames);
  CHECK(m.met.load() == 4 * frames);
}

TEST_CASE("RT-INV-SCHED-02: a conflicting pair gets an edge and never meets") {
  auto made = rtt::sched_rig::make(2);
  REQUIRE(made.has_value());
  rtt::sched_rig &r = **made;
  meet_state m;
  m.patience = std::chrono::milliseconds(20);
  add_meet(r, m, 0, component_writer(r, 0, 6));
  add_meet(r, m, 1, component_writer(r, 0, 7)); // writes position too
  REQUIRE(r.build().has_value());
  CHECK(r.systems->edges(0).empty());
  REQUIRE(r.systems->edges(1).size() == 1);
  CHECK(r.systems->edges(1)[0] == 0);
  for (std::uint32_t f = 0; f < 3; ++f) {
    m.arrived.store(0);
    REQUIRE(r.step().has_value());
  }
  CHECK(m.met.load() == 3); // only the second of each frame saw both arrivals
}
