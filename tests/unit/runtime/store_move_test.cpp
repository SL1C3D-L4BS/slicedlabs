// RT-INV-STORE-02 (SL-ARCH-003 ARCH3-R110, ADR-0025): moving rows between archetypes keeps every
// component value and constructs or destroys none. gen_tracked is not trivially relocatable and
// counts its instances: after every operation the live count equals the values the oracle holds,
// every stored instance is intact (moved by its move constructor, never by a byte copy), and after
// the world is destroyed construction and destruction balance.

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/memory.hpp"

#include "../../../engine/runtime/testing/world_gen.hpp"

#include <doctest/doctest.h>

#include <bit>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace ecs = sl::runtime::ecs;
namespace mem = sl::runtime::mem;
namespace rtt = sl::runtime::testing;

constexpr std::uint32_t k_tracked = 5;

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
  mem::tracking_resource memory{"store.move"};
  std::unique_ptr<rtt::world_oracle> oracle; // outlives the world (gen_tracked counts into it)
  std::unique_ptr<ecs::world> w;
};

std::int64_t tracked_in_oracle(const rtt::world_oracle &oracle) {
  std::int64_t n = 0;
  for (const auto &[e, vals] : oracle.entities()) {
    n += vals.values[k_tracked].has_value() ? 1 : 0;
  }
  return n;
}

bool all_tracked_intact(const ecs::world &w, ecs::component_id tracked) {
  for (const ecs::archetype *a : w.archetypes()) {
    if (!a->contains(tracked)) {
      continue;
    }
    for (std::uint32_t c = 0; c < a->chunk_count(); ++c) {
      const auto *col = static_cast<const rtt::gen_tracked *>(a->column(tracked, c));
      for (std::uint32_t r = 0; r < a->rows(c); ++r) {
        if (!col[r].intact()) {
          return false;
        }
      }
    }
  }
  return true;
}

} // namespace

TEST_CASE("RT-INV-STORE-02: moves keep values and construct or destroy nothing extra") {
  for (const std::uint64_t seed : {7ULL, 0x0920ULL}) {
    fixture f;
    rtt::op_generator gen(seed);
    std::uint64_t moved = 0;
    for (int i = 0; i < 2000; ++i) {
      const auto version = f.w->version();
      const auto r = rtt::apply(gen.next(), *f.w, *f.oracle);
      if (f.w->version() != version) { // a structural change (an in-place set is not one)
        moved += f.w->stats().last_rows_moved;
      }
      if (!r) {
        const sl::errc code = r.error().code;
        CHECK((code == sl::errc::component_exists || code == sl::errc::component_missing));
      }
      REQUIRE(f.oracle->counter().live() == tracked_in_oracle(*f.oracle));
      if (i % 250 == 249) {
        REQUIRE(all_tracked_intact(*f.w, f.ids.ids[k_tracked]));
        REQUIRE(f.oracle->compare(*f.w).empty());
      }
    }
    CHECK(moved == f.w->stats().rows_moved);
    CHECK(moved > 0);
    CHECK(f.oracle->counter().constructed > f.oracle->counter().live());
    f.w.reset();
    CHECK(f.oracle->counter().live() == 0);
    CHECK(f.oracle->counter().constructed == f.oracle->counter().destroyed);
    CHECK(f.memory.totals().live_bytes == 0);
  }
}

TEST_CASE("RT-INV-STORE-02: one entity keeps its values through many archetypes") {
  fixture f;
  rtt::live_counter &counter = f.oracle->counter();
  auto e = f.w->create_with(rtt::gen_tracked(counter, 41), rtt::gen_health{.hp = 9, .armor = 3});
  REQUIRE(e.has_value());
  auto other = f.w->create_with(rtt::gen_tracked(counter, 99), rtt::gen_health{.hp = 1});
  REQUIRE(other.has_value());
  CHECK(counter.live() == 2);
  rtt::gen_large big;
  big.words[17] = 0xABCDEF;
  REQUIRE(f.w->add(*e, big).has_value()); // an lvalue: T deduces to gen_large &
  REQUIRE(f.w->add<rtt::gen_tag>(*e, rtt::gen_tag{}).has_value());
  REQUIRE(f.w->add<rtt::gen_aligned>(*e, rtt::gen_aligned{.value = 5}).has_value());
  REQUIRE(f.w->add<rtt::gen_padded>(*e, rtt::gen_padded{.a = 1, .b = 2, .c = 3}).has_value());
  REQUIRE(f.w->remove<rtt::gen_health>(*e).has_value());
  REQUIRE(f.w->remove<rtt::gen_large>(*e).has_value());
  REQUIRE(f.w->add<rtt::gen_velocity>(*e, rtt::gen_velocity{.dx = 0.5, .dy = -2.0}).has_value());
  CHECK(counter.live() == 2);
  CHECK(f.w->stats().last_rows_moved == 1); // only e's row: it was alone in its source

  auto t = f.w->get<rtt::gen_tracked>(*e);
  REQUIRE(t.has_value());
  CHECK((*t)->value() == 41);
  CHECK((*t)->intact());
  CHECK((*f.w->get<rtt::gen_aligned>(*e))->value == 5);
  CHECK(std::bit_cast<std::uintptr_t>(*f.w->get<rtt::gen_aligned>(*e)) % 64 == 0);
  CHECK((*f.w->get<rtt::gen_padded>(*e))->b == 2);
  CHECK((*f.w->get<rtt::gen_velocity>(*e))->dy == -2.0);
  CHECK(f.w->get<rtt::gen_health>(*e).error().code == sl::errc::component_missing);
  auto o = f.w->get<rtt::gen_tracked>(*other);
  REQUIRE(o.has_value());
  CHECK((*o)->value() == 99);
  CHECK((*f.w->get<rtt::gen_health>(*other))->hp == 1);

  REQUIRE(f.w->destroy(*e).has_value());
  CHECK(counter.live() == 1);
  f.w.reset();
  CHECK(counter.live() == 0);
  CHECK(counter.constructed == counter.destroyed);
}

TEST_CASE("RT-INV-STORE-02: swap-remove moves the last row into the hole intact") {
  fixture f;
  rtt::live_counter &counter = f.oracle->counter();
  std::vector<ecs::entity> es;
  for (std::uint64_t i = 0; i < 5; ++i) {
    auto e = f.w->create_with(rtt::gen_tracked(counter, i));
    REQUIRE(e.has_value());
    es.push_back(*e);
  }
  REQUIRE(f.w->add<rtt::gen_tag>(es[1], rtt::gen_tag{}).has_value());
  CHECK(f.w->stats().last_rows_moved == 2); // es[1] moved out, es[4] filled row 1
  REQUIRE(f.w->destroy(es[0]).has_value());
  CHECK(f.w->stats().last_rows_moved == 1); // es[3] filled row 0
  const ecs::archetype &a = *f.w->archetypes()[0];
  REQUIRE(a.rows(0) == 3);
  CHECK(a.entities(0)[0] == es[3]);
  CHECK(a.entities(0)[1] == es[4]);
  CHECK(a.entities(0)[2] == es[2]);
  for (std::size_t i = 1; i < es.size(); ++i) {
    auto t = f.w->get<rtt::gen_tracked>(es[i]);
    REQUIRE(t.has_value());
    CHECK((*t)->value() == i);
    CHECK((*t)->intact());
  }
  CHECK(counter.live() == 4);
}
