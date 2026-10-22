// RT-INV-STRESS-02 (SL-ARCH-003 ARCH3-R162, R070, R073, `stress_matrix` S2): deque
// linearizability holds on both queues at stress scale. One owner pushes and pops while 1, 3, 7
// and 15 thieves steal; the queue contract suite (testing/queue_contract.hpp: the sequential
// rules, then the concurrent run where every pushed index is taken exactly once) passes for
// locked_deque and for ws_deque with 1e6 indices per run (scaled; 0.1 under the sanitizers). The
// ws_deque ring grows from its first size to 65536 cells during the runs. Each run uses its own
// seed (base seed + thieves) and runs under the harness watchdog.

#include "sl/runtime/work_queue.hpp"

#include "../../../engine/runtime/testing/queue_contract.hpp"
#include "../../../engine/runtime/testing/stress_harness.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

namespace jobs = sl::runtime::jobs;
namespace rtt = sl::runtime::testing;

constexpr const char *test_name = "runtime_stress_deque_test";

void check_queue(const char *name, const jobs::queue_factory &factory, std::uint64_t base) {
  const rtt::cap_timer cap;
  const std::uint64_t seed = rtt::stress_seed(base);
  SL_STRESS_SEED(guard, test_name, seed);
  for (const std::uint32_t thieves : {1U, 3U, 7U, 15U}) {
    rtt::queue_contract_options options;
    options.thieves = thieves;
    options.items = static_cast<std::uint32_t>(rtt::scaled(1'000'000));
    options.seed = seed + thieves;
    options.capacity = 65536;
    std::vector<std::string> failures;
    REQUIRE(rtt::run_watched([&] { failures = rtt::run_queue_contract(factory, options); }));
    INFO(name << " with " << thieves << " thieves, " << options.items << " items");
    for (const std::string &f : failures) {
      MESSAGE(f);
    }
    CHECK(failures.empty());
  }
  SL_STRESS_CAP(cap);
}

} // namespace

TEST_CASE("RT-INV-STRESS-02: S2 locked_deque takes every pushed index exactly once with 1, 3, 7 "
          "and 15 thieves at stress scale") {
  check_queue("locked_deque", jobs::make_locked_queue, 0x5EED0990002ULL);
}

TEST_CASE("RT-INV-STRESS-02: S2 ws_deque takes every pushed index exactly once with 1, 3, 7 and "
          "15 thieves at stress scale") {
  check_queue("ws_deque", jobs::make_ws_queue, 0x5EED0990012ULL);
}
