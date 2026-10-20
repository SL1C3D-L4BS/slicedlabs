// RT-INV-ENT-04 (SL-ARCH-003 ARCH3-R104, `failpoints` RFP10, ADR-0025): capacity exhaustion is
// entity_capacity and changes nothing; pages never move. Natural exhaustion at max_entities,
// RFP10 param 0 (needs the failpoints, skips otherwise) and a failed page allocation
// (out_of_memory through testing::failing_resource) all leave the captured state, the counters
// and the page addresses as they were. Pages of 4096 records keep their address while the table
// grows, shrinks, reuses and is restored.

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/faults.hpp"
#include "sl/runtime/memory.hpp"

#include "../../../engine/runtime/testing/failing_resource.hpp"
#include "../../../engine/runtime/testing/faults_arm.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

namespace {

namespace ecs = sl::runtime::ecs;
namespace mem = sl::runtime::mem;
namespace rt = sl::runtime;
namespace rtt = sl::runtime::testing;

constexpr ecs::entity ent(std::uint32_t index, std::uint32_t generation) noexcept {
  return ecs::entity{.index = index, .generation = generation};
}

constexpr std::uint32_t page = ecs::entity_page_records;

struct counts {
  std::uint32_t alive = 0;
  std::uint32_t free_count = 0;
  std::uint32_t retired = 0;
  std::uint32_t next = 0;
  std::uint32_t pages = 0;
  friend bool operator==(const counts &, const counts &) = default;
};

counts counts_of(const ecs::entity_allocator &a) {
  return {.alive = a.alive(),
          .free_count = a.free_count(),
          .retired = a.retired_count(),
          .next = a.next_index(),
          .pages = a.page_count()};
}

std::vector<const std::uint32_t *> pages_of(const ecs::entity_allocator &a) {
  std::vector<const std::uint32_t *> out;
  out.reserve(a.page_count());
  for (std::uint32_t p = 0; p < a.page_count(); ++p) {
    out.push_back(a.page_address(p));
  }
  return out;
}

} // namespace

TEST_CASE("RT-INV-ENT-04: exhaustion at max_entities is entity_capacity and changes nothing") {
  mem::tracking_resource r("ent.cap");
  ecs::entity_allocator a(10, r);
  std::vector<ecs::entity> made;
  for (std::uint32_t i = 0; i < 10; ++i) {
    auto e = a.create();
    REQUIRE(e.has_value());
    made.push_back(*e);
  }
  const ecs::entity_allocator::state before = a.capture();
  const counts c = counts_of(a);
  const mem::tag_counters mem_before = r.totals();
  for (int i = 0; i < 3; ++i) {
    auto e = a.create();
    REQUIRE_FALSE(e.has_value());
    CHECK(e.error().code == sl::errc::entity_capacity);
  }
  CHECK(a.capture() == before);
  CHECK(counts_of(a) == c);
  CHECK(r.totals().live_bytes == mem_before.live_bytes);
  for (const ecs::entity e : made) {
    CHECK(a.is_alive(e));
  }

  mem::tracking_resource r0("ent.cap.zero");
  ecs::entity_allocator none(0, r0);
  CHECK(none.create().error().code == sl::errc::entity_capacity);
  CHECK(none.page_count() == 0);
  CHECK(none.page_address(0) == nullptr);
  CHECK(none.capture() == ecs::entity_allocator::state{});
}

TEST_CASE("RT-INV-ENT-04: RFP10 param 0 forces entity_capacity and changes nothing") {
  if (!rt::faults_compiled) {
    MESSAGE("skipped: the failpoints are compiled out (Release)");
    return;
  }
  rtt::disarm_all();
  mem::tracking_resource r("ent.cap.rfp10");
  ecs::entity_allocator a(page * 2, r);
  for (std::uint32_t i = 0; i < page; ++i) {
    REQUIRE(a.create().has_value());
  }
  const ecs::entity_allocator::state before = a.capture();
  const counts c = counts_of(a);
  const std::vector<const std::uint32_t *> pages = pages_of(a);
  {
    // The next create would allocate the second page: forced capacity allocates nothing.
    const rtt::scoped_arm arm(rt::failpoint::rfp10, 1);
    REQUIRE(arm.status().has_value());
    auto e = a.create();
    REQUIRE_FALSE(e.has_value());
    CHECK(e.error().code == sl::errc::entity_capacity);
  }
  CHECK(a.capture() == before);
  CHECK(counts_of(a) == c);
  CHECK(pages_of(a) == pages);
  {
    // Any param other than 1 forces capacity too.
    const rtt::scoped_arm arm(rt::failpoint::rfp10, 1, false, 7);
    REQUIRE(arm.status().has_value());
    CHECK(a.create().error().code == sl::errc::entity_capacity);
  }
  CHECK(a.capture() == before);
  auto e = a.create();
  REQUIRE(e.has_value());
  CHECK(*e == ent(page, 1));
  CHECK(a.page_count() == 2);
}

TEST_CASE("RT-INV-ENT-04: a failed page allocation is out_of_memory and changes nothing") {
  mem::tracking_resource upstream("ent.cap.upstream");
  // Call 1 is the page table (constructor), call 2 the first page, call 3 the second page.
  rtt::failing_resource failing(upstream, 3);
  ecs::entity_allocator a(page * 2, failing);
  REQUIRE(failing.calls() == 1);
  for (std::uint32_t i = 0; i < page; ++i) {
    REQUIRE(a.create().has_value());
  }
  const ecs::entity_allocator::state before = a.capture();
  const counts c = counts_of(a);
  const std::vector<const std::uint32_t *> pages = pages_of(a);
  auto e = a.create();
  REQUIRE_FALSE(e.has_value());
  CHECK(e.error().code == sl::errc::out_of_memory);
  CHECK(failing.failed() == 1);
  CHECK(a.capture() == before);
  CHECK(counts_of(a) == c);
  CHECK(pages_of(a) == pages);
  auto retry = a.create();
  REQUIRE(retry.has_value());
  CHECK(*retry == ent(page, 1));
  CHECK(pages_of(a).front() == pages.front());
}

TEST_CASE("RT-INV-ENT-04: a restore whose page allocation fails changes nothing") {
  mem::tracking_resource upstream("ent.cap.restore");
  rtt::failing_resource failing(upstream, 3); // table, page 0, then page 1 fails
  ecs::entity_allocator a(page * 3, failing);
  ecs::entity_allocator::state s;
  s.next_index = page + 5;
  s.generations.assign(page + 5, 1);
  const mem::tag_counters held = upstream.totals();
  auto restored = a.restore(s);
  REQUIRE_FALSE(restored.has_value());
  CHECK(restored.error().code == sl::errc::out_of_memory);
  CHECK(a.capture() == ecs::entity_allocator::state{});
  CHECK(a.page_count() == 0);
  CHECK(upstream.totals().live_blocks == held.live_blocks); // the page of the attempt is freed
  REQUIRE(a.restore(s).has_value());
  CHECK(a.alive() == page + 5);
  CHECK(a.page_count() == 2);
}

TEST_CASE("RT-INV-ENT-04: pages of 4096 records never move") {
  constexpr std::uint32_t max = (page * 3) + 100;
  mem::tracking_resource r("ent.pages");
  ecs::entity_allocator a(max, r);
  CHECK(a.page_count() == 0);
  std::vector<ecs::entity> made;
  auto first = a.create();
  REQUIRE(first.has_value());
  made.push_back(*first);
  REQUIRE(a.page_count() == 1);
  const std::uint32_t *page0 = a.page_address(0);
  REQUIRE(page0 != nullptr);
  for (std::uint32_t i = 1; i < page; ++i) {
    auto e = a.create();
    REQUIRE(e.has_value());
    made.push_back(*e);
  }
  CHECK(a.page_count() == 1); // a page holds exactly 4096 records
  auto spill = a.create();
  REQUIRE(spill.has_value());
  made.push_back(*spill);
  CHECK(a.page_count() == 2);
  CHECK(a.page_address(0) == page0);
  while (made.size() < max) {
    auto e = a.create();
    REQUIRE(e.has_value());
    made.push_back(*e);
  }
  CHECK(a.page_count() == 4);
  CHECK(a.page_address(4) == nullptr);
  const std::vector<const std::uint32_t *> pages = pages_of(a);
  CHECK(pages.front() == page0);

  for (const ecs::entity e : made) {
    REQUIRE(a.destroy(e).has_value());
  }
  CHECK(pages_of(a) == pages);
  for (std::uint32_t i = 0; i < max; ++i) {
    REQUIRE(a.create().has_value());
  }
  CHECK(pages_of(a) == pages);
  // The page holds the generations: index 5 was destroyed once.
  CHECK(pages[0][5] == 2);
  const ecs::entity_allocator::state s = a.capture();
  REQUIRE(a.restore(ecs::entity_allocator::state{}).has_value());
  CHECK(pages_of(a) == pages); // restore reuses the pages in place
  CHECK(a.alive() == 0);
  CHECK(pages[0][5] == ecs::entity_generation_first);
  REQUIRE(a.restore(s).has_value());
  CHECK(pages_of(a) == pages);
  CHECK(a.capture() == s);
  CHECK(pages[0][5] == 2);
}
