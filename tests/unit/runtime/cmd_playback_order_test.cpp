// RT-INV-CMD-01 (SL-ARCH-003 ARCH3-R119, R086, ADR-0026): command playback follows the
// ordered_key order whatever the job completion order. 48 jobs, keyed (system j % 4, chunk j / 4),
// each spawn an entity, set and add on a shared target and sometimes destroy it, so the result
// depends on the order. A serial model in key order predicts every target's final state, the stale
// and refused counts and the spawn order; runs on 1 to 16 workers, with the jobs chained in
// permuted orders (the completion order is the permutation) and unchained, equal the serial
// reference: the same flush_report and the same world, row by row.

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

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <numeric>
#include <span>
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

constexpr std::uint32_t job_count = 48;
constexpr std::uint32_t target_count = 8;

det::ordered_key key_of(std::uint32_t j) {
  return det::ordered_key{.tick = 7,
                          .phase = 3,
                          .system_index = static_cast<std::uint16_t>(j % 4),
                          .chunk_index = j / 4,
                          .local_seq = 0};
}

// What job j records.
void record(ecs::command_buffer &b, std::uint32_t j, const ecs::entity *targets) noexcept {
  const auto i = static_cast<std::int32_t>(j);
  const ecs::pending_entity p = b.spawn_with(pos{.x = i, .y = 0});
  b.set(p, vel{.v = i});
  const ecs::entity target = targets[j % target_count];
  b.set(target, pos{.x = i, .y = 1});
  b.add(target, vel{.v = i}); // only the first add in key order applies
  if (j % 12 == 11) {
    b.destroy(target); // later commands on the target are stale
  }
}

struct record_job {
  ecs::command_queue *queue = nullptr;
  const ecs::entity *targets = nullptr;
  std::uint32_t j = 0;

  void operator()(const jobs::job_context &ctx) const noexcept {
    record(queue->open(key_of(j), ctx.worker()), j, targets);
  }
};

using row = std::array<std::int64_t, 6>;

// Every live entity in storage order: handle bits, has pos, pos.x, pos.y, has vel, vel.
std::vector<row> state_of(ecs::world &w) {
  std::vector<row> out;
  for (const ecs::archetype *a : w.archetypes()) {
    for (std::uint32_t c = 0; c < a->chunk_count(); ++c) {
      for (const ecs::entity e : a->entities(c)) {
        auto p = w.get<pos>(e);
        auto v = w.get<vel>(e);
        out.push_back(row{static_cast<std::int64_t>(e.bits()), p ? 1 : 0, p ? (*p)->x : 0,
                          p ? (*p)->y : 0, v ? 1 : 0, v ? (*v)->v : 0});
      }
    }
  }
  return out;
}

struct outcome {
  std::vector<ecs::flush_report::op> applied;
  std::vector<ecs::entity> spawned;
  std::vector<std::uint32_t> spawn_seq;
  std::uint32_t stale = 0;
  std::uint32_t refused = 0;
  std::vector<row> state;
  std::vector<row> targets;
};

struct rig {
  ecs::component_registry registry;
  mem::tracking_resource world_memory{"cmd.order.world"};
  mem::tracking_resource arena_memory{"cmd.order.arena"};
  std::unique_ptr<ecs::world> w;
  std::array<ecs::entity, target_count> targets{};

  rig() {
    REQUIRE(registry.add<pos>("pos").has_value());
    REQUIRE(registry.add<vel>("vel").has_value());
    REQUIRE(registry.freeze().has_value());
    auto made = ecs::world::make(registry, world_memory);
    REQUIRE(made.has_value());
    w = std::move(*made);
    for (ecs::entity &t : targets) {
      auto e = w->create_with(pos{.x = -1, .y = -1});
      REQUIRE(e.has_value());
      t = e.value_or(ecs::entity{});
    }
  }

  outcome finish(ecs::command_queue &q) {
    auto report = q.flush(*w);
    REQUIRE(report.has_value());
    outcome o;
    o.applied = report->applied;
    for (const auto &[p, e] : report->resolved) {
      o.spawned.push_back(e);
      o.spawn_seq.push_back(p.seq);
    }
    o.stale = report->skipped_stale;
    o.refused = report->skipped_refused;
    o.state = state_of(*w);
    for (const ecs::entity t : targets) {
      auto p = w->get<pos>(t);
      auto v = w->get<vel>(t);
      o.targets.push_back(row{static_cast<std::int64_t>(t.bits()), p ? 1 : 0, p ? (*p)->x : 0,
                              p ? (*p)->y : 0, v ? 1 : 0, v ? (*v)->v : 0});
    }
    return o;
  }
};

// The jobs in key order.
std::vector<std::uint32_t> key_order() {
  std::vector<std::uint32_t> js(job_count);
  std::ranges::iota(js, 0U);
  std::ranges::sort(js, [](std::uint32_t a, std::uint32_t b) { return key_of(a) < key_of(b); });
  return js;
}

// A seeded permutation of 0..n-1 (splitmix64 Fisher-Yates).
std::vector<std::uint32_t> permutation(std::uint64_t seed) {
  std::vector<std::uint32_t> p(job_count);
  std::ranges::iota(p, 0U);
  std::uint64_t s = seed;
  for (std::size_t i = p.size() - 1; i > 0; --i) {
    s += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = s;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    z ^= z >> 31U;
    std::swap(p[i], p[z % (i + 1)]);
  }
  return p;
}

// The serial reference: every buffer opened and recorded on the owner thread.
outcome serial_reference() {
  rig r;
  mem::frame_arena arena(r.arena_memory, 0);
  ecs::command_queue q(r.registry, arena);
  for (std::uint32_t j = 0; j < job_count; ++j) {
    record(q.open(key_of(j)), j, r.targets.data());
  }
  return r.finish(q);
}

// One run on `workers` workers: chained in `order` (completion order = order) or unchained.
outcome parallel_run(jobs::job_system &js, std::uint32_t workers,
                     const std::vector<std::uint32_t> &order, bool chained) {
  rig r;
  mem::frame_arena arena(r.arena_memory, workers);
  ecs::command_queue q(r.registry, arena);
  rt::job_counter done;
  jobs::job_handle prev{};
  for (std::size_t i = 0; i < order.size(); ++i) {
    const std::array<jobs::dependency, 1> deps{jobs::dependency::on(prev)};
    const std::size_t n_deps = chained && i != 0 ? 1 : 0;
    prev = js.submit(
        jobs::job_desc{.fn = record_job{.queue = &q, .targets = r.targets.data(), .j = order[i]},
                       .name = "cmd.record"},
        std::span<const jobs::dependency>(deps.data(), n_deps), &done);
  }
  REQUIRE(js.wait(done));
  CHECK(q.open_count() == job_count);
  return r.finish(q);
}

jobs::job_config config(std::uint32_t workers) {
  jobs::job_config c;
  c.workers = workers;
  c.capacity = 4096;
  return c;
}

} // namespace

TEST_CASE("RT-INV-CMD-01: the serial reference plays back in ordered_key order") {
  const outcome ref = serial_reference();
  // The model: apply each job in key order.
  struct target_model {
    bool alive = true;
    std::int32_t x = -1;
    std::int32_t y = -1;
    bool has_vel = false;
    std::int32_t v = 0;
  };
  std::array<target_model, target_count> model{};
  std::uint32_t stale = 0;
  std::uint32_t refused = 0;
  const std::vector<std::uint32_t> order = key_order();
  for (const std::uint32_t j : order) {
    target_model &t = model.at(j % target_count);
    const std::uint32_t commands = j % 12 == 11 ? 3 : 2;
    if (!t.alive) {
      stale += commands;
      continue;
    }
    t.x = static_cast<std::int32_t>(j);
    t.y = 1;
    if (t.has_vel) {
      ++refused;
    } else {
      t.has_vel = true;
      t.v = static_cast<std::int32_t>(j);
    }
    if (j % 12 == 11) {
      t.alive = false;
    }
  }
  CHECK(ref.stale == stale);
  CHECK(ref.refused == refused);
  REQUIRE(ref.targets.size() == target_count);
  for (std::size_t t = 0; t < target_count; ++t) {
    INFO("target " << t);
    const target_model &m = model.at(t);
    const row &got = ref.targets.at(t);
    CHECK((got[1] == 1) == m.alive);
    if (m.alive) {
      CHECK(got[2] == m.x);
      CHECK(got[3] == m.y);
      CHECK(got[4] == (m.has_vel ? 1 : 0));
      CHECK(got[5] == m.v);
    }
  }
  // Spawns resolve in key order: the n-th resolved entity is the spawn of the n-th job in key
  // order, and the spawn entries of `applied` come in the same order.
  REQUIRE(ref.spawned.size() == job_count);
  std::vector<ecs::entity> spawn_entries;
  for (const auto &op : ref.applied) {
    if (op.kind == ecs::flush_report::op_kind::spawn) {
      spawn_entries.push_back(op.target);
    }
  }
  CHECK(spawn_entries == ref.spawned);
  for (std::size_t n = 0; n < order.size(); ++n) {
    CHECK(ref.spawn_seq.at(n) == 0U);
    const auto x = std::ranges::find_if(
        ref.state, [&](const row &r) { return std::cmp_equal(r[0], ref.spawned.at(n).bits()); });
    REQUIRE(x != ref.state.end());
    CHECK(std::cmp_equal((*x)[2], order.at(n)));
    CHECK(std::cmp_equal((*x)[5], order.at(n)));
  }
}

namespace {

// Every run on each worker count equals the serial reference.
void check_against_serial(std::initializer_list<std::uint32_t> worker_counts) {
  const outcome ref = serial_reference();
  for (const std::uint32_t workers : worker_counts) {
    rtt::scripted_platform plat;
    auto js = jobs::job_system::start(plat.get(), config(workers));
    REQUIRE(js.has_value());
    for (const std::uint64_t seed : {1ULL, 2ULL, 3ULL}) {
      INFO("workers " << workers << " seed " << seed);
      std::vector<std::uint32_t> order = permutation(seed + workers);
      if (seed == 1) {
        order = key_order();
        std::ranges::reverse(order); // completion in reverse key order
      }
      const outcome got = parallel_run(**js, workers, order, true);
      CHECK(got.applied == ref.applied);
      CHECK(got.spawned == ref.spawned);
      CHECK(got.spawn_seq == ref.spawn_seq);
      CHECK(got.stale == ref.stale);
      CHECK(got.refused == ref.refused);
      CHECK(got.state == ref.state);
    }
    INFO("workers " << workers << " unchained");
    const outcome free_run = parallel_run(**js, workers, permutation(99), false);
    CHECK(free_run.applied == ref.applied);
    CHECK(free_run.state == ref.state);
    REQUIRE((*js)->stop());
  }
}

} // namespace

TEST_CASE("RT-INV-CMD-01: 1, 2 and 4 workers with permuted completion give the serial playback") {
  check_against_serial({1U, 2U, 4U});
}

TEST_CASE("RT-INV-CMD-01: 8 and 16 workers with permuted completion give the serial playback") {
  check_against_serial({8U, 16U});
}
