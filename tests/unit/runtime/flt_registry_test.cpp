// RT-INV-FLT-01 (SL-ARCH-003 ARCH3-R020, ADR-0022): an armed failpoint trips exactly at the Nth
// visit, counts every visit from 8 threads, fires once unless sticky, and an unarmed point returns
// proceed. The registry is process-global, so every case disarms all points first and measures
// visit counts as deltas. With the failpoints compiled out (Release) arm() is unsupported and
// visit() always proceeds; that case runs instead of the others.

#include "sl/core/error.hpp"
#include "sl/runtime/faults.hpp"

#include "../../../engine/runtime/testing/faults_arm.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

using sl::runtime::failpoint;
namespace rt = sl::runtime;
namespace rtt = sl::runtime::testing;

constexpr std::size_t thread_count = 8;
constexpr std::uint64_t visits_per_thread = 20000;

// Visits `point` from 8 threads, visits_per_thread each; returns how many visits fired.
std::uint64_t visit_from_threads(failpoint point) {
  std::atomic<std::uint64_t> fired{0};
  {
    std::vector<std::jthread> threads;
    threads.reserve(thread_count);
    for (std::size_t t = 0; t < thread_count; ++t) {
      threads.emplace_back([point, &fired] {
        for (std::uint64_t i = 0; i < visits_per_thread; ++i) {
          if (rt::visit(point).fire) {
            fired.fetch_add(1);
          }
        }
      });
    }
  }
  return fired.load();
}

} // namespace

TEST_CASE("RT-INV-FLT-01: an unarmed failpoint proceeds and counts its visits") {
  if (!rt::faults_compiled) {
    return;
  }
  rtt::disarm_all();
  for (std::size_t i = 0; i < rt::failpoint_count; ++i) {
    const auto point = static_cast<failpoint>(i);
    const std::uint64_t before = rt::visits(point);
    for (int k = 0; k < 5; ++k) {
      const rt::fault_action a = rt::visit(point);
      CHECK_FALSE(a.fire);
      CHECK(a.param == 0);
    }
    CHECK(rt::visits(point) == before + 5);
  }
}

TEST_CASE("RT-INV-FLT-01: an armed failpoint trips exactly at the Nth visit, once") {
  if (!rt::faults_compiled) {
    return;
  }
  rtt::disarm_all();
  REQUIRE(rtt::arm(failpoint::rfp2, 3, false, 7));
  for (std::uint64_t n = 1; n <= 10; ++n) {
    INFO("visit " << n);
    const rt::fault_action a = rt::visit(failpoint::rfp2);
    CHECK(a.fire == (n == 3));
    CHECK(a.param == (n == 3 ? 7U : 0U));
  }
  // The other points stay unarmed.
  CHECK_FALSE(rt::visit(failpoint::rfp3).fire);
  // Arming counts from the arm() call, not from process start.
  REQUIRE(rtt::arm(failpoint::rfp2, 1));
  CHECK(rt::visit(failpoint::rfp2).fire);
  CHECK_FALSE(rt::visit(failpoint::rfp2).fire);
  rtt::disarm_all();
}

TEST_CASE("RT-INV-FLT-01: a sticky failpoint fires at the Nth visit and every later one") {
  if (!rt::faults_compiled) {
    return;
  }
  rtt::disarm_all();
  REQUIRE(rtt::arm(failpoint::rfp6, 4, true, 250));
  for (std::uint64_t n = 1; n <= 12; ++n) {
    INFO("visit " << n);
    const rt::fault_action a = rt::visit(failpoint::rfp6);
    CHECK(a.fire == (n >= 4));
    CHECK(a.param == (n >= 4 ? 250U : 0U));
  }
  rtt::disarm(failpoint::rfp6);
  CHECK_FALSE(rt::visit(failpoint::rfp6).fire);
}

TEST_CASE("RT-INV-FLT-01: visits from 8 threads are all counted; the armed visit fires once") {
  if (!rt::faults_compiled) {
    return;
  }
  rtt::disarm_all();
  constexpr std::uint64_t total = thread_count * visits_per_thread;

  const std::uint64_t before_unarmed = rt::visits(failpoint::rfp4);
  CHECK(visit_from_threads(failpoint::rfp4) == 0);
  CHECK(rt::visits(failpoint::rfp4) - before_unarmed == total);

  REQUIRE(rtt::arm(failpoint::rfp4, 12345));
  const std::uint64_t before_once = rt::visits(failpoint::rfp4);
  CHECK(visit_from_threads(failpoint::rfp4) == 1);
  CHECK(rt::visits(failpoint::rfp4) - before_once == total);

  constexpr std::uint64_t at = 1000;
  REQUIRE(rtt::arm(failpoint::rfp4, at, true));
  const std::uint64_t before_sticky = rt::visits(failpoint::rfp4);
  CHECK(visit_from_threads(failpoint::rfp4) == total - (at - 1));
  CHECK(rt::visits(failpoint::rfp4) - before_sticky == total);
  rtt::disarm_all();
}

TEST_CASE("RT-INV-FLT-01: arm refuses bad arguments and scoped_arm disarms at scope exit") {
  if (!rt::faults_compiled) {
    return;
  }
  rtt::disarm_all();
  const auto zero = rtt::arm(failpoint::rfp0, 0);
  REQUIRE_FALSE(zero.has_value());
  CHECK(zero.error().code == sl::errc::invalid_argument);
  const auto unknown = rtt::arm(static_cast<failpoint>(rt::failpoint_count), 1);
  REQUIRE_FALSE(unknown.has_value());
  CHECK(unknown.error().code == sl::errc::invalid_argument);
  CHECK_FALSE(rt::visit(static_cast<failpoint>(rt::failpoint_count)).fire);
  CHECK(rt::visits(static_cast<failpoint>(rt::failpoint_count)) == 0);
  {
    const rtt::scoped_arm armed(failpoint::rfp10, 2, true, 9);
    REQUIRE(armed.status());
    CHECK_FALSE(rt::visit(failpoint::rfp10).fire);
    const rt::fault_action a = rt::visit(failpoint::rfp10);
    CHECK(a.fire);
    CHECK(a.param == 9);
  }
  CHECK_FALSE(rt::visit(failpoint::rfp10).fire);
  {
    const rtt::scoped_arm refused(failpoint::rfp9, 0);
    CHECK_FALSE(refused.status().has_value());
  }
}

TEST_CASE("RT-INV-FLT-01: compiled out, arm is unsupported and every visit proceeds") {
  if (rt::faults_compiled) {
    return;
  }
  const auto r = rtt::arm(failpoint::rfp1, 1);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == sl::errc::unsupported);
  for (std::size_t i = 0; i < rt::failpoint_count; ++i) {
    const auto point = static_cast<failpoint>(i);
    CHECK_FALSE(rt::visit(point).fire);
    CHECK(rt::visits(point) == 0);
  }
}
