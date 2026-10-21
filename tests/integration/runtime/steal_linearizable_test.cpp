// RT-INV-STEAL-01 (SL-ARCH-003 ARCH3-R073, ADR-0024): on both locked_deque and ws_deque, every
// index one owner pushes is popped or stolen exactly once with 1, 3, 7 and 15 concurrent thieves:
// the queue contract suite (sequential rules, then the owner-plus-thieves run) passes for each,
// with 1e5 indices per run scaled by SL_RUNTIME_STRESS_SCALE (0.1 under the sanitizers, floored
// at 1). The capacity asked is 65536, so the ws_deque ring grows from 4096 cells during the runs.
// A second owner is refused by the suite for both queues.

#include "sl/runtime/work_queue.hpp"

#include "../../../engine/runtime/testing/queue_contract.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

namespace jobs = sl::runtime::jobs;
namespace rtt = sl::runtime::testing;

std::uint32_t scaled(double base) {
  double scale = 1.0;
  if (const char *env = std::getenv("SL_RUNTIME_STRESS_SCALE"); env != nullptr && *env != '\0') {
    scale = std::strtod(env, nullptr);
  }
  return std::max<std::uint32_t>(1, static_cast<std::uint32_t>(base * scale));
}

void check_queue(const char *name, const jobs::queue_factory &factory) {
  for (const std::uint32_t thieves : {1U, 3U, 7U, 15U}) {
    rtt::queue_contract_options options;
    options.thieves = thieves;
    options.items = scaled(1e5);
    options.seed = 0x5EED0880001ULL + thieves;
    options.capacity = 65536;
    const std::vector<std::string> failures = rtt::run_queue_contract(factory, options);
    INFO(name << " with " << thieves << " thieves, " << options.items << " items");
    for (const std::string &f : failures) {
      MESSAGE(f);
    }
    CHECK(failures.empty());
  }
  rtt::queue_contract_options two;
  two.owners = 2;
  CHECK_FALSE(rtt::run_queue_contract(factory, two).empty());
}

} // namespace

TEST_CASE("RT-INV-STEAL-01: every pushed index is taken exactly once on locked_deque with 1, 3, "
          "7 and 15 thieves") {
  check_queue("locked_deque", jobs::make_locked_queue);
}

TEST_CASE("RT-INV-STEAL-01: every pushed index is taken exactly once on ws_deque with 1, 3, 7 "
          "and 15 thieves") {
  check_queue("ws_deque", jobs::make_ws_queue);
}
