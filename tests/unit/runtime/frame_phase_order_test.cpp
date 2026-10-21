// RT-INV-FRAME-01 (SL-ARCH-003 ARCH3-R094, R097, R098, R079, R080, `phases`): a frame runs the
// `phases` table in order and simulate repeats pre_tick, tick and post_tick exactly the count a
// reference frame clock and fixed stepper give for the same samples (0 on the first frame, capped
// at max_catchup); each tick sub-phase runs with its own tick and the logical tick advances after
// post_tick; the frame index grows by one per frame; freeze_clocks makes frames run no tick. Also:
// add_task refuses a frozen schedule (schedule_frozen), phases without tasks, empty bodies, more
// than 8, unknown or cross-phase after ids (invalid_argument) and more than max_tasks_per_phase
// (out_of_range); make validates its configuration; the owner-thread calls are wrong_thread on
// another thread.

#include "sl/core/error.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/memory.hpp"
#include "sl/runtime/time.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace jobs = sl::runtime::jobs;
namespace mem = sl::runtime::mem;
namespace rtt = sl::runtime::testing;
namespace time = sl::runtime::time;

using sl::errc;

// One 60 Hz tick: 16'666'667 ns x 60 is 1e9 + 20 units, one tick and a remainder of 20.
constexpr rt::nanos one_tick{16'666'667};

struct rig {
  rtt::scripted_platform plat;
  mem::tracking_resource upstream{"frame.test"};
  std::unique_ptr<jobs::job_system> js;
  std::unique_ptr<mem::frame_arena> arena;
  std::unique_ptr<rt::frame_scheduler> fs;

  explicit rig(std::uint32_t workers, rt::frame_config config = {}) {
    jobs::job_config c;
    c.workers = workers;
    c.capacity = 4096;
    auto started = jobs::job_system::start(plat.get(), std::move(c));
    REQUIRE(started.has_value());
    js = std::move(*started);
    arena = std::make_unique<mem::frame_arena>(upstream, workers, 65536);
    auto made = rt::frame_scheduler::make(*js, plat.get(), *arena, config);
    REQUIRE(made.has_value());
    fs = std::move(*made);
  }
};

struct mark {
  rt::phase ph = rt::phase::begin_frame;
  std::uint64_t tick = 0;

  friend bool operator==(const mark &, const mark &) = default;
};

// The task log: tasks run on workers, so a mutex orders it.
struct task_log {
  std::mutex mutex;
  std::vector<mark> marks;

  void add(rt::phase ph, std::uint64_t tick) {
    const std::scoped_lock lock(mutex);
    marks.push_back(mark{.ph = ph, .tick = tick});
  }
};

std::uint32_t add(rt::frame_scheduler &fs, rt::phase ph, task_log &log) {
  auto id = fs.add_task(
      rt::task_desc{.name = "order",
                    .ph = ph,
                    .fn = [&log, ph](rt::task_context &ctx) noexcept { log.add(ph, ctx.tick); },
                    .after = {}});
  REQUIRE(id.has_value());
  return *id;
}

rt::task_fn noop() {
  return [](rt::task_context &) noexcept {};
}

} // namespace

TEST_CASE("RT-INV-FRAME-01: phases run in table order and simulate repeats the stepper's count") {
  rig r(2);
  task_log tasks;
  for (const rt::phase ph : {rt::phase::apply_inputs, rt::phase::pre_tick, rt::phase::tick,
                             rt::phase::post_tick, rt::phase::observe}) {
    add(*r.fs, ph, tasks);
  }
  std::vector<mark> hooks;
  r.fs->add_phase_end_hook(
      [&hooks](rt::phase ph, time::tick t) { hooks.push_back(mark{.ph = ph, .tick = t}); });
  REQUIRE(r.fs->build());

  time::frame_clock ref_clock;
  auto ref_stepper = time::fixed_stepper::make(time::tick_rate{.num = 60, .den = 1}, 8);
  REQUIRE(ref_stepper.has_value());
  std::uint64_t tick = 0;
  // 0 (first sample), 1, 2, 3 ticks, a zero delta, 1 tick plus remainder, a hitch capped at 8.
  const std::vector<rt::nanos> advances{rt::nanos{0},
                                        one_tick,
                                        2 * one_tick,
                                        3 * one_tick,
                                        rt::nanos{0},
                                        std::chrono::milliseconds(30),
                                        std::chrono::milliseconds(500)};
  for (std::size_t f = 0; f < advances.size(); ++f) {
    r.plat.clock().advance(advances[f]);
    const std::uint32_t expected = ref_stepper->advance(ref_clock.sample(r.plat.clock().now()));
    hooks.clear();
    {
      const std::scoped_lock lock(tasks.mutex);
      tasks.marks.clear();
    }
    auto stats = r.fs->run_frame();
    REQUIRE(stats.has_value());
    INFO("frame " << f + 1);
    CHECK(stats->frame_index == f + 1);
    CHECK(r.fs->current().frame_index == f + 1);
    CHECK(stats->ticks_run == expected);

    std::vector<mark> want{mark{.ph = rt::phase::begin_frame, .tick = tick},
                           mark{.ph = rt::phase::apply_inputs, .tick = tick}};
    std::vector<mark> want_tasks{mark{.ph = rt::phase::apply_inputs, .tick = tick}};
    for (std::uint32_t k = 0; k < expected; ++k) {
      for (const rt::phase ph : {rt::phase::pre_tick, rt::phase::tick, rt::phase::post_tick}) {
        want.push_back(mark{.ph = ph, .tick = tick + k});
        want_tasks.push_back(mark{.ph = ph, .tick = tick + k});
      }
    }
    tick += expected;
    want.push_back(mark{.ph = rt::phase::observe, .tick = tick});
    want.push_back(mark{.ph = rt::phase::present, .tick = tick});
    want_tasks.push_back(mark{.ph = rt::phase::observe, .tick = tick});
    CHECK(hooks == want);
    {
      // one task per phase: the task log is in phase order too
      const std::scoped_lock lock(tasks.mutex);
      CHECK(tasks.marks == want_tasks);
    }
    CHECK(r.fs->current().now == tick);
  }
  CHECK(ref_stepper->dropped_ticks() == 7); // 250 ms at 60 Hz is 15 ticks
  auto last = r.fs->run_frame();
  REQUIRE(last.has_value());
  CHECK(last->dropped_ticks == 7);
  CHECK(last->dropped_ns == std::chrono::milliseconds(250));
}

TEST_CASE("RT-INV-FRAME-01: frozen clocks run no tick and thaw rebases") {
  rig r(1);
  std::uint32_t ticks = 0;
  task_log tasks;
  add(*r.fs, rt::phase::tick, tasks);
  r.fs->add_phase_end_hook(
      [&ticks](rt::phase ph, time::tick) { ticks += ph == rt::phase::tick ? 1U : 0U; });
  REQUIRE(r.fs->build());
  REQUIRE(r.fs->run_frame());
  r.plat.clock().advance(one_tick);
  REQUIRE(r.fs->run_frame());
  CHECK(ticks == 1);
  r.fs->freeze_clocks();
  for (int i = 0; i < 3; ++i) {
    r.plat.clock().advance(10 * one_tick);
    auto s = r.fs->run_frame();
    REQUIRE(s.has_value());
    CHECK(s->ticks_run == 0);
  }
  r.fs->thaw_clocks();
  r.plat.clock().advance(10 * one_tick);
  auto rebased = r.fs->run_frame();
  REQUIRE(rebased.has_value());
  CHECK(rebased->ticks_run == 0);
  r.plat.clock().advance(one_tick);
  auto resumed = r.fs->run_frame();
  REQUIRE(resumed.has_value());
  CHECK(resumed->ticks_run == 1);
  CHECK(ticks == 2);
  CHECK(resumed->frame_index == 7);
}

TEST_CASE("RT-INV-FRAME-01: add_task and build refuse what the phase graph cannot hold") {
  rig r(1);
  task_log tasks;
  const std::uint32_t a = add(*r.fs, rt::phase::tick, tasks);
  const std::uint32_t b = add(*r.fs, rt::phase::observe, tasks);
  CHECK(a == 0);
  CHECK(b == 1);
  CHECK_FALSE(r.fs->run_frame().has_value());
  CHECK(r.fs->run_frame().error().code == errc::invalid_state); // before build

  const auto refused = [&r](rt::phase ph, std::vector<std::uint32_t> after, bool body) {
    auto got = r.fs->add_task(rt::task_desc{
        .name = "bad", .ph = ph, .fn = body ? noop() : rt::task_fn{}, .after = std::move(after)});
    REQUIRE_FALSE(got.has_value());
    return got.error().code;
  };
  CHECK(refused(rt::phase::begin_frame, {}, true) == errc::invalid_argument);
  CHECK(refused(rt::phase::present, {}, true) == errc::invalid_argument);
  CHECK(refused(rt::phase::end_frame, {}, true) == errc::invalid_argument);
  CHECK(refused(rt::phase::tick, {}, false) == errc::invalid_argument);
  CHECK(refused(rt::phase::tick, {7}, true) == errc::invalid_argument);     // unknown id
  CHECK(refused(rt::phase::tick, {b}, true) == errc::invalid_argument);     // cross-phase
  CHECK(refused(rt::phase::tick, std::vector<std::uint32_t>(9, a), true) == // 9 edges
        errc::invalid_argument);

  for (std::uint32_t i = 1; i < rt::max_tasks_per_phase; ++i) {
    REQUIRE(r.fs->add_task(
        rt::task_desc{.name = "fill", .ph = rt::phase::tick, .fn = noop(), .after = {}}));
  }
  CHECK(refused(rt::phase::tick, {}, true) == errc::out_of_range);

  REQUIRE(r.fs->build());
  CHECK(r.fs->build().has_value()); // once built, a second build is a no-op
  CHECK(refused(rt::phase::observe, {}, true) == errc::schedule_frozen);
  CHECK(r.fs->run_frame().has_value());
}

TEST_CASE("RT-INV-FRAME-01: the owner-thread calls are wrong_thread elsewhere") {
  rig r(1);
  errc add_code = errc::ok;
  errc build_code = errc::ok;
  errc run_code = errc::ok;
  std::thread other([&] {
    auto added = r.fs->add_task(
        rt::task_desc{.name = "x", .ph = rt::phase::tick, .fn = noop(), .after = {}});
    add_code = added ? errc::ok : added.error().code;
    auto built = r.fs->build();
    build_code = built ? errc::ok : built.error().code;
    auto ran = r.fs->run_frame();
    run_code = ran ? errc::ok : ran.error().code;
  });
  other.join();
  CHECK(add_code == errc::wrong_thread);
  CHECK(build_code == errc::wrong_thread);
  CHECK(run_code == errc::wrong_thread);
  REQUIRE(r.fs->build());
  CHECK(r.fs->run_frame().has_value());
}

TEST_CASE("RT-INV-FRAME-01: make validates the rate, the frame delta cap and the arena") {
  rtt::scripted_platform plat;
  jobs::job_config c;
  c.workers = 1;
  c.capacity = 256;
  auto js = jobs::job_system::start(plat.get(), std::move(c));
  REQUIRE(js.has_value());
  mem::tracking_resource upstream("frame.test");
  mem::frame_arena arena(upstream, 1, 4096);
  mem::frame_arena wrong(upstream, 3, 4096);

  auto bad_rate = rt::frame_scheduler::make(**js, plat.get(), arena,
                                            rt::frame_config{.rate = {.num = 0, .den = 1}});
  REQUIRE_FALSE(bad_rate.has_value());
  CHECK(bad_rate.error().code == errc::invalid_tick_rate);
  auto bad_delta = rt::frame_scheduler::make(**js, plat.get(), arena,
                                             rt::frame_config{.max_frame_delta = rt::nanos{0}});
  REQUIRE_FALSE(bad_delta.has_value());
  CHECK(bad_delta.error().code == errc::invalid_argument);
  auto bad_arena = rt::frame_scheduler::make(**js, plat.get(), wrong);
  REQUIRE_FALSE(bad_arena.has_value());
  CHECK(bad_arena.error().code == errc::invalid_argument);
  auto ok = rt::frame_scheduler::make(**js, plat.get(), arena);
  REQUIRE(ok.has_value());
  CHECK((*ok)->current().frame_index == 0);
  CHECK((*ok)->frame_inputs().empty());
  CHECK((*ok)->upstream_allocs_last_frame() == 0);
}
