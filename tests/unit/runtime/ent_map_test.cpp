// RT-INV-ENT-05 (SL-ARCH-003 ARCH3-R105, ADR-0025): entity_map is one-to-one, refuses a duplicate
// key or entity with already_exists and iterates in ascending key order. Keys are fabric
// record_ids (derived, so their byte order differs from the insertion order) as in the link, and
// std::uint64_t; every refused insert leaves the map as it was, erase frees both directions, and
// the nodes come from the given resource.

#include "sl/core/error.hpp"
#include "sl/fabric/id.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/memory.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace ecs = sl::runtime::ecs;
namespace mem = sl::runtime::mem;
namespace fabric = sl::fabric;

constexpr ecs::entity ent(std::uint32_t index, std::uint32_t generation) noexcept {
  return ecs::entity{.index = index, .generation = generation};
}

fabric::record_id key_n(std::uint32_t n) {
  const fabric::record_id parent = fabric::well_known(fabric::id_class::project, 1);
  return fabric::derive(fabric::id_class::entity, parent, "lantern-" + std::to_string(n));
}

template <class Key>
std::vector<std::pair<Key, ecs::entity>> pairs_of(const ecs::entity_map<Key> &m) {
  std::vector<std::pair<Key, ecs::entity>> out;
  m.for_each([&](const Key &k, ecs::entity e) { out.emplace_back(k, e); });
  return out;
}

} // namespace

TEST_CASE("RT-INV-ENT-05: record_id to entity is one-to-one and iterates in ascending key order") {
  constexpr std::uint32_t count = 64;
  mem::tracking_resource r("ent.map");
  ecs::entity_map<fabric::record_id> m(r);
  std::vector<fabric::record_id> keys;
  for (std::uint32_t i = 0; i < count; ++i) {
    keys.push_back(key_n(i));
    REQUIRE(m.insert(keys.back(), ent(i, 1)).has_value());
  }
  CHECK(m.size() == count);
  CHECK(r.totals().live_bytes > 0);
  for (std::uint32_t i = 0; i < count; ++i) {
    auto e = m.find(keys[i]);
    REQUIRE(e.has_value());
    CHECK(*e == ent(i, 1));
    auto k = m.key_of(ent(i, 1));
    REQUIRE(k.has_value());
    CHECK(*k == keys[i]);
  }
  const auto pairs = pairs_of(m);
  REQUIRE(pairs.size() == count);
  std::vector<fabric::record_id> sorted = keys;
  std::ranges::sort(sorted);
  CHECK_FALSE(std::ranges::equal(sorted, keys)); // insertion order is not key order
  for (std::size_t i = 0; i < pairs.size(); ++i) {
    CHECK(pairs[i].first == sorted[i]);
    CHECK(m.find(pairs[i].first).value() == pairs[i].second);
  }
  for (std::size_t i = 1; i < pairs.size(); ++i) {
    CHECK(pairs[i - 1].first < pairs[i].first);
  }
}

TEST_CASE("RT-INV-ENT-05: a duplicate key or entity is already_exists and changes nothing") {
  mem::tracking_resource r("ent.map.dup");
  ecs::entity_map<fabric::record_id> m(r);
  const fabric::record_id a = key_n(1);
  const fabric::record_id b = key_n(2);
  const fabric::record_id c = key_n(3);
  REQUIRE(m.insert(a, ent(1, 1)).has_value());
  REQUIRE(m.insert(b, ent(2, 1)).has_value());
  const auto before = pairs_of(m);
  const std::uint64_t bytes = r.totals().live_bytes;

  auto dup_key = m.insert(a, ent(9, 1));
  REQUIRE_FALSE(dup_key.has_value());
  CHECK(dup_key.error().code == sl::errc::already_exists);
  auto dup_entity = m.insert(c, ent(2, 1));
  REQUIRE_FALSE(dup_entity.has_value());
  CHECK(dup_entity.error().code == sl::errc::already_exists);
  auto both = m.insert(a, ent(2, 1));
  CHECK(both.error().code == sl::errc::already_exists);
  auto null = m.insert(c, ecs::null_entity);
  CHECK(null.error().code == sl::errc::invalid_argument);

  CHECK(pairs_of(m) == before);
  CHECK(m.size() == 2);
  CHECK(r.totals().live_bytes == bytes);
  CHECK(m.find(a).value() == ent(1, 1));
  CHECK(m.find(c).error().code == sl::errc::not_found);
  CHECK(m.key_of(ent(9, 1)).error().code == sl::errc::not_found);
  // The same index with another generation is another entity.
  REQUIRE(m.insert(c, ent(2, 2)).has_value());
  CHECK(m.key_of(ent(2, 2)).value() == c);
  CHECK(m.key_of(ent(2, 1)).value() == b);
}

TEST_CASE("RT-INV-ENT-05: erase removes both directions and frees the pair") {
  mem::tracking_resource r("ent.map.erase");
  {
    ecs::entity_map<std::uint64_t> m(r);
    for (std::uint64_t k : {50ULL, 10ULL, 40ULL, 20ULL, 30ULL}) {
      REQUIRE(m.insert(k, ent(static_cast<std::uint32_t>(k), 1)).has_value());
    }
    std::vector<std::uint64_t> order;
    m.for_each([&](std::uint64_t k, ecs::entity e) {
      CHECK(e.index == k);
      order.push_back(k);
    });
    CHECK(order == std::vector<std::uint64_t>{10, 20, 30, 40, 50});
    REQUIRE(m.erase(30).has_value());
    CHECK(m.erase(30).error().code == sl::errc::not_found);
    CHECK(m.find(30).error().code == sl::errc::not_found);
    CHECK(m.key_of(ent(30, 1)).error().code == sl::errc::not_found);
    CHECK(m.size() == 4);
    // After erase the entity and the key are free to map again, one-to-one.
    REQUIRE(m.insert(31, ent(30, 1)).has_value());
    REQUIRE(m.insert(30, ent(30, 2)).has_value());
    CHECK(m.find(31).value() == ent(30, 1));
    CHECK(m.key_of(ent(30, 2)).value() == 30);
    for (std::uint64_t k : {10ULL, 20ULL, 30ULL, 31ULL, 40ULL, 50ULL}) {
      REQUIRE(m.erase(k).has_value());
    }
    CHECK(m.size() == 0);
    CHECK(r.totals().live_bytes == 0);
  }
  CHECK(r.totals().live_blocks == 0);
}
