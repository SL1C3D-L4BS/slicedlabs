// RT-INV-QUERY-02 (SL-ARCH-003 ARCH3-R115, ADR-0025): the match cache updates incrementally
// from the world's archetype generation. The first iteration examines every archetype; another
// one with no new archetype examines none, whatever rows were created or destroyed meanwhile;
// after k new archetypes (create_with or an add transition) it examines exactly those k and
// visits the matching ones; a restored world or another world is examined again in full; and
// chunks_visited counts the non-empty chunks iterated.

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/query.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/memory.hpp"

#include "../../../engine/runtime/testing/world_gen.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace {

namespace ecs = sl::runtime::ecs;
namespace mem = sl::runtime::mem;
namespace rtt = sl::runtime::testing;

using rtt::gen_health;
using rtt::gen_large;
using rtt::gen_position;
using rtt::gen_tag;
using rtt::gen_velocity;

struct fixture {
  fixture() {
    auto got = rtt::register_components(registry);
    REQUIRE(got.has_value());
    REQUIRE(registry.freeze().has_value());
    auto made = ecs::world::make(registry, memory);
    REQUIRE(made.has_value());
    w = std::move(*made);
  }
  ecs::component_registry registry;
  mem::tracking_resource memory{"query.cache"};
  std::unique_ptr<ecs::world> w;
};

using position_query = ecs::query<ecs::read<gen_position>>;

std::vector<ecs::entity> visit(position_query &q, ecs::world &w) {
  std::vector<ecs::entity> out;
  q.for_each(w, {}, [&out](ecs::entity e, const gen_position &) noexcept { out.push_back(e); });
  return out;
}

std::uint64_t archetype_count(const ecs::world &w) { return w.stats().archetypes; }

} // namespace

TEST_CASE("RT-INV-QUERY-02: without a new archetype the cache examines nothing") {
  fixture f;
  REQUIRE(f.w->create_with(gen_position{}).has_value());
  REQUIRE(f.w->create_with(gen_position{}, gen_velocity{}).has_value());
  REQUIRE(f.w->create_with(gen_velocity{}).has_value());
  REQUIRE(f.w->create_with(gen_health{}).has_value());
  auto q = position_query::make(f.registry);
  REQUIRE(q.has_value());
  CHECK(q->archetypes_visited() == 0);
  CHECK(visit(*q, *f.w).size() == 2);
  const std::uint64_t first = q->archetypes_visited();
  CHECK(first == archetype_count(*f.w));
  CHECK(visit(*q, *f.w).size() == 2);
  CHECK(q->archetypes_visited() == first);

  // Rows come and go in existing archetypes: still nothing to examine.
  std::vector<ecs::entity> more;
  for (int i = 0; i < 50; ++i) {
    auto e = f.w->create_with(gen_position{}, gen_velocity{});
    REQUIRE(e.has_value());
    more.push_back(*e);
  }
  REQUIRE(f.w->destroy(more.front()).has_value());
  const std::uint64_t generation = f.w->archetype_generation();
  CHECK(visit(*q, *f.w).size() == 51);
  CHECK(q->archetypes_visited() == first);
  CHECK(f.w->archetype_generation() == generation);
}

TEST_CASE("RT-INV-QUERY-02: after new archetypes the cache examines only those") {
  fixture f;
  REQUIRE(f.w->create_with(gen_position{}).has_value());
  REQUIRE(f.w->create_with(gen_health{}).has_value());
  auto q = position_query::make(f.registry);
  REQUIRE(q.has_value());
  CHECK(visit(*q, *f.w).size() == 1);
  std::uint64_t visited = q->archetypes_visited();
  CHECK(visited == archetype_count(*f.w));

  // One new archetype that matches.
  auto tagged = f.w->create_with(gen_position{}, gen_tag{});
  REQUIRE(tagged.has_value());
  std::vector<ecs::entity> seen = visit(*q, *f.w);
  CHECK(q->archetypes_visited() == visited + 1);
  visited = q->archetypes_visited();
  CHECK(seen.size() == 2);
  CHECK(seen.back() == *tagged);

  // Three new archetypes, one of them without position.
  const std::uint64_t before = archetype_count(*f.w);
  REQUIRE(f.w->create_with(gen_position{}, gen_health{}).has_value());
  REQUIRE(f.w->create_with(gen_velocity{}, gen_tag{}).has_value());
  REQUIRE(f.w->create_with(gen_position{}, gen_large{}).has_value());
  REQUIRE(archetype_count(*f.w) == before + 3);
  CHECK(visit(*q, *f.w).size() == 4);
  CHECK(q->archetypes_visited() == visited + 3);
  visited = q->archetypes_visited();

  // An add transition that creates an archetype.
  auto e = f.w->create_with(gen_health{});
  REQUIRE(e.has_value());
  REQUIRE(f.w->add(*e, gen_velocity{}).has_value());
  REQUIRE(f.w->add(*e, gen_position{}).has_value());
  const std::uint64_t made = archetype_count(*f.w) - before - 3;
  CHECK(made == 2); // {velocity, health} and {position, velocity, health}
  CHECK(visit(*q, *f.w).size() == 5);
  CHECK(q->archetypes_visited() == visited + made);

  // Several queries keep their own caches.
  auto other = position_query::make(f.registry);
  REQUIRE(other.has_value());
  CHECK(visit(*other, *f.w).size() == 5);
  CHECK(other->archetypes_visited() == archetype_count(*f.w));
}

TEST_CASE("RT-INV-QUERY-02: a restored world or another world is examined in full") {
  fixture source;
  REQUIRE(source.w->create_with(gen_position{}).has_value());
  REQUIRE(source.w->create_with(gen_position{}, gen_velocity{}).has_value());
  REQUIRE(source.w->create_with(gen_tag{}).has_value());
  const ecs::world_image image = source.w->capture_image();

  fixture target;
  REQUIRE(target.w->create_with(gen_health{}).has_value());
  REQUIRE(target.w->create_with(gen_position{}, gen_health{}).has_value());
  auto q = position_query::make(target.registry);
  REQUIRE(q.has_value());
  CHECK(visit(*q, *target.w).size() == 1);
  std::uint64_t visited = q->archetypes_visited();
  REQUIRE(target.w->restore_from(image).has_value());
  CHECK(visit(*q, *target.w).size() == 2);
  CHECK(q->archetypes_visited() == visited + archetype_count(*target.w));
  visited = q->archetypes_visited();

  // The same query on another world of the same registry, then back.
  auto made = ecs::world::make(target.registry, target.memory);
  REQUIRE(made.has_value());
  ecs::world &second = **made;
  REQUIRE(second.create_with(gen_position{}, gen_tag{}).has_value());
  CHECK(visit(*q, second).size() == 1);
  CHECK(q->archetypes_visited() == visited + archetype_count(second));
  visited = q->archetypes_visited();
  CHECK(visit(*q, *target.w).size() == 2);
  CHECK(q->archetypes_visited() == visited + archetype_count(*target.w));

  // A world of another registry is not iterated at all.
  const std::uint64_t now = q->archetypes_visited();
  CHECK(visit(*q, *source.w).empty());
  CHECK(q->archetypes_visited() == now);
}

TEST_CASE("RT-INV-QUERY-02: chunks_visited counts the non-empty chunks iterated") {
  fixture f;
  std::vector<ecs::entity> es;
  for (int i = 0; i < 2500; ++i) {
    auto e = f.w->create_with(gen_position{});
    REQUIRE(e.has_value());
    es.push_back(*e);
  }
  const ecs::archetype &a = *f.w->archetypes().back();
  const std::uint64_t chunks = (2500 + a.capacity() - 1) / a.capacity();
  auto q = position_query::make(f.registry);
  REQUIRE(q.has_value());
  CHECK(visit(*q, *f.w).size() == 2500);
  CHECK(q->chunks_visited() == chunks);
  for (std::size_t i = 0; i < 2000; ++i) {
    REQUIRE(f.w->destroy(es[i]).has_value());
  }
  // Emptied chunks stay until the boundary but are not iterated.
  CHECK(visit(*q, *f.w).size() == 500);
  CHECK(q->chunks_visited() == chunks + ((500 + a.capacity() - 1) / a.capacity()));
}
