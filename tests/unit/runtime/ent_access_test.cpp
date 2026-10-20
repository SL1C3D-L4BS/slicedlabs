// RT-INV-ENT-06 (SL-ARCH-003 ARCH3-R103, `assertions`, ADR-0025): checked access through a stale
// handle is stale_entity and unchecked access trips the captured debug assertion. check() is the
// checked accessor and assume_alive() the unchecked one (SL_DEBUG_ASSERT): a test assert handler
// captures the report and returns. Debug assertions are compiled out with NDEBUG (Release); the
// assertion part then skips with a message and the checked part still runs. The handle layout
// (64 bits, null all-zero, bits() round trip) is checked here too.

#include "sl/core/assert.hpp"
#include "sl/core/error.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/memory.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <type_traits>

namespace {

namespace ecs = sl::runtime::ecs;
namespace mem = sl::runtime::mem;

constexpr ecs::entity ent(std::uint32_t index, std::uint32_t generation) noexcept {
  return ecs::entity{.index = index, .generation = generation};
}

#ifdef NDEBUG
constexpr bool debug_asserts = false;
#else
constexpr bool debug_asserts = true;
#endif

struct captured {
  int count = 0;
  std::string message;
};

} // namespace

TEST_CASE("RT-INV-ENT-06: checked access through a stale handle is stale_entity") {
  mem::tracking_resource r("ent.access");
  ecs::entity_allocator a(1, r);
  static_assert(noexcept(a.check(ecs::null_entity)));
  static_assert(noexcept(a.assume_alive(ecs::null_entity)));
  static_assert(noexcept(a.is_alive(ecs::null_entity)));
  static_assert(noexcept(a.create()));
  static_assert(noexcept(a.destroy(ecs::null_entity)));
  auto e = a.create();
  REQUIRE(e.has_value());
  CHECK(a.check(*e).has_value());
  REQUIRE(a.destroy(*e).has_value());
  auto stale = a.check(*e);
  REQUIRE_FALSE(stale.has_value());
  CHECK(stale.error().code == sl::errc::stale_entity);
  CHECK(a.check(ecs::null_entity).error().code == sl::errc::stale_entity);
  CHECK(a.check(ent(5, 1)).error().code == sl::errc::stale_entity); // never issued
  CHECK(a.check(ent(e->index, e->generation + 1)).error().code ==
        sl::errc::stale_entity); // the next generation, not issued yet
  auto again = a.create();
  REQUIRE(again.has_value());
  CHECK(again->index == e->index); // max reached: the freed index comes back
  CHECK(again->generation == e->generation + 1);
  CHECK(a.check(*again).has_value());
  CHECK(a.check(*e).error().code == sl::errc::stale_entity);
}

TEST_CASE("RT-INV-ENT-06: unchecked access through a stale handle trips the debug assertion") {
  mem::tracking_resource r("ent.assume");
  ecs::entity_allocator a(8, r);
  auto e = a.create();
  REQUIRE(e.has_value());
  captured got;
  {
    const sl::scoped_assert_handler handler([&](const sl::assert_report &report) {
      ++got.count;
      got.message = std::string(report.message);
    });
    a.assume_alive(*e);
    CHECK(got.count == 0); // a live handle passes
    REQUIRE(a.destroy(*e).has_value());
    a.assume_alive(*e);
    a.assume_alive(ecs::null_entity);
  }
  if (!debug_asserts) {
    MESSAGE("skipped: SL_DEBUG_ASSERT is compiled out (NDEBUG)");
    CHECK(got.count == 0);
    return;
  }
  CHECK(got.count == 2);
  CHECK(got.message.find("stale entity") != std::string::npos);
}

TEST_CASE("RT-INV-ENT-06: the handle is 64 bits, null is all-zero and bits() round-trips") {
  static_assert(sizeof(ecs::entity) == 8);
  static_assert(std::is_trivially_copyable_v<ecs::entity>);
  static_assert(ecs::null_entity.is_null());
  static_assert(ecs::null_entity.bits() == 0);
  static_assert(ecs::entity_generation_first == 1);
  static_assert(ecs::entity_generation_retire == 0xFFFFFFFFU);
  static_assert(ecs::entity_reuse_fifo_threshold == 1024);
  static_assert(ecs::entity_page_records == 4096);
  constexpr ecs::entity e = ent(0x01020304U, 0x0A0B0C0DU);
  static_assert(e.bits() == 0x010203040A0B0C0DULL);
  static_assert(ecs::entity::from_bits(e.bits()) == e);
  static_assert(!ent(0, 1).is_null());
  static_assert(!ent(1, 0).is_null());
  // Order is (index, generation), the same as the order of bits().
  constexpr ecs::entity lo = ent(1, 9);
  constexpr ecs::entity hi = ent(2, 1);
  static_assert(lo < hi && lo.bits() < hi.bits());
  static_assert(ent(1, 1) < lo);
  CHECK(ecs::entity::from_bits(0xFFFFFFFFFFFFFFFFULL) ==
        ent(0xFFFFFFFFU, ecs::entity_generation_retire));
}
