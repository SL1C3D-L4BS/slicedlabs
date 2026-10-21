// RT-INV-STEAL-02 (SL-ARCH-003 ARCH3-R072, R073, ADR-0024): on one thread, owner pop is LIFO and
// steal is FIFO on both locked_deque and ws_deque; ws_deque keeps both orders across ring growth
// (4096 -> 16384 cells), frees its retired rings at quiesce, fills to exactly its capacity, and
// sizes itself as min(bit_ceil(capacity), 65536) cells starting from min(4096, that).

#include "sl/core/error.hpp"
#include "sl/runtime/work_queue.hpp"
#include "sl/runtime/ws_deque.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace jobs = sl::runtime::jobs;

struct named_factory {
  std::string name;
  jobs::queue_factory make;
};

std::vector<named_factory> factories() {
  return {{.name = "locked_deque", .make = jobs::make_locked_queue},
          {.name = "ws_deque", .make = jobs::make_ws_queue}};
}

} // namespace

TEST_CASE("RT-INV-STEAL-02: single-threaded owner pop is LIFO and steal is FIFO on both queues") {
  for (const named_factory &f : factories()) {
    INFO(f.name);
    auto made = f.make(4096);
    REQUIRE(made);
    jobs::work_queue &q = **made;
    CHECK_FALSE(q.pop());
    CHECK_FALSE(q.steal());
    constexpr std::uint32_t n = 1000;
    for (std::uint32_t i = 0; i < n; ++i) {
      REQUIRE(q.push(i));
    }
    // the steal end yields the oldest indices in push order
    for (std::uint32_t i = 0; i < 10; ++i) {
      CHECK(q.steal() == i);
    }
    // the owner end yields the newest in reverse push order
    for (std::uint32_t i = 0; i < 10; ++i) {
      CHECK(q.pop() == n - 1 - i);
    }
    // a push after pops is the next pop; the steal end is untouched
    REQUIRE(q.push(5000));
    CHECK(q.pop() == 5000U);
    CHECK(q.steal() == 10U);
    std::uint32_t expected = n - 11;
    std::uint32_t mismatches = 0;
    std::uint32_t taken = 0;
    while (const std::optional<std::uint32_t> got = q.pop()) {
      mismatches += *got == expected ? 0U : 1U;
      --expected;
      ++taken;
    }
    CHECK(mismatches == 0);
    CHECK(taken == n - 21);
    CHECK_FALSE(q.pop());
    CHECK_FALSE(q.steal());
  }
}

TEST_CASE("RT-INV-STEAL-02: ws_deque keeps LIFO and FIFO order across ring growth") {
  auto made = jobs::ws_deque::create(jobs::ws_deque_max_cells);
  REQUIRE(made);
  jobs::ws_deque &d = **made;
  CHECK(d.capacity() == jobs::ws_deque_max_cells);
  CHECK(d.cells() == jobs::ws_deque_initial_cells);
  constexpr std::uint32_t n = 10000;
  for (std::uint32_t i = 0; i < n; ++i) {
    REQUIRE(d.push(i));
  }
  CHECK(d.cells() == 16384);
  CHECK(d.retired_rings() == 2);
  std::uint32_t mismatches = 0;
  for (std::uint32_t i = 0; i < 100; ++i) {
    mismatches += d.steal() == i ? 0U : 1U;
    mismatches += d.pop() == n - 1 - i ? 0U : 1U;
  }
  CHECK(mismatches == 0);
  std::uint32_t expected = n - 101;
  std::uint32_t taken = 0;
  while (const std::optional<std::uint32_t> got = d.pop()) {
    mismatches += *got == expected ? 0U : 1U;
    --expected;
    ++taken;
  }
  CHECK(mismatches == 0);
  CHECK(taken == n - 200);
  d.quiesce();
  CHECK(d.retired_rings() == 0);
  CHECK(d.cells() == 16384);
  // fills to exactly capacity(); the next push fails and changes nothing
  for (std::uint32_t i = 0; i < jobs::ws_deque_max_cells; ++i) {
    if (!d.push(i)) {
      FAIL("push failed below capacity at " << i);
    }
  }
  CHECK_FALSE(d.push(0xFFFFFFFFU));
  CHECK(d.cells() == jobs::ws_deque_max_cells);
  CHECK(d.steal() == 0U);
  CHECK(d.pop() == jobs::ws_deque_max_cells - 1);
}

TEST_CASE("RT-INV-STEAL-02: ws_deque sizes from the limits table; capacity 0 is invalid") {
  CHECK(jobs::ws_deque::create(0).error().code == sl::errc::invalid_argument);
  CHECK(jobs::make_ws_queue(0).error().code == sl::errc::invalid_argument);
  const std::vector<std::pair<std::size_t, std::size_t>> sizes{
      {1, 1}, {100, 128}, {4096, 4096}, {5000, 8192}, {65536, 65536}, {1U << 20U, 65536}};
  for (const auto &[asked, cap] : sizes) {
    INFO("asked " << asked);
    auto d = jobs::ws_deque::create(asked);
    REQUIRE(d);
    CHECK((*d)->capacity() == cap);
    const std::size_t first =
        cap < jobs::ws_deque_initial_cells ? cap : jobs::ws_deque_initial_cells;
    CHECK((*d)->cells() == first);
  }
  auto q = jobs::make_ws_queue(16);
  REQUIRE(q);
  REQUIRE(*q);
  CHECK((*q)->capacity() == 16);
  auto one = jobs::ws_deque::create(1);
  REQUIRE(one);
  CHECK((*one)->push(7));
  CHECK_FALSE((*one)->push(8));
  CHECK((*one)->steal() == 7U);
  CHECK((*one)->push(9));
  CHECK((*one)->pop() == 9U);
}
