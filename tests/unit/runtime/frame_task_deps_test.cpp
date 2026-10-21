// RT-INV-FRAME-05 (SL-ARCH-003 ARCH3-R100, R094, R063): intra-phase after-edges are honoured (a
// chain a -> b -> c and a diamond d -> {e, f} -> g in the tick phase, on 4 workers, over 40
// frames: no task starts before each of its after tasks has finished); two independent observe
// tasks overlap on 2 workers (each waits for the other to arrive, on different threads); and
// frame_stats reports per-phase job counts (summed over the ticks) and the critical path in jobs
// (3 for the tick graph, 1 for two independent tasks, 0 for a phase that did not run).

#include "sl/core/error.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/memory.hpp"
#include "sl/runtime/time.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace jobs = sl::runtime::jobs;
namespace mem = sl::runtime::mem;
namespace rtt = sl::runtime::testing;

constexpr rt::nanos one_tick{16'666'667};

struct rig {
  rtt::scripted_platform plat;
  mem::tracking_resource upstream{"frame.test"};
  std::unique_ptr<jobs::job_system> js;
  std::unique_ptr<mem::frame_arena> arena;
  std::unique_ptr<rt::frame_scheduler> fs;

  explicit rig(std::uint32_t workers) {
    jobs::job_config c;
    c.workers = workers;
    c.capacity = 4096;
    auto started = jobs::job_system::start(plat.get(), std::move(c));
    REQUIRE(started.has_value());
    js = std::move(*started);
    arena = std::make_unique<mem::frame_arena>(upstream, workers, 65536);
    auto made = rt::frame_scheduler::make(*js, plat.get(), *arena);
    REQUIRE(made.has_value());
    fs = std::move(*made);
  }
};

constexpr std::size_t task_count = 7;

// finished[i] counts the runs of task i that completed; a task checks that each of its after
// tasks has completed as many runs as it is starting now.
struct graph_state {
  std::vector<std::atomic<std::uint32_t>> finished =
      std::vector<std::atomic<std::uint32_t>>(task_count);
  std::vector<std::atomic<std::uint32_t>> started =
      std::vector<std::atomic<std::uint32_t>>(task_count);
  std::atomic<std::uint32_t> early{0};
  std::vector<std::vector<std::uint32_t>> after;
};

struct graph_task {
  graph_state *g = nullptr;
  std::uint32_t self = 0;

  void operator()(rt::task_context & /*ctx*/) const noexcept {
    const std::uint32_t run = g->started[self].fetch_add(1) + 1;
    for (const std::uint32_t a : g->after[self]) {
      if (g->finished[a].load() < run) {
        g->early.fetch_add(1);
      }
    }
    for (int i = 0; i < 10; ++i) {
      std::this_thread::yield();
    }
    g->finished[self].fetch_add(1);
  }
};

// Two tasks that each wait (up to a deadline) for the other to arrive.
struct meet_state {
  std::atomic<std::uint32_t> arrived{0};
  std::atomic<std::uint32_t> met{0};
  std::vector<std::atomic<std::thread::id>> threads = std::vector<std::atomic<std::thread::id>>(2);
};

struct meet_task {
  meet_state *m = nullptr;
  std::uint32_t self = 0;

  void operator()(rt::task_context & /*ctx*/) const noexcept {
    m->threads[self].store(std::this_thread::get_id());
    m->arrived.fetch_add(1);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (m->arrived.load() < 2 && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    if (m->arrived.load() >= 2) {
      m->met.fetch_add(1);
    }
  }
};

} // namespace

TEST_CASE("RT-INV-FRAME-05: after-edges are honoured and frame_stats reports the critical path") {
  rig r(4);
  graph_state g;
  // a=0 -> b=1 -> c=2; d=3 -> {e=4, f=5} -> g=6
  g.after = {{}, {0}, {1}, {}, {3}, {3}, {4, 5}};
  for (std::uint32_t i = 0; i < task_count; ++i) {
    auto id = r.fs->add_task(rt::task_desc{.name = "graph",
                                           .ph = rt::phase::tick,
                                           .fn = graph_task{.g = &g, .self = i},
                                           .after = g.after[i]});
    REQUIRE(id.has_value());
    CHECK(*id == i);
  }
  for (int k = 0; k < 2; ++k) {
    REQUIRE(r.fs->add_task(rt::task_desc{.name = "free",
                                         .ph = rt::phase::observe,
                                         .fn = [](rt::task_context &) noexcept {},
                                         .after = {}}));
  }
  REQUIRE(r.fs->build());

  constexpr std::uint32_t frames = 40;
  std::uint32_t ticks = 0;
  for (std::uint32_t f = 1; f <= frames; ++f) {
    r.plat.clock().advance(f % 3 == 0 ? 2 * one_tick : one_tick);
    auto s = r.fs->run_frame();
    REQUIRE(s.has_value());
    ticks += s->ticks_run;
    const auto &tick = s->phases[static_cast<std::size_t>(rt::phase::tick)];
    const auto &observe = s->phases[static_cast<std::size_t>(rt::phase::observe)];
    const auto &apply = s->phases[static_cast<std::size_t>(rt::phase::apply_inputs)];
    const auto &pre = s->phases[static_cast<std::size_t>(rt::phase::pre_tick)];
    INFO("frame " << f);
    CHECK(tick.jobs == task_count * s->ticks_run);
    CHECK(tick.critical_path == (s->ticks_run == 0 ? 0U : 3U));
    CHECK(observe.jobs == 2);
    CHECK(observe.critical_path == 1);
    CHECK(apply.jobs == 0);
    CHECK(apply.critical_path == 0);
    CHECK(pre.jobs == 0);
    CHECK(s->phases[static_cast<std::size_t>(rt::phase::present)].jobs == 0);
    CHECK(s->phases[static_cast<std::size_t>(rt::phase::end_frame)].jobs == 0);
  }
  CHECK(g.early.load() == 0);
  for (std::size_t i = 0; i < task_count; ++i) {
    CHECK(g.finished[i].load() == ticks);
  }
  CHECK(ticks > frames);
}

TEST_CASE("RT-INV-FRAME-05: independent tasks of one phase overlap on 2 workers") {
  rig r(2);
  meet_state m;
  for (std::uint32_t i = 0; i < 2; ++i) {
    REQUIRE(r.fs->add_task(rt::task_desc{.name = "meet",
                                         .ph = rt::phase::observe,
                                         .fn = meet_task{.m = &m, .self = i},
                                         .after = {}}));
  }
  REQUIRE(r.fs->build());
  constexpr std::uint32_t frames = 5;
  std::uint32_t distinct = 0;
  for (std::uint32_t f = 1; f <= frames; ++f) {
    m.arrived.store(0);
    auto s = r.fs->run_frame();
    REQUIRE(s.has_value());
    CHECK(s->phases[static_cast<std::size_t>(rt::phase::observe)].jobs == 2);
    CHECK(s->phases[static_cast<std::size_t>(rt::phase::observe)].critical_path == 1);
    distinct += m.threads[0].load() != m.threads[1].load() ? 1U : 0U;
  }
  CHECK(m.met.load() == 2 * frames);
  CHECK(distinct == frames);
}
