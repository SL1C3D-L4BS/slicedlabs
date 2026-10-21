// RT-INV-FRAME-04 (SL-ARCH-003 ARCH3-R051, R097, `limits` warmup_frames 10 and
// frame_arena_block_bytes, `thread_safety` frame_arena::flip): tasks allocate frame memory from
// their worker's frame arena every phase run; after the 10 warm-up frames every frame reports
// upstream_allocs 0 (frame_stats and upstream_allocs_last_frame) and the tracking upstream of the
// frame arena sees no new block, and the arena flips exactly once per end_frame. On 0 workers the
// warm-up takes the blocks; on 2 workers the arenas of both sets are warmed first, so which worker
// runs which task cannot move a first allocation past the warm-up. A drained background result is
// an allocation the scheduler sees: it reaches frame_inputs and the record_sink and is counted.

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
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace jobs = sl::runtime::jobs;
namespace mem = sl::runtime::mem;
namespace rtt = sl::runtime::testing;

constexpr rt::nanos one_tick{16'666'667};
constexpr std::size_t block_bytes = 65536;
constexpr std::size_t task_bytes = 1024;

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
    arena = std::make_unique<mem::frame_arena>(upstream, workers, block_bytes);
    auto made = rt::frame_scheduler::make(*js, plat.get(), *arena);
    REQUIRE(made.has_value());
    fs = std::move(*made);
  }
};

struct alloc_task {
  mem::frame_arena *arena = nullptr;
  std::atomic<std::uint32_t> *failures = nullptr;

  void operator()(const rt::task_context &ctx) const noexcept {
    mem::resource &mine = arena->for_worker(ctx.job.worker());
    auto got = mine.try_allocate(task_bytes, 16, mine.default_tag());
    if (got.has_value()) {
      std::memset(*got, 0x5A, task_bytes);
    } else {
      failures->fetch_add(1);
    }
  }
};

void add_alloc_tasks(rig &r, std::atomic<std::uint32_t> &failures) {
  for (const rt::phase ph : {rt::phase::apply_inputs, rt::phase::pre_tick, rt::phase::tick,
                             rt::phase::post_tick, rt::phase::observe}) {
    for (int k = 0; k < 2; ++k) {
      REQUIRE(r.fs->add_task(
          rt::task_desc{.name = "alloc",
                        .ph = ph,
                        .fn = alloc_task{.arena = r.arena.get(), .failures = &failures},
                        .after = {}}));
    }
  }
  REQUIRE(r.fs->build());
}

// Runs `frames` one-tick frames and checks the zero-upstream rule after warm-up.
void run_steady(rig &r, std::uint32_t frames) {
  std::uint64_t blocks_after_warmup = 0;
  for (std::uint32_t f = 1; f <= frames; ++f) {
    r.plat.clock().advance(one_tick);
    auto s = r.fs->run_frame();
    REQUIRE(s.has_value());
    INFO("frame " << f);
    CHECK(r.arena->flips() == f);
    CHECK(r.fs->upstream_allocs_last_frame() == s->upstream_allocs);
    if (f == rt::warmup_frames_default) {
      blocks_after_warmup = r.upstream.totals().allocs;
    }
    if (f > rt::warmup_frames_default) {
      CHECK(s->upstream_allocs == 0);
      CHECK(r.upstream.totals().allocs == blocks_after_warmup);
    }
  }
}

} // namespace

TEST_CASE("RT-INV-FRAME-04: after warm-up a steady frame takes no upstream block (0 workers)") {
  rig r(0);
  std::atomic<std::uint32_t> failures{0};
  add_alloc_tasks(r, failures);
  std::uint64_t warmup_allocs = 0;
  r.fs->add_boundary_hook(rt::boundary_slot::diagnostics,
                          [&](const rt::boundary_context &) { ++warmup_allocs; });
  run_steady(r, 30);
  CHECK(failures.load() == 0);
  CHECK(warmup_allocs == 30);
  CHECK(r.upstream.totals().allocs >= 2); // one block per set during warm-up
}

TEST_CASE("RT-INV-FRAME-04: after warm-up a steady frame takes no upstream block (2 workers)") {
  rig r(2);
  // Warm every arena of both sets from this thread; a flip unbinds the set it makes current.
  for (int set = 0; set < 2; ++set) {
    for (std::uint32_t k = 0; k <= r.arena->workers(); ++k) {
      mem::resource &a = r.arena->for_worker(k);
      auto got = a.try_allocate(task_bytes, 16, a.default_tag());
      REQUIRE(got.has_value());
    }
    r.arena->flip();
  }
  const std::uint64_t warm_flips = r.arena->flips();
  REQUIRE(warm_flips == 2);
  std::atomic<std::uint32_t> failures{0};
  add_alloc_tasks(r, failures);
  std::uint64_t blocks = r.upstream.totals().allocs;
  for (std::uint32_t f = 1; f <= 30; ++f) {
    r.plat.clock().advance(one_tick);
    auto s = r.fs->run_frame();
    REQUIRE(s.has_value());
    INFO("frame " << f);
    CHECK(r.arena->flips() == warm_flips + f);
    if (f > rt::warmup_frames_default) {
      CHECK(s->upstream_allocs == 0);
      CHECK(r.upstream.totals().allocs == blocks);
    }
    blocks = r.upstream.totals().allocs;
  }
  CHECK(failures.load() == 0);
}

TEST_CASE("RT-INV-FRAME-04: a drained background result is an external input and is counted") {
  rig r(0);
  REQUIRE(r.fs->build());
  for (int f = 0; f < 3; ++f) {
    REQUIRE(r.fs->run_frame());
  }
  const std::array<std::byte, 3> bytes{std::byte{1}, std::byte{2}, std::byte{3}};
  const jobs::job_handle h = r.js->submit({.fn =
                                               [&bytes](jobs::job_context &ctx) noexcept {
                                                 if (!ctx.post_result(7, bytes)) {
                                                   ctx.fail(sl::errc::invalid_state);
                                                 }
                                               },
                                           .prio = jobs::priority::background,
                                           .name = "background"});
  REQUIRE(r.js->wait(h));
  r.plat.sink().clear();
  auto s = r.fs->run_frame();
  REQUIRE(s.has_value());
  CHECK(s->upstream_allocs >= 1);
  REQUIRE(r.fs->frame_inputs().size() == 1);
  const auto &in = r.fs->frame_inputs()[0];
  CHECK(in.frame == 4);
  CHECK(in.kind == 7);
  CHECK(in.bytes == std::vector<std::byte>(bytes.begin(), bytes.end()));
  CHECK(in.key.phase == static_cast<std::uint8_t>(rt::phase::begin_frame));
  const auto &records = r.plat.sink().records();
  REQUIRE(records.size() == 2);
  CHECK(records[0].kind == rtt::record_kind::wall_sample);
  CHECK(records[0].frame == 4);
  CHECK(records[1].kind == rtt::record_kind::external);
  CHECK(records[1].frame == 4);
  CHECK(records[1].code == 7);
  auto next = r.fs->run_frame();
  REQUIRE(next.has_value());
  CHECK(r.fs->frame_inputs().empty());
}
