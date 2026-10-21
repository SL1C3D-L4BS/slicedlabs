// RT-INV-FRAME-02 (SL-ARCH-003 ARCH3-R095, R096, R094): the end of every phase is a sync
// boundary: on 4 workers, with 4 tasks in every task phase over 2 ticks per frame, no job of a
// phase run starts before every job of the previous phase run has finished (a global sequence
// number stamps each start and end), and end_frame starts with zero frame jobs in flight: a child
// job a task submitted outside the phase counter has completed (the job system is quiesced) and
// no job is pending when the first boundary slot runs. A failed task cancels its dependents and
// the frame still closes.

#include "sl/core/error.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/memory.hpp"
#include "sl/runtime/time.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <algorithm>
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
namespace time = sl::runtime::time;

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

// One task run: the phase run it belongs to and its start and end stamps.
struct stamp {
  std::uint32_t run = 0;
  std::uint64_t start = 0;
  std::uint64_t end = 0;
};

constexpr std::size_t max_stamps = 4096;

struct recorder {
  std::atomic<std::uint64_t> seq{0};
  std::atomic<std::uint32_t> run{0}; // advanced by the phase end hook (owner thread)
  std::atomic<std::size_t> used{0};
  std::vector<stamp> stamps = std::vector<stamp>(max_stamps);

  void body() noexcept {
    const std::uint32_t r = run.load();
    const std::uint64_t start = seq.fetch_add(1);
    for (int i = 0; i < 20; ++i) {
      std::this_thread::yield();
    }
    const std::uint64_t end = seq.fetch_add(1);
    const std::size_t slot = used.fetch_add(1);
    if (slot < stamps.size()) {
      stamps[slot] = stamp{.run = r, .start = start, .end = end};
    }
  }
};

} // namespace

TEST_CASE("RT-INV-FRAME-02: no job of a phase starts before the previous phase completed") {
  rig r(4);
  recorder rec;
  for (const rt::phase ph : {rt::phase::apply_inputs, rt::phase::pre_tick, rt::phase::tick,
                             rt::phase::post_tick, rt::phase::observe}) {
    for (int k = 0; k < 4; ++k) {
      REQUIRE(
          r.fs->add_task(rt::task_desc{.name = "sync",
                                       .ph = ph,
                                       .fn = [&rec](rt::task_context &) noexcept { rec.body(); },
                                       .after = {}}));
    }
  }
  r.fs->add_phase_end_hook([&rec](rt::phase, time::tick) { rec.run.fetch_add(1); });

  // A tick task submits a child job outside the phase counter; end_frame must find it done.
  std::atomic<std::uint32_t> children_done{0};
  std::atomic<std::uint32_t> children{0};
  REQUIRE(r.fs->add_task(
      rt::task_desc{.name = "parent",
                    .ph = rt::phase::tick,
                    .fn =
                        [&r, &children, &children_done](rt::task_context &) noexcept {
                          children.fetch_add(1);
                          static_cast<void>(r.js->submit(
                              {.fn =
                                   [&children_done](jobs::job_context &) noexcept {
                                     std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                     children_done.fetch_add(1);
                                   },
                               .name = "child"}));
                        },
                    .after = {}}));
  std::uint32_t boundary_checks = 0;
  std::uint32_t boundary_violations = 0;
  r.fs->add_boundary_hook(rt::boundary_slot::fault_check, [&](const rt::boundary_context &) {
    ++boundary_checks;
    if (r.js->stats().pending != 0 || children_done.load() != children.load()) {
      ++boundary_violations;
    }
  });
  REQUIRE(r.fs->build());

  constexpr std::uint32_t frames = 12;
  for (std::uint32_t f = 0; f < frames; ++f) {
    r.plat.clock().advance(2 * one_tick);
    auto s = r.fs->run_frame();
    REQUIRE(s.has_value());
  }
  CHECK(boundary_checks == frames);
  CHECK(boundary_violations == 0);
  CHECK(children.load() == 2 * (frames - 1)); // the first frame runs no tick
  CHECK(children_done.load() == children.load());

  const std::size_t n = std::min(rec.used.load(), rec.stamps.size());
  // the first frame: apply_inputs and observe; every later frame also 2 ticks of 3 sub-phases
  constexpr std::size_t first_frame_stamps = 8;
  constexpr std::size_t ticked_frame_stamps = 32;
  REQUIRE(n == first_frame_stamps + (ticked_frame_stamps * (frames - 1)));
  // Per phase run: the latest end and the earliest start.
  const std::uint32_t runs = rec.run.load();
  std::vector<std::uint64_t> last_end(runs, 0);
  std::vector<std::uint64_t> first_start(runs, UINT64_MAX);
  for (std::size_t i = 0; i < n; ++i) {
    const stamp &s = rec.stamps[i];
    REQUIRE(s.run < runs);
    last_end[s.run] = std::max(last_end[s.run], s.end);
    first_start[s.run] = std::min(first_start[s.run], s.start);
  }
  std::uint32_t overlaps = 0;
  std::uint32_t task_runs = 0;
  std::uint64_t previous_end = 0;
  bool have_previous = false;
  for (std::uint32_t run = 0; run < runs; ++run) {
    if (first_start[run] == UINT64_MAX) {
      continue; // begin_frame and present run no task
    }
    ++task_runs;
    if (have_previous && first_start[run] < previous_end) {
      ++overlaps;
    }
    previous_end = last_end[run];
    have_previous = true;
  }
  CHECK(task_runs == (2 * frames) + (6 * (frames - 1)));
  CHECK(overlaps == 0);
}

TEST_CASE("RT-INV-FRAME-02: a failed task cancels its dependents and the frame still closes") {
  rig r(2);
  std::atomic<std::uint32_t> dependent_runs{0};
  std::atomic<std::uint32_t> observe_runs{0};
  auto failing = r.fs->add_task(rt::task_desc{
      .name = "fails",
      .ph = rt::phase::apply_inputs,
      .fn = [](rt::task_context &ctx) noexcept { ctx.job.fail(sl::errc::job_failed); },
      .after = {}});
  REQUIRE(failing.has_value());
  REQUIRE(r.fs->add_task(rt::task_desc{
      .name = "dependent",
      .ph = rt::phase::apply_inputs,
      .fn = [&dependent_runs](rt::task_context &) noexcept { dependent_runs.fetch_add(1); },
      .after = {*failing}}));
  REQUIRE(r.fs->add_task(rt::task_desc{
      .name = "observer",
      .ph = rt::phase::observe,
      .fn = [&observe_runs](rt::task_context &) noexcept { observe_runs.fetch_add(1); },
      .after = {}}));
  std::uint32_t closed = 0;
  r.fs->add_boundary_hook(rt::boundary_slot::diagnostics,
                          [&closed](const rt::boundary_context &) { ++closed; });
  REQUIRE(r.fs->build());
  for (int f = 0; f < 3; ++f) {
    auto s = r.fs->run_frame();
    REQUIRE(s.has_value());
    CHECK(s->phases[static_cast<std::size_t>(rt::phase::apply_inputs)].jobs == 2);
  }
  CHECK(dependent_runs.load() == 0);
  CHECK(observe_runs.load() == 3);
  CHECK(closed == 3);
  CHECK(r.js->stats().pending == 0);
  CHECK(r.js->stats().cancelled >= 3);
}
