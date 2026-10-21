// RT-INV-STORE-01 (SL-ARCH-003 ARCH3-R107, ADR-0025): after seeded churn every live entity is in
// exactly one archetype row and row counts equal live counts. Archetypes are sorted id sets,
// created on demand in playback order, kept in creation order, never destroyed, and limited by
// world_config::max_archetypes (archetype_limit, nothing changes). The checked accessors answer
// stale_entity, component_unknown, component_missing and component_exists.

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/memory.hpp"

#include "../../../engine/runtime/testing/world_gen.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace ecs = sl::runtime::ecs;
namespace mem = sl::runtime::mem;
namespace rtt = sl::runtime::testing;

struct unregistered {
  int v = 0;
};

struct fixture {
  explicit fixture(ecs::world_config config = {}) {
    auto got = rtt::register_components(registry);
    REQUIRE(got.has_value());
    ids = *got;
    REQUIRE(registry.freeze().has_value());
    oracle = std::make_unique<rtt::world_oracle>(ids);
    auto made = ecs::world::make(registry, memory, config);
    REQUIRE(made.has_value());
    w = std::move(*made);
  }
  ecs::component_registry registry;
  rtt::gen_ids ids;
  mem::tracking_resource memory{"store.rows"};
  std::unique_ptr<rtt::world_oracle> oracle; // outlives the world (gen_tracked counts into it)
  std::unique_ptr<ecs::world> w;
};

std::string joined(const std::vector<std::string> &lines) {
  std::string out;
  for (const std::string &l : lines) {
    out += l + "\n";
  }
  return out;
}

} // namespace

TEST_CASE("RT-INV-STORE-01: after seeded churn every live entity is in exactly one row") {
  for (const std::uint64_t seed : {1ULL, 2ULL, 0x5EED0920ULL}) {
    fixture f;
    rtt::world_oracle &oracle = *f.oracle;
    rtt::op_generator gen(seed);
    for (int i = 0; i < 3000; ++i) {
      const auto r = rtt::apply(gen.next(), *f.w, oracle);
      if (!r) {
        const sl::errc code = r.error().code;
        CHECK((code == sl::errc::component_exists || code == sl::errc::component_missing));
      }
      if (i % 500 == 499) {
        const auto diff = oracle.compare(*f.w);
        INFO(joined(diff));
        REQUIRE(diff.empty());
      }
    }
    std::map<ecs::entity, int> seen;
    std::uint64_t rows = 0;
    for (const ecs::archetype *a : f.w->archetypes()) {
      for (std::uint32_t c = 0; c < a->chunk_count(); ++c) {
        rows += a->rows(c);
        for (const ecs::entity e : a->entities(c)) {
          ++seen[e];
        }
      }
      CHECK(rows <= f.w->entities().alive());
    }
    CHECK(rows == f.w->entities().alive());
    CHECK(seen.size() == f.w->entities().alive());
    for (const auto &[e, count] : seen) {
      CHECK(count == 1);
      CHECK(f.w->is_alive(e));
    }
    CHECK(f.w->stats().rows == rows);
    CHECK(f.w->archetypes().size() > 16); // the churn reached many signatures
    CHECK(oracle.compare(*f.w).empty());
  }
}

TEST_CASE("RT-INV-STORE-01: archetypes are sorted id sets kept in creation order") {
  fixture f;
  const auto pos = f.ids.ids[0];
  const auto vel = f.ids.ids[1];
  CHECK(f.w->archetypes().empty());
  CHECK(f.w->archetype_generation() == 0);
  auto e = f.w->create_with(rtt::gen_velocity{.dx = 1.0, .dy = 2.0}, rtt::gen_position{});
  REQUIRE(e.has_value());
  REQUIRE(f.w->archetypes().size() == 1);
  const ecs::archetype &first = *f.w->archetypes()[0];
  REQUIRE(first.components().size() == 2);
  CHECK(first.components()[0] == pos); // ascending ids whatever the argument order
  CHECK(first.components()[1] == vel);
  CHECK(first.creation_index() == 0);
  CHECK(f.w->version() == 1);
  CHECK(f.w->archetype_generation() == 1);

  // {vel} is new; adding position again returns to {pos, vel}.
  REQUIRE(f.w->remove<rtt::gen_position>(*e).has_value());
  REQUIRE(f.w->add<rtt::gen_position>(*e, rtt::gen_position{}).has_value());
  CHECK(f.w->archetypes().size() == 2);
  CHECK(f.w->archetype_generation() == 2);
  CHECK(f.w->version() == 3);
  auto plain = f.w->create(); // {}: new, third
  REQUIRE(plain.has_value());
  REQUIRE(f.w->archetypes().size() == 3);
  CHECK(f.w->archetypes()[2]->components().empty());
  CHECK(f.w->archetypes()[2]->creation_index() == 2);
  REQUIRE(f.w->destroy(*e).has_value()); // archetypes are never destroyed
  CHECK(f.w->archetypes().size() == 3);
  CHECK(f.w->archetypes()[0]->row_count() == 0);
  CHECK(f.w->get<rtt::gen_velocity>(*plain).error().code == sl::errc::component_missing);
}

TEST_CASE("RT-INV-STORE-01: the archetype limit is archetype_limit and changes nothing") {
  fixture f(ecs::world_config{.max_entities = 64, .max_archetypes = 2});
  auto e = f.w->create();
  REQUIRE(e.has_value());
  REQUIRE(f.w->add<rtt::gen_position>(*e, rtt::gen_position{}).has_value());
  const auto version = f.w->version();
  const auto before = rtt::hash_world(*f.w, f.ids);
  auto refused = f.w->add<rtt::gen_health>(*e, rtt::gen_health{.hp = 3, .armor = 1});
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().code == sl::errc::archetype_limit);
  CHECK(f.w->archetypes().size() == 2);
  CHECK(f.w->version() == version);
  CHECK(rtt::hash_world(*f.w, f.ids) == before);
  CHECK(f.w->has(*e, f.ids.ids[0]));
}

TEST_CASE("RT-INV-STORE-01: checked access answers stale, unknown, missing and exists") {
  fixture f;
  auto e = f.w->create_with(rtt::gen_health{.hp = 7, .armor = 2});
  REQUIRE(e.has_value());
  auto hp = f.w->get<rtt::gen_health>(*e);
  REQUIRE(hp.has_value());
  CHECK((*hp)->hp == 7);
  CHECK(f.w->get<unregistered>(*e).error().code == sl::errc::component_unknown);
  CHECK(f.w->get<rtt::gen_tag>(*e).error().code == sl::errc::component_missing);
  CHECK(f.w->remove<rtt::gen_tag>(*e).error().code == sl::errc::component_missing);
  CHECK(f.w->add<rtt::gen_health>(*e, rtt::gen_health{}).error().code ==
        sl::errc::component_exists);
  CHECK(f.w->create_with(rtt::gen_tag{}, rtt::gen_tag{}).error().code ==
        sl::errc::invalid_argument);
  REQUIRE(f.w->destroy(*e).has_value());
  CHECK_FALSE(f.w->is_alive(*e));
  CHECK(f.w->destroy(*e).error().code == sl::errc::stale_entity);
  CHECK(f.w->get<rtt::gen_health>(*e).error().code == sl::errc::stale_entity);
  CHECK(f.w->add<rtt::gen_tag>(*e, rtt::gen_tag{}).error().code == sl::errc::stale_entity);
  CHECK(f.w->set<rtt::gen_tag>(*e, rtt::gen_tag{}).error().code == sl::errc::stale_entity);
  CHECK(f.w->entities().alive() == 0);
}
