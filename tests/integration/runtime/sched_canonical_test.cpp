// RT-INV-SCHED-06 (SL-ARCH-003 ARCH3-R090): serial_canonical and parallel give identical per-tick
// state hashes. testing::sim_fixture (eight reference systems: four par_for_each, two recording
// commands, one reduction merged by ordered_key) runs the same seeds under serial_canonical (the
// oracle: every system and batch inline on the owner thread in ordered_key order) and parallel on
// 1, 2, 4 and 8 workers; every run gives the same hash after every tick as the serial run on one
// worker. The run is not trivial: entities spawn and die, the energy sum moves and every hash
// differs from the previous one. Ticks and seeds scale with SL_RUNTIME_STRESS_SCALE.

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/ecs/system.hpp"

#include "../../../engine/runtime/testing/sim_fixture.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <vector>

namespace {

namespace ecs = sl::runtime::ecs;
namespace rtt = sl::runtime::testing;
using sl::fabric::digest;

std::uint32_t stress_scale() {
  const char *text = std::getenv("SL_RUNTIME_STRESS_SCALE");
  if (text == nullptr) {
    return 1;
  }
  const long value = std::strtol(text, nullptr, 10);
  return value < 1 ? 1U : static_cast<std::uint32_t>(value);
}

struct run_result {
  std::vector<digest> hashes;
  std::uint64_t population = 0;
  std::uint64_t ticks = 0;
};

run_result run(std::uint32_t workers, ecs::execution_policy policy, std::uint64_t seed,
               std::uint32_t ticks) {
  rtt::sim_config c;
  c.workers = workers;
  c.policy = policy;
  c.seed = seed;
  c.ticks = ticks;
  auto made = rtt::sim_fixture::make(c);
  REQUIRE(made.has_value());
  rtt::sim_fixture &f = **made;
  auto hashes = f.run();
  if (!hashes) {
    FAIL("sim_fixture::run: " << hashes.error().message);
  }
  return run_result{.hashes = *hashes, .population = f.stats().population, .ticks = f.ticks_run()};
}

} // namespace

TEST_CASE("RT-INV-SCHED-06: serial_canonical and parallel give identical per-tick hashes") {
  const std::uint32_t scale = stress_scale();
  const std::uint32_t ticks = 16 * scale;
  for (std::uint64_t seed = 1; seed <= scale; ++seed) {
    INFO("seed " << seed);
    const run_result oracle = run(1, ecs::execution_policy::serial_canonical, seed, ticks);
    REQUIRE(oracle.hashes.size() == ticks);
    CHECK(oracle.ticks == ticks);
    CHECK(oracle.population > 0);
    std::size_t moved = 0;
    for (std::size_t i = 1; i < oracle.hashes.size(); ++i) {
      moved += oracle.hashes[i] == oracle.hashes[i - 1] ? 0U : 1U;
    }
    CHECK(moved == ticks - 1);
    // The oracle is serial_canonical on one worker; serial on 8 checks it ignores workers.
    const std::array<std::pair<std::uint32_t, ecs::execution_policy>, 5> runs{{
        {1U, ecs::execution_policy::parallel},
        {2U, ecs::execution_policy::parallel},
        {4U, ecs::execution_policy::parallel},
        {8U, ecs::execution_policy::parallel},
        {8U, ecs::execution_policy::serial_canonical},
    }};
    for (const auto &[workers, policy] : runs) {
      INFO("workers " << workers << " policy " << static_cast<int>(policy));
      const run_result other = run(workers, policy, seed, ticks);
      REQUIRE(other.hashes.size() == ticks);
      for (std::size_t i = 0; i < ticks; ++i) {
        INFO("tick " << i);
        CHECK(other.hashes[i] == oracle.hashes[i]);
        if (other.hashes[i] != oracle.hashes[i]) {
          break;
        }
      }
      CHECK(other.population == oracle.population);
    }
  }
}

TEST_CASE("RT-INV-SCHED-06: the reference systems change the world") {
  rtt::sim_config c;
  c.workers = 4;
  c.ticks = 30;
  auto made = rtt::sim_fixture::make(c);
  REQUIRE(made.has_value());
  rtt::sim_fixture &f = **made;
  const std::uint32_t before = f.rig().world->entities().alive();
  auto hashes = f.run();
  REQUIRE(hashes.has_value());
  const std::uint32_t after = f.rig().world->entities().alive();
  CHECK(f.stats().energy > 0.0);
  CHECK(f.stats().population > 0);
  CHECK(f.systems().failures() == 0);
  // mortals die (hp drifts down) and sources spawn new ones: the census moved
  CHECK(after != before);
}
