// RT-INV-MEM-04 (SL-ARCH-003 ARCH3-R042, ADR-0023, ADR-0009): try_allocate under RFP0 is
// out_of_memory with counters unchanged and never throws; do_allocate (the pmr path) forwards to
// the upstream and, without one, invokes the captured assert handler with the tag report. The
// RFP0 cases need the failpoints compiled in (Debug and sanitizer builds) and skip with a message
// otherwise; the no-upstream case exhausts the backing through testing::failing_resource, so it
// runs in every build.

#include "sl/core/assert.hpp"
#include "sl/core/error.hpp"
#include "sl/runtime/faults.hpp"
#include "sl/runtime/memory.hpp"

#include "../../../engine/runtime/testing/failing_resource.hpp"
#include "../../../engine/runtime/testing/faults_arm.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <memory_resource>
#include <string>
#include <utility>

namespace {

namespace mem = sl::runtime::mem;
namespace rt = sl::runtime;
namespace rtt = sl::runtime::testing;

bool same(const mem::tag_counters &a, const mem::tag_counters &b) {
  return a.live_bytes == b.live_bytes && a.live_blocks == b.live_blocks &&
         a.peak_bytes == b.peak_bytes && a.allocs == b.allocs && a.frees == b.frees &&
         a.failures == b.failures;
}

} // namespace

TEST_CASE("RT-INV-MEM-04: try_allocate under RFP0 is out_of_memory with counters unchanged") {
  mem::tracking_resource r("rfp0");
  static_assert(noexcept(r.try_allocate(64, 16, mem::memory_tag{})));
  static_assert(noexcept(r.release(nullptr, 64, 16)));
  static_assert(noexcept(r.forward_allocate(64, 16)));
  if (!rt::faults_compiled) {
    MESSAGE("skipped: the failpoints are compiled out (Release)");
    return;
  }
  rtt::disarm_all();
  auto held = r.try_allocate(40, 8, r.default_tag());
  REQUIRE(held.has_value());
  const mem::tag_counters tag_before = r.counters(r.default_tag());
  const mem::tag_counters total_before = r.totals();
  {
    const rtt::scoped_arm arm(rt::failpoint::rfp0, 1);
    REQUIRE(arm.status().has_value());
    auto got = r.try_allocate(64, 16, r.default_tag());
    REQUIRE_FALSE(got.has_value());
    CHECK(got.error().code == sl::errc::out_of_memory);
    CHECK(same(r.counters(r.default_tag()), tag_before));
    CHECK(same(r.totals(), total_before));
    // RFP0 fires once: the next call is served
    auto next = r.try_allocate(64, 16, r.default_tag());
    REQUIRE(next.has_value());
    r.release(*next, 64, 16);
  }
  r.release(*held, 40, 8);
  CHECK(r.totals().live_bytes == 0);
}

TEST_CASE("RT-INV-MEM-04: do_allocate forwards to the upstream when try_allocate fails") {
  if (!rt::faults_compiled) {
    MESSAGE("skipped: the failpoints are compiled out (Release)");
    return;
  }
  rtt::disarm_all();
  mem::tracking_resource upstream("upstream");
  mem::tracking_resource r("primary");
  r.set_upstream(&upstream);
  CHECK(r.upstream() == &upstream);
  std::pmr::memory_resource &pmr = r;
  void *p = nullptr;
  {
    const rtt::scoped_arm arm(rt::failpoint::rfp0, 1);
    REQUIRE(arm.status().has_value());
    p = pmr.allocate(128, 32);
  }
  REQUIRE(p != nullptr);
  CHECK(r.totals().allocs == 0);
  CHECK(upstream.totals().allocs == 1);
  CHECK(upstream.totals().live_bytes == 128);
  pmr.deallocate(p, 128, 32);
  CHECK(upstream.totals().live_bytes == 0);
  CHECK(upstream.totals().frees == 1);
  CHECK(r.totals().frees == 0);

  // without a failure the pmr path is served by the resource itself
  void *q = pmr.allocate(16, 16);
  CHECK(r.totals().live_bytes == 16);
  pmr.deallocate(q, 16, 16);
  CHECK(r.totals().live_bytes == 0);
  CHECK(upstream.totals().allocs == 1);
}

TEST_CASE("RT-INV-MEM-04: without an upstream the pmr path invokes the assert handler with the "
          "tag report") {
  mem::tag_registry registry;
  const auto events = registry.add("events");
  REQUIRE(events.has_value());
  rtt::failing_resource exhausted(*std::pmr::new_delete_resource(), 0, *events);
  mem::tracking_resource r("frame", *events, &registry, &exhausted);

  int fired = 0;
  std::string report;
  {
    const sl::scoped_assert_handler capture([&fired, &report](const sl::assert_report &a) {
      ++fired;
      report = std::string(a.message);
    });
    CHECK(r.forward_allocate(64, 16) == nullptr);
  }
  CHECK(fired == 1);
  CHECK(report.starts_with("frame: events exhausted"));
  CHECK(report.find("no upstream") != std::string::npos);
  CHECK(r.counters(*events).failures == 1);
  CHECK(r.counters(*events).allocs == 0);
  CHECK(exhausted.failed() == 1);
}

TEST_CASE("RT-INV-MEM-04: failing_resource fails the Nth try_allocate or every one of a tag") {
  mem::tag_registry registry;
  const auto chunks = registry.add("ecs.chunks");
  const auto events = registry.add("events");
  REQUIRE(chunks.has_value());
  REQUIRE(events.has_value());
  {
    rtt::failing_resource f(*std::pmr::new_delete_resource(), 3);
    for (int call = 1; call <= 5; ++call) {
      auto got = f.try_allocate(32, 8, *chunks);
      if (call == 3) {
        REQUIRE_FALSE(got.has_value());
        CHECK(got.error().code == sl::errc::out_of_memory);
      } else {
        REQUIRE(got.has_value());
        f.release(*got, 32, 8);
      }
    }
    CHECK(f.calls() == 5);
    CHECK(f.failed() == 1);
  }
  {
    rtt::failing_resource f(*std::pmr::new_delete_resource(), 1, *events);
    for (int call = 0; call < 4; ++call) {
      auto bad = f.try_allocate(32, 8, *events);
      REQUIRE_FALSE(bad.has_value());
      CHECK(bad.error().code == sl::errc::out_of_memory);
      auto good = f.try_allocate(32, 8, *chunks);
      REQUIRE(good.has_value());
      f.release(*good, 32, 8);
    }
    CHECK(f.failed() == 4);
  }
  {
    // the pmr path of a failing_resource forwards the failed call to its upstream
    mem::tracking_resource upstream("upstream");
    rtt::failing_resource f(*std::pmr::new_delete_resource(), 1);
    f.set_upstream(&upstream);
    std::pmr::memory_resource &pmr = f;
    void *p = pmr.allocate(48, 16);
    REQUIRE(p != nullptr);
    CHECK(upstream.totals().live_bytes == 48);
    pmr.deallocate(p, 48, 16);
    CHECK(upstream.totals().live_bytes == 0);
  }
}
