// RT-INV-STEAL-03 (SL-ARCH-003 ARCH3-R072, ADR-0024): a ws_deque that starts at 4096 cells loses
// nothing while it grows under a burst of 1e5 pushes (scaled by SL_RUNTIME_STRESS_SCALE, floored
// at 3 x 4096 so the ring always grows) with concurrent thieves. Throttled run: 3 thieves steal
// only while they have taken less than half of what was pushed, so the deque holds more than 4096
// indices and must grow while they steal. Free run: 7 unthrottled thieves. In both every index is
// taken exactly once, the ring ends between 8192 and 65536 cells, a push at the maximum fails
// (the owner pops and retries) and quiesce() frees every retired ring once the thieves stopped.

#include "sl/runtime/work_queue.hpp"
#include "sl/runtime/ws_deque.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <thread>
#include <vector>

namespace {

namespace jobs = sl::runtime::jobs;

std::uint32_t burst() {
  double scale = 1.0;
  if (const char *env = std::getenv("SL_RUNTIME_STRESS_SCALE"); env != nullptr && *env != '\0') {
    scale = std::strtod(env, nullptr);
  }
  const auto n = static_cast<std::uint32_t>(1e5 * scale);
  return std::max<std::uint32_t>(3 * jobs::ws_deque_initial_cells, n);
}

void run(std::uint32_t thief_count, bool throttled) {
  auto made = jobs::ws_deque::create(jobs::ws_deque_max_cells);
  REQUIRE(made);
  jobs::ws_deque &d = **made;
  REQUIRE(d.cells() == jobs::ws_deque_initial_cells);
  const std::uint32_t items = burst();
  std::vector<std::atomic<std::uint32_t>> taken(items);
  std::atomic<std::uint32_t> pushed{0};
  std::atomic<std::uint32_t> stolen{0};
  std::atomic<std::uint32_t> out_of_range{0};
  std::atomic<bool> done{false};
  const auto record = [&](std::uint32_t index) {
    if (index >= items) {
      out_of_range.fetch_add(1);
      return;
    }
    taken[index].fetch_add(1);
  };
  std::vector<std::jthread> thieves;
  thieves.reserve(thief_count);
  for (std::uint32_t t = 0; t < thief_count; ++t) {
    thieves.emplace_back([&] {
      while (!done.load()) {
        if (throttled && 2U * stolen.load() >= pushed.load()) {
          std::this_thread::yield();
          continue;
        }
        if (const std::optional<std::uint32_t> index = d.steal()) {
          record(*index);
          stolen.fetch_add(1);
        }
      }
    });
  }
  std::size_t max_cells = d.cells();
  std::uint32_t full = 0;
  for (std::uint32_t i = 0; i < items; ++i) {
    while (!d.push(i)) {
      ++full;
      if (const std::optional<std::uint32_t> index = d.pop()) {
        record(*index);
      }
    }
    pushed.store(i + 1);
    max_cells = std::max(max_cells, d.cells());
  }
  done.store(true);
  thieves.clear(); // joins
  while (const std::optional<std::uint32_t> index = d.pop()) {
    record(*index);
  }
  while (const std::optional<std::uint32_t> index = d.steal()) {
    record(*index);
  }
  std::uint32_t lost = 0;
  std::uint32_t twice = 0;
  for (std::uint32_t i = 0; i < items; ++i) {
    const std::uint32_t c = taken[i].load();
    lost += c == 0 ? 1U : 0U;
    twice += c > 1 ? 1U : 0U;
  }
  INFO(thief_count << " thieves, throttled " << throttled << ", " << items << " pushes, ring "
                   << max_cells << " cells, " << full << " full pushes, " << stolen.load()
                   << " stolen");
  CHECK(lost == 0);
  CHECK(twice == 0);
  CHECK(out_of_range.load() == 0);
  CHECK(max_cells <= jobs::ws_deque_max_cells);
  CHECK(d.cells() == max_cells);
  if (throttled) {
    CHECK(max_cells >= 2 * jobs::ws_deque_initial_cells);
  }
  const bool grew = max_cells > jobs::ws_deque_initial_cells;
  CHECK((d.retired_rings() > 0) == grew);
  d.quiesce();
  CHECK(d.retired_rings() == 0);
  CHECK_FALSE(d.pop());
  CHECK_FALSE(d.steal());
}

} // namespace

TEST_CASE("RT-INV-STEAL-03: ws_deque growth under a push burst with throttled thieves loses "
          "nothing") {
  run(3, true);
}

TEST_CASE("RT-INV-STEAL-03: ws_deque under a push burst with free thieves loses nothing") {
  run(7, false);
}
