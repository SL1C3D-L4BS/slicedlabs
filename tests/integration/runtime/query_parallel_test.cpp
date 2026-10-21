// RT-INV-QUERY-03 (SL-ARCH-003 ARCH3-R116, ADR-0026, `determinism` ordered_key chunk_index):
// par_for_each visits each match exactly once on 1 to 16 workers, and the batch partition is a
// function of the data only. A world of five archetypes (four matching, about 20000 rows) is
// iterated on 1, 2, 3, 4, 8 and 16 workers and with a one-record job pool (every batch beyond
// the first runs on the caller after job_capacity): every matched row is visited once per call,
// its chunk_index is the batch an independent partition gives (whole chunks in creation order, a
// batch closing at 4096 rows or more), each batch's rows arrive in (archetype, chunk, row) order,
// and the write term stamps every visited row. Rounds per worker count scale with
// SL_RUNTIME_STRESS_SCALE (0.1 under the sanitizers, floored at 1).

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/query.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/memory.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"
#include "../../../engine/runtime/testing/world_gen.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

namespace {

namespace ecs = sl::runtime::ecs;
namespace jobs = sl::runtime::jobs;
namespace mem = sl::runtime::mem;
namespace rtt = sl::runtime::testing;

using rtt::gen_health;
using rtt::gen_position;
using rtt::gen_tag;
using rtt::gen_velocity;

std::uint32_t scaled(double base) {
  double scale = 1.0;
  if (const char *env = std::getenv("SL_RUNTIME_STRESS_SCALE"); env != nullptr && *env != '\0') {
    scale = std::strtod(env, nullptr);
  }
  return std::max<std::uint32_t>(1, static_cast<std::uint32_t>(base * scale));
}

struct fixture {
  fixture() {
    auto got = rtt::register_components(registry);
    REQUIRE(got.has_value());
    ids = *got;
    REQUIRE(registry.freeze().has_value());
    auto made = ecs::world::make(registry, memory);
    REQUIRE(made.has_value());
    w = std::move(*made);
    for (int i = 0; i < 9000; ++i) {
      REQUIRE(w->create_with(gen_position{}, gen_velocity{}).has_value());
    }
    for (int i = 0; i < 3000; ++i) {
      REQUIRE(w->create_with(gen_position{}).has_value());
    }
    for (int i = 0; i < 1000; ++i) {
      REQUIRE(w->create_with(gen_velocity{}).has_value()); // does not match
    }
    for (int i = 0; i < 500; ++i) {
      REQUIRE(w->create_with(gen_position{}, gen_health{}).has_value());
    }
    for (int i = 0; i < 7000; ++i) {
      REQUIRE(w->create_with(gen_position{}, gen_velocity{}, gen_tag{}).has_value());
    }
  }
  ecs::component_registry registry;
  rtt::gen_ids ids;
  mem::tracking_resource memory{"query.parallel"};
  std::unique_ptr<ecs::world> w;
};

using batches = std::vector<std::vector<ecs::entity>>;

// The independent partition: matched archetypes in creation order, whole non-empty chunks, a
// batch closing once it holds at least par_batch_min_rows rows.
batches partition(const ecs::world &w, ecs::component_id position) {
  batches out;
  std::uint32_t rows = 0;
  bool open = false;
  for (const ecs::archetype *a : w.archetypes()) {
    if (!a->contains(position)) {
      continue;
    }
    for (std::uint32_t c = 0; c < a->chunk_count(); ++c) {
      if (a->rows(c) == 0) {
        continue;
      }
      if (!open) {
        out.emplace_back();
        open = true;
      }
      const auto es = a->entities(c);
      out.back().insert(out.back().end(), es.begin(), es.end());
      rows += a->rows(c);
      if (rows >= ecs::par_batch_min_rows) {
        rows = 0;
        open = false;
      }
    }
  }
  return out;
}

struct run_result {
  batches seen;
  std::uint64_t twice = 0;     // rows visited more than once
  std::uint64_t missed = 0;    // matched rows not visited
  std::uint64_t foreign = 0;   // a chunk_index outside the partition
  std::uint64_t unstamped = 0; // visited velocity rows whose changed tick is not the write tick
  bool ok = false;
};

using par_query = ecs::query<ecs::read<gen_position>, ecs::optional<gen_velocity>>;

// One par_for_each over the world; each batch appends to its own vector (one job per batch).
run_result run(fixture &f, par_query &q, jobs::job_system &js, std::uint32_t tick,
               std::size_t expected_batches) {
  run_result r;
  r.seen.resize(expected_batches);
  std::vector<std::atomic<std::uint32_t>> visits(f.w->entities().next_index());
  std::atomic<std::uint64_t> foreign{0};
  auto done = q.par_for_each(
      *f.w, js, {.since = 0, .write_tick = tick},
      [&](std::uint32_t batch, ecs::entity e, const gen_position &, gen_velocity *v) noexcept {
        visits[e.index].fetch_add(1);
        if (v != nullptr) {
          v->dx += 1.0;
        }
        if (batch >= expected_batches) {
          foreign.fetch_add(1);
          return;
        }
        r.seen[batch].push_back(e);
      });
  r.ok = done.has_value();
  r.foreign = foreign.load();
  const ecs::component_id position = f.ids.ids.at(0);
  const ecs::component_id velocity = f.ids.ids.at(1);
  for (const ecs::archetype *a : f.w->archetypes()) {
    for (std::uint32_t c = 0; c < a->chunk_count(); ++c) {
      const auto es = a->entities(c);
      const auto ticks = a->changed_ticks(velocity, c);
      for (std::size_t row = 0; row < es.size(); ++row) {
        const std::uint32_t n = visits[es[row].index].load();
        if (!a->contains(position)) {
          r.foreign += n;
          continue;
        }
        r.twice += n > 1 ? 1 : 0;
        r.missed += n == 0 ? 1 : 0;
        if (!ticks.empty() && ticks[row] != tick) {
          ++r.unstamped;
        }
      }
    }
  }
  return r;
}

} // namespace

TEST_CASE("RT-INV-QUERY-03: par_for_each visits each match once on 1 to 16 workers") {
  fixture f;
  const batches want = partition(*f.w, f.ids.ids.at(0));
  REQUIRE(want.size() >= 4);
  for (std::size_t b = 0; b + 1 < want.size(); ++b) {
    CHECK(want[b].size() >= ecs::par_batch_min_rows);
  }
  auto q = par_query::make(f.registry);
  REQUIRE(q.has_value());
  const std::uint32_t rounds = scaled(10);
  std::uint32_t tick = 1;
  for (const std::uint32_t workers : {1U, 2U, 3U, 4U, 8U, 16U}) {
    CAPTURE(workers);
    rtt::scripted_platform plat;
    jobs::job_config c;
    c.workers = workers;
    c.capacity = 1024;
    auto js = jobs::job_system::start(plat.get(), std::move(c));
    REQUIRE(js.has_value());
    for (std::uint32_t round = 0; round < rounds; ++round) {
      ++tick;
      const run_result r = run(f, *q, **js, tick, want.size());
      CHECK(r.ok);
      CHECK(r.twice == 0);
      CHECK(r.missed == 0);
      CHECK(r.foreign == 0);
      CHECK(r.unstamped == 0);
      CHECK(r.seen == want);
    }
    REQUIRE((*js)->stop().has_value());
  }
  // Every velocity value was incremented exactly once per call.
  const double calls = 6.0 * rounds;
  std::uint64_t off = 0;
  auto check = ecs::query<ecs::read<gen_velocity>>::make(f.registry);
  REQUIRE(check.has_value());
  check->for_each(*f.w, {}, [&off, calls](ecs::entity, const gen_velocity &v) noexcept {
    off += v.dx == calls ? 0 : 1;
  });
  CHECK(off == 1000); // only the velocity-only archetype, which does not match, stayed at 0
}

TEST_CASE("RT-INV-QUERY-03: a full job pool runs batches on the caller with the same partition") {
  fixture f;
  const batches want = partition(*f.w, f.ids.ids.at(0));
  auto q = par_query::make(f.registry);
  REQUIRE(q.has_value());
  rtt::scripted_platform plat;
  jobs::job_config c;
  c.workers = 4;
  c.capacity = 1;
  auto js = jobs::job_system::start(plat.get(), std::move(c));
  REQUIRE(js.has_value());
  for (std::uint32_t round = 0; round < scaled(10); ++round) {
    const run_result r = run(f, *q, **js, 100 + round, want.size());
    CHECK(r.ok);
    CHECK(r.twice == 0);
    CHECK(r.missed == 0);
    CHECK(r.foreign == 0);
    CHECK(r.unstamped == 0);
    CHECK(r.seen == want);
  }
  REQUIRE((*js)->stop().has_value());
}

TEST_CASE("RT-INV-QUERY-03: a change filter keeps the batches but selects no unchanged row") {
  fixture f;
  auto q = ecs::query<ecs::changed<gen_position>>::make(f.registry);
  REQUIRE(q.has_value());
  rtt::scripted_platform plat;
  jobs::job_config c;
  c.workers = 3;
  c.capacity = 1024;
  auto js = jobs::job_system::start(plat.get(), std::move(c));
  REQUIRE(js.has_value());
  // Nothing changed after tick 0: every batch runs, none visits a row.
  std::atomic<std::uint64_t> rows{0};
  const std::uint64_t chunks_before = q->chunks_visited();
  auto done = q->par_for_each(*f.w, **js, {.since = 0},
                              [&rows](std::uint32_t, ecs::entity) noexcept { rows.fetch_add(1); });
  CHECK(done.has_value());
  CHECK(rows.load() == 0);
  CHECK(q->chunks_visited() > chunks_before);
  REQUIRE((*js)->stop().has_value());
}
