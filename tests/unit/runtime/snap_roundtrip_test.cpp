// RT-INV-SNAP-01 (SL-ARCH-003 ARCH3-R145, ADR-0026): take then restore reproduces entity handles,
// iteration order and the state hash. The world_gen components are registered with reflection
// (every field snapshot and hashed); a seeded churn fills the world, and the snapshot is restored
// into a fresh world and into a world with another history. Both then match the original: the
// allocator state, the (archetype creation index, entity) iteration order, the oracle's model
// (values, archetype creation order, row order), the world_gen hash, the state hash, the rng
// contributor, and a second take is byte-identical. Resources whose type is a registered component
// come back from the snapshot; a runtime-owned resource stays as the target world had it.

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/memory.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/replay.hpp"
#include "sl/runtime/snapshot.hpp"

#include "../../../engine/runtime/testing/world_gen.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace ecs = sl::runtime::ecs;
namespace mem = sl::runtime::mem;
namespace det = sl::runtime::det;
namespace rtt = sl::runtime::testing;

constexpr rt::field_flags kept = rt::field_flags::snapshot | rt::field_flags::hashed;
constexpr std::uint32_t no_tracked = 0xFFU & ~(1U << 5U); // every kind but gen_tracked

/// The world_gen components with reflection (gen_tracked without), in world_gen's kind order.
void register_reflected(rt::type_registry &types, ecs::component_registry &registry,
                        rtt::gen_ids &ids) {
  rt::type_builder<rtt::gen_position> position("gen.position");
  SL_RT_FIELD(position, rtt::gen_position, x, kept);
  SL_RT_FIELD(position, rtt::gen_position, y, kept);
  SL_RT_FIELD(position, rtt::gen_position, z, kept);
  rt::type_builder<rtt::gen_velocity> velocity("gen.velocity");
  SL_RT_FIELD(velocity, rtt::gen_velocity, dx, kept);
  SL_RT_FIELD(velocity, rtt::gen_velocity, dy, kept);
  rt::type_builder<rtt::gen_padded> padded("gen.padded");
  SL_RT_FIELD(padded, rtt::gen_padded, a, kept);
  SL_RT_FIELD(padded, rtt::gen_padded, b, kept);
  SL_RT_FIELD(padded, rtt::gen_padded, c, kept);
  rt::type_builder<rtt::gen_large> large("gen.large");
  large.bytes("words", 0, sizeof(rtt::gen_large), kept);
  const rt::type_builder<rtt::gen_tag> tag("gen.tag");
  rt::type_builder<rtt::gen_aligned> aligned("gen.aligned");
  SL_RT_FIELD(aligned, rtt::gen_aligned, value, kept);
  rt::type_builder<rtt::gen_health> health("gen.health");
  SL_RT_FIELD(health, rtt::gen_health, hp, kept);
  SL_RT_FIELD(health, rtt::gen_health, armor, kept);
  const auto add = [&types](const rt::type_info &info) {
    auto t = types.add(info);
    REQUIRE(t.has_value());
    return *t;
  };
  const std::array<sl::result<ecs::component_id>, rtt::gen_component_count> got = {
      registry.add<rtt::gen_position>("gen.position", add(position.build())),
      registry.add<rtt::gen_velocity>("gen.velocity", add(velocity.build())),
      registry.add<rtt::gen_padded>("gen.padded", add(padded.build())),
      registry.add<rtt::gen_large>("gen.large", add(large.build())),
      registry.add<rtt::gen_tag>("gen.tag", add(tag.build())),
      registry.add<rtt::gen_tracked>("gen.tracked"),
      registry.add<rtt::gen_aligned>("gen.aligned", add(aligned.build())),
      registry.add<rtt::gen_health>("gen.health", add(health.build()))};
  for (std::size_t k = 0; k < got.size(); ++k) {
    REQUIRE(got.at(k).has_value());
    ids.ids.at(k) = *got.at(k);
  }
}

/// A runtime-owned resource: its type is not a registered component.
struct owned_counter {
  std::uint64_t n = 0;
};

struct fixture {
  fixture() {
    register_reflected(types, registry, ids);
    REQUIRE(types.freeze().has_value());
    REQUIRE(registry.freeze().has_value());
    oracle = std::make_unique<rtt::world_oracle>(ids);
    w = make_world();
  }

  std::unique_ptr<ecs::world> make_world() {
    auto made = ecs::world::make(registry, memory);
    REQUIRE(made.has_value());
    return std::move(*made);
  }

  rt::type_registry types;
  ecs::component_registry registry;
  rtt::gen_ids ids;
  mem::tracking_resource memory{"snap.roundtrip"};
  std::unique_ptr<rtt::world_oracle> oracle; // outlives the worlds
  std::unique_ptr<ecs::world> w;
};

// Applies `count` seeded operations; returns how many the world accepted.
int churn(ecs::world &w, rtt::world_oracle &oracle, std::uint64_t seed, int count) {
  rtt::op_generator gen(seed, no_tracked);
  int accepted = 0;
  for (int i = 0; i < count; ++i) {
    accepted += rtt::apply(gen.next(), w, oracle).has_value() ? 1 : 0;
  }
  return accepted;
}

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

sl::fabric::digest abi_digest() {
  sl::fabric::digest d;
  d.bytes.at(0) = 0xAB;
  return d;
}

rt::snapshot_meta meta_at(std::uint64_t frame, std::uint64_t tick) {
  rt::snapshot_meta m;
  m.frame = frame;
  m.tick = tick;
  m.runtime_abi_digest = abi_digest();
  return m;
}

sl::fabric::digest hash_of(const fixture &f, const ecs::world &w,
                           std::span<const det::snapshot_contributor *const> contributors) {
  auto h = rt::state_hash(w, f.registry, f.types, contributors);
  REQUIRE(h.has_value());
  return *h;
}

} // namespace

TEST_CASE("RT-INV-SNAP-01: take then restore reproduces handles, iteration order and the hash") {
  fixture f;
  REQUIRE(churn(*f.w, *f.oracle, 0x5EED0098, 2500) > 0);
  det::rng_stream stream(7, 1);
  for (int i = 0; i < 13; ++i) {
    [[maybe_unused]] const std::uint64_t draw = stream.next_u64();
  }
  const std::array<det::rng_stream *, 1> streams{&stream};
  const det::rng_contributor rng(streams);
  const std::array<const det::snapshot_contributor *, 1> taken_from{&rng};
  const rt::snapshot_meta meta = meta_at(41, 40);
  auto s = rt::take(*f.w, f.registry, f.types, taken_from, meta);
  REQUIRE(s.has_value());
  CHECK(s->frame() == 41);
  CHECK(s->tick() == 40);
  CHECK(s->runtime_abi_digest() == abi_digest());
  CHECK(s->state_hash() == hash_of(f, *f.w, taken_from));

  // into a fresh world, and into one with another history (its own oracle)
  rtt::world_oracle other_oracle(f.ids);
  std::unique_ptr<ecs::world> fresh = f.make_world();
  std::unique_ptr<ecs::world> other = f.make_world();
  REQUIRE(churn(*other, other_oracle, 99, 700) > 0);
  for (ecs::world *target : {fresh.get(), other.get()}) {
    det::rng_stream restored_stream(1234, 5);
    const std::array<det::rng_stream *, 1> restored_streams{&restored_stream};
    det::rng_contributor restored_rng(restored_streams);
    const std::array<det::snapshot_contributor *, 1> restore_into{&restored_rng};
    REQUIRE(rt::restore(*target, f.registry, f.types, restore_into, *s, abi_digest()).has_value());

    CHECK(target->entities().capture() == f.w->entities().capture());
    CHECK(iteration_order(*target) == iteration_order(*f.w));
    CHECK(f.oracle->compare(*target).empty());
    CHECK(rtt::hash_world(*target, f.ids) == rtt::hash_world(*f.w, f.ids));
    CHECK(restored_stream == stream);
    const std::array<const det::snapshot_contributor *, 1> hashed{&restored_rng};
    CHECK(hash_of(f, *target, hashed) == s->state_hash());
    auto again = rt::take(*target, f.registry, f.types, hashed, meta);
    REQUIRE(again.has_value());
    CHECK(std::ranges::equal(again->bytes(), s->bytes()));
  }
}

TEST_CASE("RT-INV-SNAP-01: the restored world continues like the original") {
  fixture f;
  REQUIRE(churn(*f.w, *f.oracle, 3, 1200) > 0);
  auto s = rt::take(*f.w, f.registry, f.types, {}, meta_at(1, 1));
  REQUIRE(s.has_value());
  rtt::world_oracle copy_oracle(f.ids);
  std::unique_ptr<ecs::world> copy = f.make_world();
  REQUIRE(churn(*copy, copy_oracle, 3, 1200) > 0); // the same history builds the same oracle
  std::unique_ptr<ecs::world> restored = f.make_world();
  REQUIRE(rt::restore(*restored, f.registry, f.types, {}, *s, abi_digest()).has_value());
  CHECK(copy_oracle.compare(*restored).empty());
  // the same next operations on the original and on the restored world
  REQUIRE(churn(*f.w, *f.oracle, 77, 800) == churn(*restored, copy_oracle, 77, 800));
  CHECK(f.oracle->compare(*f.w).empty());
  CHECK(copy_oracle.compare(*restored).empty());
  CHECK(iteration_order(*restored) == iteration_order(*f.w));
  CHECK(hash_of(f, *restored, {}) == hash_of(f, *f.w, {}));
}

TEST_CASE("RT-INV-SNAP-01: component resources come back, runtime-owned resources stay") {
  fixture f;
  REQUIRE(churn(*f.w, *f.oracle, 11, 300) > 0);
  constexpr ecs::resource_key health_key{3};
  constexpr ecs::resource_key counter_key{9};
  REQUIRE(f.w->add_resource(health_key, rtt::gen_health{.hp = 77, .armor = -4}).has_value());
  REQUIRE(f.w->add_resource(counter_key, owned_counter{.n = 1}).has_value());
  auto s = rt::take(*f.w, f.registry, f.types, {}, meta_at(1, 1));
  REQUIRE(s.has_value());

  std::unique_ptr<ecs::world> target = f.make_world();
  REQUIRE(target->add_resource(counter_key, owned_counter{.n = 42}).has_value());
  REQUIRE(target->add_resource(ecs::resource_key{5}, rtt::gen_health{.hp = 1}).has_value());
  REQUIRE(rt::restore(*target, f.registry, f.types, {}, *s, abi_digest()).has_value());
  auto health = target->resource<rtt::gen_health>(health_key);
  REQUIRE(health.has_value());
  CHECK((*health)->hp == 77);
  CHECK((*health)->armor == -4);
  auto counter = target->resource<owned_counter>(counter_key);
  REQUIRE(counter.has_value());
  CHECK((*counter)->n == 42); // runtime-owned: kept from the target
  auto dropped = target->resource<rtt::gen_health>(ecs::resource_key{5});
  REQUIRE_FALSE(dropped.has_value()); // a component resource the snapshot lacks is gone
  CHECK(dropped.error().code == sl::errc::not_found);
  CHECK(hash_of(f, *target, {}) == s->state_hash());
}
