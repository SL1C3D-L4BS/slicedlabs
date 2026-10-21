// RT-INV-CMD-02 (SL-ARCH-003 ARCH3-R119, R118, ADR-0025): pending entities resolve at flush in
// key order, the handles they become are identical across worker counts, and commands on a
// pending entity apply after its spawn. A pending entity of an earlier key resolves in a later
// buffer; one of a later key, or one no buffer spawned, is stale.

#include "sl/core/error.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/ecs/command_buffer.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/memory.hpp"
#include "sl/runtime/replay.hpp"
#include "sl/runtime/sync.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <array>
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

using op_kind = ecs::flush_report::op_kind;

struct pos {
  std::int32_t x = 0;
  std::int32_t y = 0;
};
struct vel {
  std::int32_t v = 0;
};

det::ordered_key key(std::uint16_t system, std::uint32_t chunk) {
  return det::ordered_key{
      .tick = 3, .phase = 2, .system_index = system, .chunk_index = chunk, .local_seq = 0};
}

struct rig {
  ecs::component_registry registry;
  mem::tracking_resource world_memory{"cmd.pending.world"};
  mem::tracking_resource arena_memory{"cmd.pending.arena"};
  std::unique_ptr<ecs::world> w;
  ecs::component_id pos_id;
  ecs::component_id vel_id;

  rig() {
    pos_id = registry.add<pos>("pos").value_or(ecs::component_id{});
    vel_id = registry.add<vel>("vel").value_or(ecs::component_id{});
    REQUIRE(registry.freeze().has_value());
    auto made = ecs::world::make(registry, world_memory);
    REQUIRE(made.has_value());
    w = std::move(*made);
  }
};

constexpr std::uint32_t job_count = 32;
constexpr std::uint32_t spawns_per_job = 3;

struct spawn_job {
  ecs::command_queue *queue = nullptr;
  std::uint32_t j = 0;

  void operator()(const jobs::job_context &ctx) const noexcept {
    ecs::command_buffer &b =
        queue->open(key(static_cast<std::uint16_t>(j % 5), j / 5), ctx.worker());
    for (std::uint32_t s = 0; s < spawns_per_job; ++s) {
      const auto i = static_cast<std::int32_t>((j * spawns_per_job) + s);
      const ecs::pending_entity p = b.spawn();
      b.set(p, pos{.x = i, .y = 0});
      if (s == 1) {
        b.add(p, vel{.v = i});
      }
    }
  }
};

struct resolved_run {
  std::vector<std::uint32_t> seqs;
  std::vector<ecs::entity> entities;
  std::vector<std::int32_t> xs;
};

jobs::job_config config(std::uint32_t workers) {
  jobs::job_config c;
  c.workers = workers;
  c.capacity = 4096;
  return c;
}

resolved_run run_on(std::uint32_t workers) {
  rtt::scripted_platform plat;
  auto js = jobs::job_system::start(plat.get(), config(workers));
  REQUIRE(js.has_value());
  rig r;
  mem::frame_arena arena(r.arena_memory, workers);
  ecs::command_queue q(r.registry, arena);
  rt::job_counter done;
  // Submitted in reverse key order, so open order and completion order differ from key order.
  for (std::uint32_t n = job_count; n > 0; --n) {
    (*js)->submit(jobs::job_desc{.fn = spawn_job{.queue = &q, .j = n - 1}, .name = "cmd.spawn"}, {},
                  &done);
  }
  REQUIRE((*js)->wait(done));
  auto report = q.flush(*r.w);
  REQUIRE(report.has_value());
  resolved_run out;
  for (const auto &[p, e] : report->resolved) {
    out.seqs.push_back(p.seq);
    out.entities.push_back(e);
    auto got = r.w->get<pos>(e);
    out.xs.push_back(got ? (*got)->x : -1);
  }
  CHECK(report->skipped_stale == 0);
  REQUIRE((*js)->stop());
  return out;
}

} // namespace

TEST_CASE("RT-INV-CMD-02: commands on a pending entity apply after its spawn") {
  rig r;
  mem::frame_arena arena(r.arena_memory, 0);
  ecs::command_queue q(r.registry, arena);
  ecs::command_buffer &b = q.open(key(1, 0));
  const ecs::pending_entity p = b.spawn();
  CHECK(p.buffer == b.id());
  CHECK(p.seq == 0);
  b.set(p, pos{.x = 3, .y = 4});
  b.add(p, vel{.v = 5});
  b.remove<vel>(p);
  b.add(p, vel{.v = 6});
  CHECK(b.size() == 5);
  auto report = q.flush(*r.w);
  REQUIRE(report.has_value());
  REQUIRE(report->resolved.size() == 1);
  CHECK(report->resolved[0].first == p);
  const ecs::entity e = report->resolved[0].second;
  CHECK(r.w->is_alive(e));
  auto got_pos = r.w->get<pos>(e);
  REQUIRE(got_pos.has_value());
  CHECK((*got_pos)->x == 3);
  CHECK((*got_pos)->y == 4);
  auto got_vel = r.w->get<vel>(e);
  REQUIRE(got_vel.has_value());
  CHECK((*got_vel)->v == 6);
  const std::vector<ecs::flush_report::op> expected{
      {.kind = op_kind::spawn, .target = e, .component = ecs::component_id{}},
      {.kind = op_kind::set, .target = e, .component = r.pos_id},
      {.kind = op_kind::add, .target = e, .component = r.vel_id},
      {.kind = op_kind::remove, .target = e, .component = r.vel_id},
      {.kind = op_kind::add, .target = e, .component = r.vel_id}};
  CHECK(report->applied == expected);
  CHECK(q.open_count() == 0);
}

TEST_CASE("RT-INV-CMD-02: pending entities resolve in key order whatever the open order") {
  rig r;
  mem::frame_arena arena(r.arena_memory, 0);
  ecs::command_queue q(r.registry, arena);
  ecs::command_buffer &late = q.open(key(2, 0));
  ecs::command_buffer &early = q.open(key(1, 7));
  std::vector<ecs::pending_entity> late_p;
  std::vector<ecs::pending_entity> early_p;
  for (int i = 0; i < 3; ++i) {
    late_p.push_back(late.spawn_with(pos{.x = 20 + i, .y = 0}));
    early_p.push_back(early.spawn_with(pos{.x = 10 + i, .y = 0}, vel{.v = i}));
  }
  auto report = q.flush(*r.w);
  REQUIRE(report.has_value());
  REQUIRE(report->resolved.size() == 6);
  for (std::size_t i = 0; i < 3; ++i) {
    CHECK(report->resolved[i].first == early_p[i]);
    CHECK(report->resolved[i + 3].first == late_p[i]);
  }
  for (std::size_t i = 1; i < 6; ++i) {
    CHECK(report->resolved[i - 1].second.index < report->resolved[i].second.index);
  }
  for (std::size_t i = 0; i < 6; ++i) {
    auto got = r.w->get<pos>(report->resolved[i].second);
    REQUIRE(got.has_value());
    CHECK((*got)->x == (i < 3 ? 10 : 17) + static_cast<std::int32_t>(i));
  }
}

TEST_CASE("RT-INV-CMD-02: a pending entity of a later key or of no spawn is stale") {
  rig r;
  mem::frame_arena arena(r.arena_memory, 0);
  ecs::command_queue q(r.registry, arena);
  ecs::command_buffer &a = q.open(key(1, 0));
  ecs::command_buffer &b = q.open(key(1, 1));
  const ecs::pending_entity pa = a.spawn();
  const ecs::pending_entity pb = b.spawn();
  b.set(pa, pos{.x = 1, .y = 1}); // key (1, 1) after pa's spawn at (1, 0): applies
  a.set(pb, pos{.x = 2, .y = 2}); // key (1, 0) before pb's spawn: stale
  a.destroy(ecs::pending_entity{.buffer = 99, .seq = 0});
  a.add(ecs::pending_entity{.buffer = a.id(), .seq = 50}, vel{.v = 1});
  b.remove<pos>(ecs::pending_entity{.buffer = b.id(), .seq = 1}); // seq 1 is not a spawn
  auto report = q.flush(*r.w);
  REQUIRE(report.has_value());
  CHECK(report->skipped_stale == 4);
  REQUIRE(report->resolved.size() == 2);
  const ecs::entity ea = report->resolved[0].second;
  const ecs::entity eb = report->resolved[1].second;
  CHECK(report->resolved[0].first == pa);
  CHECK(report->resolved[1].first == pb);
  auto got_a = r.w->get<pos>(ea);
  REQUIRE(got_a.has_value());
  CHECK((*got_a)->x == 1);
  CHECK_FALSE(r.w->get<pos>(eb).has_value());
}

TEST_CASE("RT-INV-CMD-02: resolved handles are identical on 1 to 16 workers") {
  const resolved_run ref = run_on(1);
  REQUIRE(ref.entities.size() == std::size_t{job_count} * spawns_per_job);
  // Key order: system j % 5 then chunk j / 5; within a job the spawns in seq order.
  std::vector<std::int32_t> expected;
  for (std::uint32_t system = 0; system < 5; ++system) {
    for (std::uint32_t j = system; j < job_count; j += 5) {
      for (std::uint32_t s = 0; s < spawns_per_job; ++s) {
        expected.push_back(static_cast<std::int32_t>((j * spawns_per_job) + s));
      }
    }
  }
  CHECK(ref.xs == expected);
  for (const std::uint32_t workers : {2U, 4U, 8U, 16U}) {
    INFO("workers " << workers);
    const resolved_run got = run_on(workers);
    CHECK(got.entities == ref.entities);
    CHECK(got.seqs == ref.seqs);
    CHECK(got.xs == ref.xs);
  }
}
