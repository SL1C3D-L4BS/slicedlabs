// RT-INV-FRAME-03 (SL-ARCH-003 ARCH3-R096, R066, R099, `boundary_slots`, seam V06-2): the
// boundary hooks of every slot run on the owner thread, in `boundary_slots` order (hooks of one
// slot in registration order), after the present phase of a frame and before the begin_frame of
// the next; the slot's own action runs first (arena_flip hooks see the flip, the slot before does
// not). Main-affinity jobs are drained there: one a worker task submits during observe has run on
// the owner thread before the first slot, one a boundary hook submits has run on the owner thread
// before run_frame returns.

#include "sl/core/error.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/memory.hpp"
#include "sl/runtime/time.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <atomic>
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

// One entry of the owner-thread log: a phase end (kind 0) or a boundary slot hook (kind 1, with a
// sub-index for the second hook of a slot).
struct entry {
  int kind = 0;
  int value = 0;
  int sub = 0;
  std::uint64_t frame = 0;

  friend bool operator==(const entry &, const entry &) = default;
};

constexpr int phase_kind = 0;
constexpr int slot_kind = 1;

} // namespace

TEST_CASE("RT-INV-FRAME-03: boundary hooks run on the owner thread in boundary_slots order") {
  rig r(2);
  const std::thread::id owner = std::this_thread::get_id();
  std::vector<entry> log;
  std::uint32_t off_owner = 0;
  std::uint64_t flips_seen_before = 0;
  std::uint64_t flips_seen_at = 0;

  REQUIRE(r.fs->add_task(rt::task_desc{.name = "tick",
                                       .ph = rt::phase::tick,
                                       .fn = [](rt::task_context &) noexcept {},
                                       .after = {}}));
  r.fs->add_phase_end_hook([&](rt::phase ph, time::tick) {
    off_owner += std::this_thread::get_id() == owner ? 0U : 1U;
    log.push_back(entry{.kind = phase_kind,
                        .value = static_cast<int>(ph),
                        .sub = 0,
                        .frame = r.fs->current().frame_index});
  });
  for (std::size_t s = 0; s < rt::boundary_slot_count; ++s) {
    r.fs->add_boundary_hook(
        static_cast<rt::boundary_slot>(s), [&, s](const rt::boundary_context &ctx) {
          off_owner += std::this_thread::get_id() == owner ? 0U : 1U;
          CHECK(ctx.slot == static_cast<rt::boundary_slot>(s));
          log.push_back(entry{
              .kind = slot_kind, .value = static_cast<int>(s), .sub = 0, .frame = ctx.frame_index});
        });
  }
  // A second hook on one slot runs after the first.
  r.fs->add_boundary_hook(rt::boundary_slot::snapshot, [&](const rt::boundary_context &ctx) {
    log.push_back(entry{.kind = slot_kind,
                        .value = static_cast<int>(rt::boundary_slot::snapshot),
                        .sub = 1,
                        .frame = ctx.frame_index});
  });
  r.fs->add_boundary_hook(rt::boundary_slot::lifecycle_requests, [&](const rt::boundary_context &) {
    flips_seen_before = r.arena->flips();
  });
  r.fs->add_boundary_hook(rt::boundary_slot::arena_flip,
                          [&](const rt::boundary_context &) { flips_seen_at = r.arena->flips(); });
  REQUIRE(r.fs->build());

  constexpr std::uint64_t frames = 3;
  for (std::uint64_t f = 1; f <= frames; ++f) {
    r.plat.clock().advance(one_tick);
    REQUIRE(r.fs->run_frame());
    CHECK(flips_seen_before == f - 1);
    CHECK(flips_seen_at == f);
  }
  CHECK(off_owner == 0);

  std::vector<entry> want;
  for (std::uint64_t f = 1; f <= frames; ++f) {
    const auto p = [&](rt::phase ph) {
      want.push_back(
          entry{.kind = phase_kind, .value = static_cast<int>(ph), .sub = 0, .frame = f});
    };
    p(rt::phase::begin_frame);
    p(rt::phase::apply_inputs);
    if (f > 1) { // the first frame runs no tick
      p(rt::phase::pre_tick);
      p(rt::phase::tick);
      p(rt::phase::post_tick);
    }
    p(rt::phase::observe);
    p(rt::phase::present);
    for (std::size_t s = 0; s < rt::boundary_slot_count; ++s) {
      want.push_back(entry{.kind = slot_kind, .value = static_cast<int>(s), .sub = 0, .frame = f});
      if (static_cast<rt::boundary_slot>(s) == rt::boundary_slot::snapshot) {
        want.push_back(
            entry{.kind = slot_kind, .value = static_cast<int>(s), .sub = 1, .frame = f});
      }
    }
  }
  CHECK(log == want);
}

TEST_CASE("RT-INV-FRAME-03: main-affinity jobs are drained by the owner thread at the boundary") {
  rig r(2);
  const std::thread::id owner = std::this_thread::get_id();
  std::atomic<std::uint32_t> task_main_runs{0};
  std::atomic<std::uint32_t> hook_main_runs{0};
  std::atomic<std::uint32_t> main_off_owner{0};
  std::atomic<std::uint32_t> submitted_off_owner{0};

  REQUIRE(r.fs->add_task(rt::task_desc{.name = "observe",
                                       .ph = rt::phase::observe,
                                       .fn =
                                           [&](rt::task_context &) noexcept {
                                             if (std::this_thread::get_id() != owner) {
                                               submitted_off_owner.fetch_add(1);
                                             }
                                             static_cast<void>(r.js->submit(
                                                 {.fn =
                                                      [&](jobs::job_context &) noexcept {
                                                        task_main_runs.fetch_add(1);
                                                        if (std::this_thread::get_id() != owner) {
                                                          main_off_owner.fetch_add(1);
                                                        }
                                                      },
                                                  .aff = jobs::affinity::main,
                                                  .name = "main.task"}));
                                           },
                                       .after = {}}));
  std::uint32_t frame_seen = 0;
  std::uint32_t drained_before_slots = 0;
  r.fs->add_boundary_hook(rt::boundary_slot::fault_check, [&](const rt::boundary_context &) {
    ++frame_seen;
    drained_before_slots += task_main_runs.load() == frame_seen ? 1U : 0U;
  });
  r.fs->add_boundary_hook(rt::boundary_slot::swap, [&](const rt::boundary_context &) {
    static_cast<void>(r.js->submit({.fn =
                                        [&](jobs::job_context &) noexcept {
                                          hook_main_runs.fetch_add(1);
                                          if (std::this_thread::get_id() != owner) {
                                            main_off_owner.fetch_add(1);
                                          }
                                        },
                                    .aff = jobs::affinity::main,
                                    .name = "main.hook"}));
  });
  std::uint32_t hook_jobs_at_begin = 0;
  r.fs->add_phase_end_hook([&](rt::phase ph, time::tick) {
    if (ph == rt::phase::begin_frame && hook_main_runs.load() == frame_seen) {
      ++hook_jobs_at_begin; // every earlier frame's hook job ran before this begin_frame ended
    }
  });
  REQUIRE(r.fs->build());

  constexpr std::uint32_t frames = 20;
  for (std::uint32_t f = 1; f <= frames; ++f) {
    REQUIRE(r.fs->run_frame());
    CHECK(hook_main_runs.load() == f);
  }
  CHECK(task_main_runs.load() == frames);
  CHECK(drained_before_slots == frames);
  CHECK(hook_jobs_at_begin == frames);
  CHECK(main_off_owner.load() == 0);
  INFO("observe tasks run off the owner thread: " << submitted_off_owner.load());
}
