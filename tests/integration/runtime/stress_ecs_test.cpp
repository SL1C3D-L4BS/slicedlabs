// RT-INV-STRESS-04 (SL-ARCH-003 ARCH3-R166, R051, R105, `stress_matrix` S6, `limits`
// warmup_frames): ECS churn over testing::sim_fixture (the eight reference systems). For entities
// {0, 1, 1e3, 1e5} on workers {1, 2, 4, 8, 16}: before every frame the owner thread despawns 5% of
// the tracked entities, spawns as many mortals and adds or removes gen_large on another 5%, all
// drawn from the case seed. After every frame no despawned handle resolves (is_alive false, get
// stale_entity; this tick's handles and 64 older ones per tick, every one after the last tick),
// the rows of all archetypes equal the live entities, and a steady frame (after the 10 warm-up
// frames; the frame arenas of both sets are warmed first, as in RT-INV-FRAME-04) emits zero sl::log
// events (a memory sink on the default logger) and takes zero upstream allocations
// (frame_stats::upstream_allocs and the tracking_resource counters of the arena's upstream). The
// per-tick summary (population, energy, live entities, archetypes) and the fixture hash are equal
// on every worker count (schedule independence). The 1e3 and 1e5 entity counts scale with
// SL_RUNTIME_STRESS_SCALE. The 1e5 row runs 5 churn frames on 1 and 16 workers and compares the
// per-tick summary only: at 1e5 a Debug tick costs about 0.5 s and the fixture hash about 3 s, and
// the fixture's population transient (its first mortals dying) outlasts the warm-up, so its
// steady-frame rule is carried by the 0, 1 and 1e3 rows (deviation WO-098).

#include "sl/core/error.hpp"
#include "sl/core/log.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/ecs/command_buffer.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/frame.hpp"

#include "../../../engine/runtime/testing/sim_fixture.hpp"
#include "../../../engine/runtime/testing/stress_harness.hpp"
#include "../../../engine/runtime/testing/world_gen.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace ecs = sl::runtime::ecs;
namespace rtt = sl::runtime::testing;

constexpr const char *test_name = "runtime_stress_ecs_test";
constexpr std::uint32_t hash_every = 50;
constexpr std::uint32_t big = 100000;
// The 1e5 row: 5 churn frames, all inside the warm-up (deviation WO-098).
constexpr std::uint32_t big_ticks = 5;

std::uint64_t splitmix64(std::uint64_t &state) noexcept {
  state += 0x9E3779B97F4A7C15ULL;
  std::uint64_t z = state;
  z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31U);
}

struct cell_outcome {
  std::uint32_t entities = 0;
  std::uint32_t workers = 0;
  std::uint32_t ticks = 0;
  bool made = false;
  bool warmed = false;
  std::string frame_error;
  std::uint64_t churn_errors = 0;
  std::uint64_t stale_checked = 0;
  std::uint64_t stale_resolved = 0;
  std::uint64_t row_mismatch = 0;
  std::uint64_t steady_log_events = 0;
  std::uint64_t steady_upstream = 0;
  std::uint64_t despawned = 0;
  std::uint64_t toggled = 0;
  double wall_s = 0.0;
  std::vector<std::uint64_t> summaries;
  std::vector<sl::fabric::digest> hashes;
};

std::uint64_t rows(const ecs::world &w) {
  return std::accumulate(
      w.archetypes().begin(), w.archetypes().end(), std::uint64_t{0},
      [](std::uint64_t n, const ecs::archetype *a) { return n + a->row_count(); });
}

bool resolves(ecs::world &w, ecs::entity e) {
  return w.is_alive(e) || w.get<rtt::gen_position>(e).has_value();
}

// Population, energy, live entities and archetypes, folded.
std::uint64_t summary(const rtt::sim_fixture &fx, const ecs::world &w) {
  const auto energy = std::bit_cast<std::uint64_t>(fx.stats().energy);
  std::uint64_t h = fx.stats().population;
  for (const std::uint64_t v :
       {energy, std::uint64_t{w.entities().alive()}, std::uint64_t{w.archetypes().size()}}) {
    h ^= v;
    h = splitmix64(h);
  }
  return h;
}

// Every entity of the seeded world.
std::vector<ecs::entity> live_entities(const ecs::world &w) {
  std::vector<ecs::entity> out;
  for (const ecs::archetype *a : w.archetypes()) {
    for (std::uint32_t c = 0; c < a->chunk_count(); ++c) {
      const auto chunk = a->entities(c);
      out.insert(out.end(), chunk.begin(), chunk.end());
    }
  }
  return out;
}

// Despawns, spawns and toggles 5% of the tracked entities each.
void churn(ecs::world &w, std::vector<ecs::entity> &tracked, std::vector<ecs::entity> &stale,
           std::uint64_t &state, cell_outcome &out) {
  const std::size_t k = tracked.empty() ? 0 : std::max<std::size_t>(1, tracked.size() / 20);
  for (std::size_t i = 0; i < k && !tracked.empty(); ++i) {
    const std::size_t at = splitmix64(state) % tracked.size();
    const ecs::entity e = tracked.at(at);
    tracked.at(at) = tracked.back();
    tracked.pop_back();
    if (w.is_alive(e)) {
      out.churn_errors += w.destroy(e).has_value() ? 0U : 1U;
      ++out.despawned;
    }
    stale.push_back(e); // despawned here or reaped by sim.reaper
  }
  for (std::size_t i = 0; i < k; ++i) {
    const std::uint64_t r = splitmix64(state);
    const rtt::gen_position at{.x = static_cast<float>(r % 200U), .y = 1.0F, .z = 0.0F};
    const rtt::gen_aligned aligned{.value = static_cast<std::uint32_t>(r >> 32U)};
    const rtt::gen_health hp{.hp = 1 + static_cast<std::int32_t>(r % 30U),
                             .armor = static_cast<std::int16_t>((r >> 8U) % 4U)};
    auto made = w.create_with(at, rtt::gen_velocity{.dx = 0.5, .dy = -0.25}, aligned, hp);
    if (!made) {
      ++out.churn_errors;
      continue;
    }
    tracked.push_back(*made);
  }
  for (std::size_t i = 0; i < k && !tracked.empty(); ++i) {
    const ecs::entity e = tracked.at(splitmix64(state) % tracked.size());
    if (!w.is_alive(e)) {
      continue;
    }
    const bool had = w.get<rtt::gen_large>(e).has_value();
    const bool ok =
        had ? w.remove<rtt::gen_large>(e).has_value() : w.add(e, rtt::gen_large{}).has_value();
    out.churn_errors += ok ? 0U : 1U;
    out.toggled += ok ? 1U : 0U;
  }
}

// Warms every frame arena of both sets from this thread, as RT-INV-FRAME-04 does: which worker
// runs which recording job cannot move a first block past the warm-up. A flip unbinds the set.
bool warm_arenas(rtt::sched_rig &rig) {
  for (int set = 0; set < 2; ++set) {
    for (std::uint32_t k = 0; k <= rig.arena->workers(); ++k) {
      rt::mem::resource &a = rig.arena->for_worker(k);
      if (!a.try_allocate(ecs::command_buffer_block_bytes, 16, a.default_tag())) {
        return false;
      }
    }
    rig.arena->flip();
  }
  return true;
}

cell_outcome run_cell(std::uint32_t entities, std::uint32_t workers, std::uint64_t seed,
                      const sl::log::memory_sink &events) {
  const rtt::cap_timer timer;
  cell_outcome out;
  out.entities = entities >= 1000 ? static_cast<std::uint32_t>(rtt::scaled(entities)) : entities;
  out.workers = workers;
  out.ticks = static_cast<std::uint32_t>(entities >= big ? big_ticks : rtt::scaled(300));
  const bool hash_ticks = entities < big;
  auto made = rtt::sim_fixture::make({.workers = workers,
                                      .policy = ecs::execution_policy::parallel,
                                      .ticks = out.ticks,
                                      .seed = seed,
                                      .entities = out.entities});
  if (!made) {
    return out;
  }
  out.made = true;
  rtt::sim_fixture &fx = **made;
  rtt::sched_rig &rig = fx.rig();
  out.warmed = warm_arenas(rig);
  if (!out.warmed) {
    return out;
  }
  std::vector<ecs::entity> tracked = live_entities(*rig.world);
  std::vector<ecs::entity> stale;
  std::size_t fresh_from = 0;
  std::size_t cursor = 0;
  std::uint64_t state = seed ^ (std::uint64_t{entities} << 20U);
  for (std::uint32_t t = 1; t <= out.ticks; ++t) {
    churn(*rig.world, tracked, stale, state, out);
    const std::uint64_t events_before = events.total();
    const std::uint64_t upstream_before = rig.upstream.totals().allocs;
    auto frame = rig.step();
    if (!frame) {
      out.frame_error = frame.error().message;
      return out;
    }
    if (rig.systems->failures() != 0 || !rig.systems->last_flush()) {
      out.frame_error = "a system or the command flush failed";
      return out;
    }
    if (t > rt::warmup_frames_default) {
      out.steady_log_events += events.total() - events_before;
      out.steady_upstream += frame->upstream_allocs;
      out.steady_upstream += rig.upstream.totals().allocs - upstream_before;
    }
    for (std::size_t i = fresh_from; i < stale.size(); ++i) {
      out.stale_resolved += resolves(*rig.world, stale.at(i)) ? 1U : 0U;
      ++out.stale_checked;
    }
    for (std::size_t i = 0; i < 64 && fresh_from > 0; ++i) {
      out.stale_resolved += resolves(*rig.world, stale.at(cursor++ % fresh_from)) ? 1U : 0U;
      ++out.stale_checked;
    }
    fresh_from = stale.size();
    out.row_mismatch += rows(*rig.world) == rig.world->entities().alive() ? 0U : 1U;
    out.summaries.push_back(summary(fx, *rig.world));
    if (hash_ticks && (t % hash_every == 0 || t == out.ticks)) {
      out.hashes.push_back(fx.fixture_hash());
    }
  }
  for (const ecs::entity e : stale) {
    out.stale_resolved += resolves(*rig.world, e) ? 1U : 0U;
    ++out.stale_checked;
  }
  out.wall_s = timer.elapsed_s();
  return out;
}

void check_row(std::uint32_t entities) {
  const rtt::cap_timer cap;
  const std::uint64_t seed = rtt::stress_seed(0x5EED0990006ULL);
  SL_STRESS_SEED(guard, test_name, seed);
  auto events = std::make_shared<sl::log::memory_sink>(64);
  sl::log::default_logger().add_sink(events);
  std::vector<cell_outcome> outcomes;
  // The 1e5 row runs on 1 and 16 workers only, within the file cap (deviation WO-098).
  const std::vector<std::uint32_t> workers = entities >= big
                                                 ? std::vector<std::uint32_t>{1, 16}
                                                 : std::vector<std::uint32_t>{1, 2, 4, 8, 16};
  auto watched = rtt::run_watched([&] {
    std::ranges::transform(workers, std::back_inserter(outcomes),
                           [&](std::uint32_t w) { return run_cell(entities, w, seed, *events); });
  });
  sl::log::default_logger().remove_sink(events.get());
  REQUIRE(watched);
  REQUIRE(outcomes.size() == workers.size());
  for (const cell_outcome &o : outcomes) {
    const cell_outcome &first = outcomes.front();
    INFO("entities " << o.entities << " workers " << o.workers << " ticks " << o.ticks);
    MESSAGE("S6 cell: entities " << o.entities << " workers " << o.workers << " ticks " << o.ticks
                                 << " wall " << o.wall_s << " s");
    REQUIRE(o.made);
    REQUIRE(o.warmed);
    CHECK(o.frame_error.empty());
    CHECK(o.churn_errors == 0);
    CHECK(o.stale_resolved == 0);
    CHECK(o.row_mismatch == 0);
    CHECK(o.steady_log_events == 0);
    CHECK(o.steady_upstream == 0);
    CHECK(o.summaries.size() == o.ticks);
    CHECK(o.hashes.empty() == (o.entities >= rtt::scaled(big)));
    if (o.entities >= 20) {
      CHECK(o.despawned > 0);
      CHECK(o.toggled > 0);
      CHECK(o.stale_checked > 0);
    }
    CHECK(o.summaries == first.summaries);
    CHECK(o.hashes == first.hashes);
  }
  SL_STRESS_CAP(cap);
}

} // namespace

TEST_CASE("RT-INV-STRESS-04: S6 ECS churn on an empty world keeps steady frames silent and "
          "allocation-free on 1 to 16 workers") {
  check_row(0);
}

TEST_CASE("RT-INV-STRESS-04: S6 ECS churn on 1 entity resolves no stale handle, keeps rows equal "
          "to live entities and hashes schedule-independently on 1 to 16 workers") {
  check_row(1);
}

TEST_CASE("RT-INV-STRESS-04: S6 ECS churn on 1e3 entities resolves no stale handle, keeps rows "
          "equal to live entities, hashes schedule-independently and keeps steady frames silent "
          "and allocation-free on 1 to 16 workers") {
  check_row(1000);
}

TEST_CASE("RT-INV-STRESS-04: S6 ECS churn on 1e5 entities resolves no stale handle, keeps rows "
          "equal to live entities and summarises schedule-independently on 1 and 16 workers") {
  check_row(big);
}
