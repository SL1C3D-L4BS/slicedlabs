// RT-INV-CHG-04 (SL-ARCH-003 ARCH3-R130, ADR-0025): the world version increments iff a
// structural change happened and the archetype generation iff an archetype was created. Every
// world call is applied once with the expected deltas: create, destroy, add, remove and a set
// that adds move the version by one; refused calls, a set that replaces, reads, mark_changed, a
// write<T> iteration, sweep_ticks, release_empty_chunks, resources and the change tick move
// nothing; the generation moves with the archetype count. Through command flushes the version
// moves by the number of entity signals of the structural_log batch and the generation by its
// archetype_created signals; restore_from moves both.

#include "sl/core/error.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/ecs/change.hpp"
#include "sl/runtime/ecs/command_buffer.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/query.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/memory.hpp"
#include "sl/runtime/replay.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace {

namespace ecs = sl::runtime::ecs;
namespace mem = sl::runtime::mem;
namespace det = sl::runtime::det;

struct pos {
  std::int32_t x = 0;
};
struct vel {
  std::int32_t v = 0;
};
struct tag {
  std::int32_t t = 0;
};

struct splitmix {
  std::uint64_t state = 0;
  std::uint64_t next() {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
  }
  std::uint32_t below(std::uint32_t n) { return static_cast<std::uint32_t>(next() % n); }
};

struct rig {
  ecs::component_registry registry;
  mem::tracking_resource memory{"chg.versions"};
  mem::tracking_resource arena_memory{"chg.versions.arena"};
  std::unique_ptr<ecs::world> w;
  std::unique_ptr<mem::frame_arena> arena;
  std::unique_ptr<ecs::command_queue> queue;

  rig() {
    REQUIRE(registry.add<pos>("pos").has_value());
    REQUIRE(registry.add<vel>("vel").has_value());
    REQUIRE(registry.add<tag>("tag").has_value());
    REQUIRE(registry.freeze().has_value());
    auto made = ecs::world::make(registry, memory);
    REQUIRE(made.has_value());
    w = std::move(*made);
    arena = std::make_unique<mem::frame_arena>(arena_memory, 0);
    queue = std::make_unique<ecs::command_queue>(registry, *arena);
  }
};

// The version, generation and archetype count since the last step.
struct probe {
  const ecs::world *w = nullptr;
  std::uint64_t version = 0;
  std::uint64_t generation = 0;
  std::size_t archetypes = 0;

  void reset() {
    version = w->version();
    generation = w->archetype_generation();
    archetypes = w->archetypes().size();
  }
  // Checks the version delta and that the generation moved with the archetype count; returns the
  // generation delta.
  std::uint64_t step(std::uint64_t dv) {
    CHECK(w->version() - version == dv);
    const std::uint64_t dg = w->archetype_generation() - generation;
    CHECK(dg == w->archetypes().size() - archetypes);
    reset();
    return dg;
  }
};

det::ordered_key key(std::uint16_t system) {
  return det::ordered_key{
      .tick = 2, .phase = 4, .system_index = system, .chunk_index = 0, .local_seq = 0};
}

} // namespace

TEST_CASE("RT-INV-CHG-04: the version moves on structural changes only") {
  rig r;
  ecs::world &w = *r.w;
  probe p{.w = &w};
  p.reset();

  auto e = w.create_with(pos{.x = 1});
  REQUIRE(e.has_value());
  CHECK(p.step(1) == 1); // {pos} is new
  REQUIRE(w.create_with(pos{.x = 2}).has_value());
  CHECK(p.step(1) == 0);
  REQUIRE(w.create().has_value());
  static_cast<void>(p.step(1)); // the empty archetype: new or not, the generation follows it
  REQUIRE(w.add(*e, vel{.v = 1}).has_value());
  CHECK(p.step(1) == 1); // {pos, vel}

  CHECK_FALSE(w.add(*e, vel{.v = 2}).has_value()); // component_exists
  CHECK(p.step(0) == 0);
  REQUIRE(w.remove<vel>(*e).has_value());
  CHECK(p.step(1) == 0);
  CHECK_FALSE(w.remove<vel>(*e).has_value()); // component_missing
  CHECK(p.step(0) == 0);
  REQUIRE(w.set(*e, pos{.x = 3}).has_value()); // replaces
  CHECK(p.step(0) == 0);
  REQUIRE(w.set(*e, pos{.x = 4}, ecs::write_source::document).has_value());
  CHECK(p.step(0) == 0);
  // A set that adds tag: {pos, tag} is new.
  REQUIRE(w.set(*e, tag{.t = 1}).has_value());
  CHECK(p.step(1) == 1);
  // A type given twice: invalid_argument.
  CHECK_FALSE(w.create_with(pos{}, pos{}).has_value());
  CHECK(p.step(0) == 0);

  // Reads, writes and bookkeeping are not structural.
  REQUIRE(w.get<pos>(*e).has_value());
  REQUIRE(ecs::mark_changed<pos>(w, *e, 7).has_value());
  auto writer = ecs::query<ecs::write<pos>>::make(r.registry);
  REQUIRE(writer.has_value());
  writer->for_each(w, {.since = 0, .write_tick = 8}, [](ecs::entity, pos &q) noexcept { ++q.x; });
  ecs::sweep_ticks(w, 9);
  w.set_change_tick(10);
  w.release_empty_chunks();
  REQUIRE(w.add_resource(ecs::resource_key{.value = 1}, std::int32_t{5}).has_value());
  CHECK(p.step(0) == 0);

  REQUIRE(w.destroy(*e).has_value());
  CHECK(p.step(1) == 0);
  CHECK_FALSE(w.destroy(*e).has_value()); // stale_entity
  CHECK_FALSE(w.add(*e, vel{}).has_value());
  CHECK_FALSE(w.set(*e, pos{}).has_value());
  CHECK(p.step(0) == 0);
  REQUIRE(w.create_with(pos{}, vel{}).has_value());
  CHECK(p.step(1) == 0); // {pos, vel} exists
}

TEST_CASE("RT-INV-CHG-04: through flushes the deltas equal the structural_log signals") {
  for (const std::uint64_t seed : {11ULL, 0xBEEFULL, 0x095ULL}) {
    CAPTURE(seed);
    rig r;
    splitmix rng{seed};
    std::vector<ecs::entity> pool;
    for (int i = 0; i < 16; ++i) {
      auto e = r.w->create_with(pos{.x = i});
      REQUIRE(e.has_value());
      pool.push_back(*e);
    }
    ecs::structural_log log;
    log.append(ecs::flush_report{}, *r.w); // announces the archetypes so far
    log.clear();
    probe p{.w = r.w.get()};
    p.reset();
    std::uint64_t structural = 0;
    for (int flush = 0; flush < 12; ++flush) {
      for (std::uint16_t system = 0; system < 2; ++system) {
        ecs::command_buffer &b = r.queue->open(key(system));
        for (int i = 0; i < 10; ++i) {
          const ecs::entity e = pool[rng.below(static_cast<std::uint32_t>(pool.size()))];
          switch (rng.below(6)) {
          case 0:
            static_cast<void>(b.spawn_with(pos{.x = i}, tag{.t = i}));
            break;
          case 1:
            b.destroy(e);
            break;
          case 2:
            b.add(e, vel{.v = i});
            break;
          case 3:
            b.add(e, tag{.t = i});
            break;
          case 4:
            b.remove<pos>(e);
            break;
          default:
            b.remove<vel>(e);
            break;
          }
        }
      }
      auto report = r.queue->flush(*r.w);
      REQUIRE(report.has_value());
      for (const auto &[pending, e] : report->resolved) {
        pool.push_back(e);
      }
      log.append(*report, *r.w);
      const auto signals = log.signals();
      const auto created = static_cast<std::uint64_t>(std::ranges::count(
          signals, ecs::change_signal_kind::archetype_created, &ecs::change_signal::kind));
      const std::uint64_t entity_signals = signals.size() - created;
      structural += entity_signals;
      CHECK(p.step(entity_signals) == created);
      log.clear();
    }
    CHECK(structural > 60);
  }
}

TEST_CASE("RT-INV-CHG-04: a set that adds moves the version by its component_added signal") {
  rig r;
  auto e = r.w->create_with(pos{.x = 1});
  REQUIRE(e.has_value());
  ecs::structural_log log;
  log.append(ecs::flush_report{}, *r.w);
  log.clear();
  probe p{.w = r.w.get()};
  p.reset();
  r.queue->open(key(0)).set(*e, vel{.v = 1}); // adds vel: {pos, vel} is new
  auto report = r.queue->flush(*r.w);
  REQUIRE(report.has_value());
  log.append(*report, *r.w);
  CHECK(p.step(1) == 1);
  // The new archetype, then the set's component_added (WO-109): one entity signal, one version.
  REQUIRE(log.signals().size() == 2);
  CHECK(log.signals()[0].kind == ecs::change_signal_kind::archetype_created);
  CHECK(log.signals()[1].kind == ecs::change_signal_kind::component_added);
  CHECK(log.signals()[1].target == *e);
}

TEST_CASE("RT-INV-CHG-04: restore_from moves the version and the generation") {
  rig source;
  REQUIRE(source.w->create_with(pos{.x = 1}, vel{.v = 1}).has_value());
  const ecs::world_image image = source.w->capture_image();
  rig target;
  REQUIRE(target.w->create_with(tag{.t = 1}).has_value());
  probe p{.w = target.w.get()};
  p.reset();
  REQUIRE(target.w->restore_from(image).has_value());
  CHECK(target.w->version() - p.version == 1);
  CHECK(target.w->archetype_generation() > p.generation);
}
