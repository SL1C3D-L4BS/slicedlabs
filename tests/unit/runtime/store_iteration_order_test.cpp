// RT-INV-STORE-06 (SL-ARCH-003 ARCH3-R111, ADR-0025, `determinism`): iteration order is archetype
// creation order, then chunk, then row, and it is a function of the structural history only: two
// worlds fed the same seeded history iterate the same entities in the same order and hash the
// same, the oracle's independent model of creation order and swap-remove agrees, and a world
// restored from a captured image reproduces the order and the hash.

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/memory.hpp"

#include "../../../engine/runtime/testing/world_gen.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace ecs = sl::runtime::ecs;
namespace mem = sl::runtime::mem;
namespace rtt = sl::runtime::testing;

constexpr std::uint32_t no_tracked = 0xFFU & ~(1U << 5U); // every kind but gen_tracked

struct fixture {
  fixture() {
    auto got = rtt::register_components(registry);
    REQUIRE(got.has_value());
    ids = *got;
    REQUIRE(registry.freeze().has_value());
    oracle = std::make_unique<rtt::world_oracle>(ids);
    auto made = ecs::world::make(registry, memory);
    REQUIRE(made.has_value());
    w = std::move(*made);
  }
  ecs::component_registry registry;
  rtt::gen_ids ids;
  mem::tracking_resource memory{"store.order"};
  std::unique_ptr<rtt::world_oracle> oracle; // outlives the world (gen_tracked counts into it)
  std::unique_ptr<ecs::world> w;
};

using order = std::vector<std::pair<std::uint32_t, ecs::entity>>;

order iteration_order(const ecs::world &w) {
  order out;
  for (const ecs::archetype *a : w.archetypes()) {
    for (std::uint32_t c = 0; c < a->chunk_count(); ++c) {
      std::ranges::transform(a->entities(c), std::back_inserter(out),
                             [a](ecs::entity e) { return std::pair{a->creation_index(), e}; });
    }
  }
  return out;
}

// Applies `ops` seeded operations; returns how many the world accepted.
int churn(fixture &f, std::uint64_t seed, int ops, std::uint32_t mask = 0xFFU) {
  rtt::op_generator gen(seed, mask);
  int accepted = 0;
  for (int i = 0; i < ops; ++i) {
    accepted += rtt::apply(gen.next(), *f.w, *f.oracle).has_value() ? 1 : 0;
  }
  return accepted;
}

} // namespace

TEST_CASE("RT-INV-STORE-06: identical histories iterate identically") {
  for (const std::uint64_t seed : {3ULL, 0xC0FFEEULL}) {
    fixture one;
    fixture two;
    churn(one, seed, 2500);
    churn(two, seed, 2500);
    CHECK(one.oracle->compare(*one.w).empty());
    CHECK(two.oracle->compare(*two.w).empty());
    const order a = iteration_order(*one.w);
    const order b = iteration_order(*two.w);
    CHECK(a.size() == one.w->entities().alive());
    CHECK(a == b);
    CHECK(rtt::hash_world(*one.w, one.ids) == rtt::hash_world(*two.w, two.ids));
    CHECK(one.w->version() == two.w->version());
    CHECK(one.w->archetype_generation() == two.w->archetype_generation());

    fixture other;
    churn(other, seed + 1, 2500);
    CHECK(rtt::hash_world(*other.w, other.ids) != rtt::hash_world(*one.w, one.ids));
  }
}

TEST_CASE("RT-INV-STORE-06: removal fills the hole from the last row") {
  fixture f;
  std::vector<ecs::entity> es;
  for (int i = 0; i < 4; ++i) {
    auto e = f.w->create_with(rtt::gen_position{.x = static_cast<float>(i)});
    REQUIRE(e.has_value());
    es.push_back(*e);
  }
  REQUIRE(f.w->destroy(es[1]).has_value());
  CHECK(iteration_order(*f.w) == order{{0, es[0]}, {0, es[3]}, {0, es[2]}});
  REQUIRE(f.w->add<rtt::gen_velocity>(es[0], rtt::gen_velocity{}).has_value());
  CHECK(iteration_order(*f.w) == order{{0, es[2]}, {0, es[3]}, {1, es[0]}});
  REQUIRE(f.w->remove<rtt::gen_velocity>(es[0]).has_value());
  CHECK(iteration_order(*f.w) == order{{0, es[2]}, {0, es[3]}, {0, es[0]}});
  CHECK((*f.w->get<rtt::gen_position>(es[3]))->x == 3.0F);
}

TEST_CASE("RT-INV-STORE-06: a restored world reproduces the order and the hash") {
  fixture source;
  churn(source, 0x0920, 2000, no_tracked);
  const ecs::world_image image = source.w->capture_image();

  fixture target;
  churn(target, 0x1234, 300, no_tracked); // replaced wholesale by the restore
  REQUIRE(target.w->restore_from(image).has_value());
  CHECK(iteration_order(*target.w) == iteration_order(*source.w));
  CHECK(rtt::hash_world(*target.w, target.ids) == rtt::hash_world(*source.w, source.ids));
  CHECK(source.oracle->compare(*target.w).empty());
  CHECK(target.w->change_tick() == source.w->change_tick());

  // The restored world goes on exactly like the source: the same calls on the same handles.
  const order before = iteration_order(*source.w);
  for (std::size_t i = 0; i < before.size(); i += 3) {
    const ecs::entity e = before[i].second;
    if (i % 2 == 0) {
      REQUIRE(source.w->destroy(e).has_value());
      REQUIRE(target.w->destroy(e).has_value());
    } else if (source.w->has(e, source.ids.ids[4])) {
      REQUIRE(source.w->remove<rtt::gen_tag>(e).has_value());
      REQUIRE(target.w->remove<rtt::gen_tag>(e).has_value());
    } else {
      REQUIRE(source.w->add<rtt::gen_tag>(e, rtt::gen_tag{}).has_value());
      REQUIRE(target.w->add<rtt::gen_tag>(e, rtt::gen_tag{}).has_value());
    }
  }
  for (int i = 0; i < 100; ++i) {
    auto s = source.w->create_with(rtt::gen_health{.hp = i});
    auto t = target.w->create_with(rtt::gen_health{.hp = i});
    REQUIRE(s.has_value());
    REQUIRE(t.has_value());
    CHECK(*s == *t);
  }
  CHECK(iteration_order(*target.w) == iteration_order(*source.w));
  CHECK(rtt::hash_world(*target.w, target.ids) == rtt::hash_world(*source.w, source.ids));
}
