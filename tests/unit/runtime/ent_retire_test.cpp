// RT-INV-ENT-02 (SL-ARCH-003 ARCH3-R101, `failpoints` RFP10, ADR-0025): generation saturation
// retires the slot and it is never returned. RFP10 armed with param 1 fast-forwards the slot that
// create chooses to generation 0xFFFFFFFE; its destroy moves it to 0xFFFFFFFF, the slot is retired
// (never in the free FIFO) and no later create returns its index, on the fresh path, the FIFO
// path or the exhausted path. The RFP10 case needs the failpoints compiled in and skips with a
// message otherwise; the restore case reaches saturation through a restored state and runs in
// every build.

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/faults.hpp"
#include "sl/runtime/memory.hpp"

#include "../../../engine/runtime/testing/faults_arm.hpp"

#include <doctest/doctest.h>

#include <cstddef>
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

constexpr std::uint32_t saturated = ecs::entity_generation_retire - 1;

// Creates until entity_capacity and returns the handles; counts the ones on `banned`.
std::vector<ecs::entity> fill(ecs::entity_allocator &a, std::uint32_t banned, std::uint32_t &hits) {
  std::vector<ecs::entity> made;
  for (;;) {
    auto e = a.create();
    if (!e) {
      CHECK(e.error().code == sl::errc::entity_capacity);
      return made;
    }
    if (e->index == banned) {
      ++hits;
    }
    made.push_back(*e);
  }
}

} // namespace

TEST_CASE("RT-INV-ENT-02: RFP10 fast-forward retires the slot and it is never returned") {
  if (!rt::faults_compiled) {
    MESSAGE("skipped: the failpoints are compiled out (Release)");
    return;
  }
  rtt::disarm_all();
  constexpr std::uint32_t max = 1100;
  mem::tracking_resource r("ent.retire");
  ecs::entity_allocator a(max, r);
  ecs::entity doomed{};
  {
    const rtt::scoped_arm arm(rt::failpoint::rfp10, 1, false, 1);
    REQUIRE(arm.status().has_value());
    auto e = a.create();
    REQUIRE(e.has_value());
    doomed = *e;
  }
  CHECK(doomed == ent(0, saturated));
  CHECK(a.is_alive(doomed));
  REQUIRE(a.destroy(doomed).has_value());
  CHECK_FALSE(a.is_alive(doomed));
  CHECK_FALSE(a.is_alive(ent(0, ecs::entity_generation_retire)));
  CHECK(a.destroy(doomed).error().code == sl::errc::stale_entity);
  CHECK(a.retired_count() == 1);
  CHECK(a.free_count() == 0);
  CHECK(a.alive() == 0);
  const ecs::entity_allocator::state s = a.capture();
  CHECK(s.retired == std::vector<std::uint32_t>{0});
  CHECK(s.free_fifo.empty());
  CHECK(s.generations == std::vector<std::uint32_t>{ecs::entity_generation_retire});

  // Fresh path, then exhausted path, then FIFO path (1099 >= 1024 free): index 0 never returns.
  std::uint32_t hits = 0;
  std::vector<ecs::entity> all = fill(a, 0, hits);
  CHECK(all.size() == max - 1);
  for (std::uint32_t round = 0; round < 4; ++round) {
    for (const ecs::entity e : all) {
      REQUIRE(a.destroy(e).has_value());
    }
    CHECK(a.free_count() == max - 1);
    const std::vector<ecs::entity> again = fill(a, 0, hits);
    REQUIRE(again.size() == all.size());
    for (std::size_t i = 0; i < again.size(); ++i) {
      CHECK(again[i].index == all[i].index); // FIFO order of destruction
      CHECK(again[i].generation == all[i].generation + 1);
    }
    all = again;
  }
  CHECK(hits == 0);
  CHECK(a.retired_count() == 1);
  CHECK(a.alive() == max - 1);
}

TEST_CASE("RT-INV-ENT-02: RFP10 fast-forward applies to a reused slot too") {
  if (!rt::faults_compiled) {
    MESSAGE("skipped: the failpoints are compiled out (Release)");
    return;
  }
  rtt::disarm_all();
  constexpr std::uint32_t max = 4;
  mem::tracking_resource r("ent.retire.reuse");
  ecs::entity_allocator a(max, r);
  std::vector<ecs::entity> made;
  for (std::uint32_t i = 0; i < max; ++i) {
    auto e = a.create();
    REQUIRE(e.has_value());
    made.push_back(*e);
  }
  REQUIRE(a.destroy(made[2]).has_value());
  ecs::entity doomed{};
  {
    const rtt::scoped_arm arm(rt::failpoint::rfp10, 1, false, 1);
    REQUIRE(arm.status().has_value());
    auto e = a.create(); // max reached: the oldest free index, 2
    REQUIRE(e.has_value());
    doomed = *e;
  }
  CHECK(doomed == ent(2, saturated));
  REQUIRE(a.destroy(doomed).has_value());
  CHECK(a.retired_count() == 1);
  CHECK(a.create().error().code == sl::errc::entity_capacity);
}

TEST_CASE("RT-INV-ENT-02: a restored saturated slot retires on destroy in every build") {
  constexpr std::uint32_t max = 3000;
  mem::tracking_resource r("ent.retire.restore");
  ecs::entity_allocator a(max, r);
  ecs::entity_allocator::state s;
  s.next_index = 1;
  s.generations = {saturated};
  REQUIRE(a.restore(s).has_value());
  const ecs::entity doomed = ent(0, saturated);
  REQUIRE(a.is_alive(doomed));
  REQUIRE(a.destroy(doomed).has_value());
  CHECK(a.retired_count() == 1);
  CHECK(a.capture().retired == std::vector<std::uint32_t>{0});
  std::uint32_t hits = 0;
  std::vector<ecs::entity> all = fill(a, 0, hits);
  CHECK(all.size() == max - 1);
  for (std::uint32_t round = 0; round < 3; ++round) {
    for (const ecs::entity e : all) {
      REQUIRE(a.destroy(e).has_value());
    }
    all = fill(a, 0, hits);
    CHECK(all.size() == max - 1);
  }
  CHECK(hits == 0);
  // A capture round-trips the retired slot.
  ecs::entity_allocator b(max, r);
  REQUIRE(b.restore(a.capture()).has_value());
  CHECK(b.capture() == a.capture());
  CHECK(b.retired_count() == 1);
}

TEST_CASE("RT-INV-ENT-02: restore refuses an inconsistent retired list and changes nothing") {
  mem::tracking_resource r("ent.retire.bad");
  ecs::entity_allocator a(16, r);
  auto e = a.create();
  REQUIRE(e.has_value());
  const ecs::entity_allocator::state before = a.capture();

  ecs::entity_allocator::state listed_not_saturated;
  listed_not_saturated.next_index = 2;
  listed_not_saturated.generations = {1, 5};
  listed_not_saturated.retired = {1};
  CHECK(a.restore(listed_not_saturated).error().code == sl::errc::invalid_argument);

  ecs::entity_allocator::state saturated_not_listed;
  saturated_not_listed.next_index = 2;
  saturated_not_listed.generations = {1, ecs::entity_generation_retire};
  CHECK(a.restore(saturated_not_listed).error().code == sl::errc::invalid_argument);

  ecs::entity_allocator::state retired_and_free;
  retired_and_free.next_index = 1;
  retired_and_free.generations = {ecs::entity_generation_retire};
  retired_and_free.retired = {0};
  retired_and_free.free_fifo = {0};
  CHECK(a.restore(retired_and_free).error().code == sl::errc::invalid_argument);

  ecs::entity_allocator::state zero_generation;
  zero_generation.next_index = 1;
  zero_generation.generations = {0};
  CHECK(a.restore(zero_generation).error().code == sl::errc::invalid_argument);

  ecs::entity_allocator::state too_big;
  too_big.next_index = 17;
  too_big.generations.assign(17, 1);
  CHECK(a.restore(too_big).error().code == sl::errc::invalid_argument);

  ecs::entity_allocator::state wrong_size;
  wrong_size.next_index = 3;
  wrong_size.generations = {1};
  CHECK(a.restore(wrong_size).error().code == sl::errc::invalid_argument);

  ecs::entity_allocator::state twice_free;
  twice_free.next_index = 2;
  twice_free.generations = {2, 2};
  twice_free.free_fifo = {1, 1};
  CHECK(a.restore(twice_free).error().code == sl::errc::invalid_argument);

  CHECK(a.capture() == before);
  CHECK(a.is_alive(*e));
}
