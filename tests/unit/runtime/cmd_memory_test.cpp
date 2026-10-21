// RT-INV-CMD-04 (SL-ARCH-003 ARCH3-R118, `limits` command_buffer_block_bytes and warmup_frames,
// `ownership` command buffers): command buffers draw only frame memory. Recording takes 16 KiB
// blocks from the frame arena and nothing from the world's memory; on 2 workers, after the
// warm-up frames, a steady frame of 8 recording jobs, a flush and an arena flip takes no upstream
// allocation (mem::tracking_resource counters of the arena's upstream). Payloads live in the
// blocks: flush and clear destroy every one (a counted component balances).

#include "sl/core/error.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/ecs/command_buffer.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/memory.hpp"
#include "sl/runtime/replay.hpp"
#include "sl/runtime/sync.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace ecs = sl::runtime::ecs;
namespace jobs = sl::runtime::jobs;
namespace mem = sl::runtime::mem;
namespace det = sl::runtime::det;
namespace rtt = sl::runtime::testing;

struct pos {
  std::int32_t x = 0;
  std::int32_t y = 0;
};
struct vel {
  std::int32_t v = 0;
};

// Counts its live instances (moved-from ones included) through a borrowed counter.
class counted {
public:
  explicit counted(std::atomic<std::int32_t> *live) noexcept : live_(live) { live_->fetch_add(1); }
  counted(counted &&other) noexcept : live_(other.live_) { live_->fetch_add(1); }
  counted(const counted &) = delete;
  counted &operator=(const counted &) = delete;
  counted &operator=(counted &&other) noexcept {
    live_ = other.live_;
    return *this;
  }
  ~counted() { live_->fetch_sub(1); }

private:
  std::atomic<std::int32_t> *live_;
};

det::ordered_key key(std::uint16_t system, std::uint32_t chunk) {
  return det::ordered_key{
      .tick = 1, .phase = 2, .system_index = system, .chunk_index = chunk, .local_seq = 0};
}

struct rig {
  ecs::component_registry registry;
  mem::tracking_resource world_memory{"cmd.memory.world"};
  mem::tracking_resource arena_memory{"cmd.memory.arena"};
  std::unique_ptr<ecs::world> w;

  rig() {
    REQUIRE(registry.add<pos>("pos").has_value());
    REQUIRE(registry.add<vel>("vel").has_value());
    REQUIRE(registry.add<counted>("counted").has_value());
    REQUIRE(registry.freeze().has_value());
    auto made = ecs::world::make(registry, world_memory);
    REQUIRE(made.has_value());
    w = std::move(*made);
  }
};

constexpr std::uint32_t jobs_per_frame = 8;
constexpr std::uint32_t spawns_per_job = 16;

// One frame's job: spawn 16 entities with pos and vel, set them, and destroy the entities this
// job spawned the frame before (handles from the previous flush).
struct frame_job {
  ecs::command_queue *queue = nullptr;
  const std::vector<ecs::entity> *previous = nullptr;
  std::uint32_t j = 0;

  void operator()(const jobs::job_context &ctx) const noexcept {
    ecs::command_buffer &b = queue->open(key(0, j), ctx.worker());
    for (std::uint32_t s = 0; s < spawns_per_job; ++s) {
      const auto i = static_cast<std::int32_t>(s);
      const ecs::pending_entity p = b.spawn_with(pos{.x = i, .y = 0}, vel{.v = i});
      b.set(p, pos{.x = i, .y = 1});
    }
    for (std::size_t n = std::size_t{j} * spawns_per_job;
         n < (j + 1) * std::size_t{spawns_per_job} && n < previous->size(); ++n) {
      b.destroy((*previous)[n]);
    }
  }
};

jobs::job_config config(std::uint32_t workers) {
  jobs::job_config c;
  c.workers = workers;
  c.capacity = 4096;
  return c;
}

} // namespace

TEST_CASE("RT-INV-CMD-04: recording takes 16 KiB blocks from the frame arena only") {
  rig r;
  mem::frame_arena arena(r.arena_memory, 0);
  ecs::command_queue q(r.registry, arena);
  std::vector<ecs::entity> es;
  for (std::int32_t i = 0; i < 64; ++i) {
    auto e = r.w->create_with(pos{.x = i, .y = 0});
    REQUIRE(e.has_value());
    es.push_back(e.value_or(ecs::entity{}));
  }
  const std::uint64_t world_allocs = r.world_memory.totals().allocs;
  CHECK(arena.used() == 0);
  ecs::command_buffer &b = q.open(key(0, 0));
  CHECK(arena.used() == 0); // open takes no memory
  b.set(es[0], pos{.x = 1, .y = 1});
  const std::size_t one_block = arena.used();
  CHECK(one_block >= ecs::command_buffer_block_bytes);
  CHECK(one_block < 2 * ecs::command_buffer_block_bytes);
  // About 1000 commands of under 100 bytes: several blocks, each of command_buffer_block_bytes.
  for (std::uint32_t n = 0; n < 1000; ++n) {
    b.set(es[n % es.size()], pos{.x = static_cast<std::int32_t>(n), .y = 2});
  }
  CHECK(b.status() == sl::errc::ok);
  CHECK(b.size() == 1001);
  const std::size_t used = arena.used();
  CHECK(used > 4 * ecs::command_buffer_block_bytes);
  // cppcheck-suppress knownConditionTrueFalse ; recording must not touch the world's memory
  CHECK(r.world_memory.totals().allocs == world_allocs); // nothing from the world's memory
  CHECK(r.arena_memory.totals().allocs >= 1);            // the arena's upstream block
  auto report = q.flush(*r.w);
  REQUIRE(report.has_value());
  CHECK(report->applied.size() == 1001);
  CHECK(arena.used() == used); // the blocks are frame memory: the arena reclaims them at its flip
}

TEST_CASE("RT-INV-CMD-04: a steady frame takes no upstream allocation after warm-up") {
  constexpr std::uint32_t workers = 2;
  rtt::scripted_platform plat;
  auto js = jobs::job_system::start(plat.get(), config(workers));
  REQUIRE(js.has_value());
  rig r;
  mem::frame_arena arena(r.arena_memory, workers);
  // Warm every arena of both sets from this thread; a flip unbinds the set it makes current.
  for (int set = 0; set < 2; ++set) {
    for (std::uint32_t k = 0; k <= arena.workers(); ++k) {
      mem::resource &a = arena.for_worker(k);
      REQUIRE(a.try_allocate(64, 16, a.default_tag()).has_value());
    }
    arena.flip();
  }
  ecs::command_queue q(r.registry, arena);
  std::vector<ecs::entity> previous;
  std::uint64_t blocks = r.arena_memory.totals().allocs;
  constexpr std::uint32_t frames = 30;
  for (std::uint32_t f = 1; f <= frames; ++f) {
    INFO("frame " << f);
    rt::job_counter done;
    for (std::uint32_t j = 0; j < jobs_per_frame; ++j) {
      (*js)->submit(jobs::job_desc{.fn = frame_job{.queue = &q, .previous = &previous, .j = j},
                                   .name = "cmd.frame"},
                    {}, &done);
    }
    REQUIRE((*js)->wait(done));
    auto report = q.flush(*r.w);
    REQUIRE(report.has_value());
    CHECK(report->skipped_stale == 0);
    previous.clear();
    for (const auto &[p, e] : report->resolved) {
      previous.push_back(e);
    }
    CHECK(previous.size() == std::size_t{jobs_per_frame} * spawns_per_job);
    arena.flip();
    if (f > rt::warmup_frames_default) {
      CHECK(r.arena_memory.totals().allocs == blocks);
    }
    blocks = r.arena_memory.totals().allocs;
  }
  REQUIRE((*js)->stop());
}

TEST_CASE("RT-INV-CMD-04: flush and clear destroy every payload in the blocks") {
  std::atomic<std::int32_t> live{0};
  {
    rig r;
    mem::frame_arena arena(r.arena_memory, 0);
    ecs::command_queue q(r.registry, arena);
    ecs::command_buffer &b = q.open(key(0, 0));
    std::vector<ecs::pending_entity> ps;
    ps.reserve(5);
    for (int i = 0; i < 5; ++i) {
      ps.push_back(b.spawn_with(counted(&live), pos{.x = i, .y = 0}));
    }
    b.set(ps[0], counted(&live)); // replaces the spawned value
    CHECK(live.load() == 6);      // the recorded values, in the blocks
    auto report = q.flush(*r.w);
    REQUIRE(report.has_value());
    CHECK(live.load() == 5); // one per entity; the buffer's moved-from values are gone

    ecs::command_buffer &c = q.open(key(0, 1));
    for (int i = 0; i < 4; ++i) {
      c.spawn_with(counted(&live));
      c.add(report->resolved[0].second, counted(&live));
    }
    CHECK(live.load() == 13);
    q.clear(); // a faulted frame: nothing applied, every value destroyed
    CHECK(live.load() == 5);
    CHECK(q.open_count() == 0);
    CHECK(r.w->stats().rows == 5);

    ecs::command_buffer &d = q.open(key(0, 2));
    d.spawn_with(counted(&live));
    CHECK(live.load() == 6);
  } // the queue's destructor destroys the unplayed value, then the world its five
  CHECK(live.load() == 0);
}
