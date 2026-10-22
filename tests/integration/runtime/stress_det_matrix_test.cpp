// RT-INV-STRESS-05 (SL-ARCH-003 ARCH3-R084, R085, R090, `stress_matrix` S7, `failpoints` RFP4):
// the per-tick fixture hash of testing::sim_fixture (4400 entities, the eight reference systems,
// one world seed) is the same in every cell of the determinism matrix: workers {1, 2, 4, 8, 16}
// under the parallel policy and under serial_canonical, and 8 chaos cells (workers cycling 2, 4,
// 8, 16) where RFP4 (worker.stall) is armed again before every tick at a seeded visit with a
// seeded stall of 0 to 2000 us. In every cell the correlation ids are perturbed: before every
// tick the owner thread opens 0 to 3 extra correlation_scopes (each draws a new id) around the
// frame that submits the jobs. 20 ticks per cell (scaled); every hash vector equals the one of
// the serial_canonical cell on 1 worker. The chaos cells are skipped when the failpoints are
// compiled out.

#include "sl/core/log.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/faults.hpp"

#include "../../../engine/runtime/testing/faults_arm.hpp"
#include "../../../engine/runtime/testing/sim_fixture.hpp"
#include "../../../engine/runtime/testing/stress_harness.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace ecs = sl::runtime::ecs;
namespace rtt = sl::runtime::testing;

constexpr const char *test_name = "runtime_stress_det_matrix_test";
constexpr std::uint64_t world_seed = 0x5EED0990007ULL;
constexpr std::uint64_t max_stall_us = 2000;

std::uint64_t splitmix64(std::uint64_t &state) noexcept {
  state += 0x9E3779B97F4A7C15ULL;
  std::uint64_t z = state;
  z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31U);
}

struct cell {
  std::uint32_t workers = 1;
  ecs::execution_policy policy = ecs::execution_policy::parallel;
  std::optional<std::uint64_t> chaos_seed; // RFP4 stalls when set
};

struct cell_outcome {
  cell c;
  std::string error;
  std::uint64_t stalls_armed = 0;
  std::uint64_t scopes_opened = 0;
  std::vector<sl::fabric::digest> hashes;
};

// One tick under 0 to 3 extra correlation scopes (nested, so each one draws a fresh id).
sl::result<std::vector<sl::fabric::digest>> perturbed_tick(rtt::sim_fixture &fx, std::uint64_t r,
                                                           std::uint64_t &opened) {
  const std::uint64_t extra = r % 4U;
  opened += extra;
  std::optional<sl::log::correlation_scope> a;
  std::optional<sl::log::correlation_scope> b;
  std::optional<sl::log::correlation_scope> c;
  if (extra >= 1) {
    a.emplace("stress.perturb", r);
  }
  if (extra >= 2) {
    b.emplace("stress.perturb", r >> 8U);
  }
  if (extra >= 3) {
    c.emplace("stress.perturb", r >> 16U);
  }
  return fx.run(1);
}

cell_outcome run_cell(const cell &c, std::uint32_t ticks, std::uint64_t seed) {
  cell_outcome out;
  out.c = c;
  auto made = rtt::sim_fixture::make(
      {.workers = c.workers, .policy = c.policy, .ticks = ticks, .seed = world_seed});
  if (!made) {
    out.error = "make: " + made.error().message;
    return out;
  }
  std::uint64_t state = seed ^ (c.chaos_seed.value_or(0) * 0x100000001B3ULL) ^ c.workers;
  for (std::uint32_t t = 0; t < ticks; ++t) {
    const std::uint64_t r = splitmix64(state);
    if (c.chaos_seed) {
      const std::uint64_t at_visit = 1 + (splitmix64(state) % 64U);
      const auto stall_us = static_cast<std::uint32_t>(splitmix64(state) % (max_stall_us + 1));
      if (!rtt::arm(rt::failpoint::rfp4, at_visit, false, stall_us)) {
        out.error = "arm RFP4 failed";
        return out;
      }
      ++out.stalls_armed;
    }
    auto hashes = perturbed_tick(**made, r, out.scopes_opened);
    if (!hashes || hashes->size() != 1) {
      out.error = hashes ? "a tick returned no hash" : hashes.error().message;
      break;
    }
    out.hashes.push_back(hashes->front());
  }
  rtt::disarm(rt::failpoint::rfp4);
  return out;
}

} // namespace

TEST_CASE("RT-INV-STRESS-05: S7 per-tick hashes are equal for workers 1 to 16, serial_canonical, "
          "RFP4 chaos stalls under 8 seeds and perturbed correlation ids") {
  const rtt::cap_timer cap;
  const std::uint64_t seed = rtt::stress_seed(0x5EED0990017ULL);
  SL_STRESS_SEED(guard, test_name, seed);
  // 20 ticks per cell (scaled), not the table's 300: a Debug tick with its fixture hash costs about
  // 37 ms at 4400 entities, so 18 cells x 300 ticks would take about 200 s (deviation WO-098).
  const auto ticks = static_cast<std::uint32_t>(rtt::scaled(20));
  std::vector<cell> cells;
  for (const auto policy :
       {ecs::execution_policy::serial_canonical, ecs::execution_policy::parallel}) {
    for (const std::uint32_t workers : {1U, 2U, 4U, 8U, 16U}) {
      cells.push_back({.workers = workers, .policy = policy, .chaos_seed = std::nullopt});
    }
  }
  if (rt::faults_compiled) {
    constexpr std::array<std::uint32_t, 4> chaos_workers = {2, 4, 8, 16};
    for (std::uint64_t s = 0; s < 8; ++s) {
      cells.push_back({.workers = chaos_workers.at(s % 4),
                       .policy = ecs::execution_policy::parallel,
                       .chaos_seed = seed + s});
    }
  }
  std::vector<cell_outcome> outcomes;
  REQUIRE(rtt::run_watched([&] {
    std::ranges::transform(cells, std::back_inserter(outcomes),
                           [&](const cell &c) { return run_cell(c, ticks, seed); });
  }));
  REQUIRE(outcomes.size() == cells.size());
  const cell_outcome &reference = outcomes.front();
  REQUIRE(reference.error.empty());
  REQUIRE(reference.hashes.size() == ticks);
  for (const cell_outcome &o : outcomes) {
    INFO("workers " << o.c.workers << " policy "
                    << (o.c.policy == ecs::execution_policy::parallel ? "parallel"
                                                                      : "serial_canonical")
                    << " chaos seed " << o.c.chaos_seed.value_or(0) << " ticks " << ticks);
    CHECK(o.error.empty());
    REQUIRE(o.hashes.size() == reference.hashes.size());
    std::uint32_t first_diff = 0;
    while (first_diff < o.hashes.size() &&
           o.hashes.at(first_diff) == reference.hashes.at(first_diff)) {
      ++first_diff;
    }
    INFO("first differing tick " << first_diff + 1);
    CHECK(first_diff == o.hashes.size());
    if (o.c.chaos_seed) {
      CHECK(o.stalls_armed == ticks);
    }
  }
  const std::uint64_t scopes =
      std::accumulate(outcomes.begin(), outcomes.end(), std::uint64_t{0},
                      [](std::uint64_t n, const cell_outcome &o) { return n + o.scopes_opened; });
  CHECK(scopes > 0);
  SL_STRESS_CAP(cap);
}
