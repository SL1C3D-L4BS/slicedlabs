// RT-INV-CHG-03 (SL-ARCH-003 ARCH3-R129, ADR-0026): structural signals arrive in playback order
// and equal the flush_report history. Each flush of a command_queue (buffers opened out of key
// order) is appended to a structural_log; the signals of the batch equal, in order, the
// archetypes created since the previous append (creation order) followed by one signal per
// applied spawn, destroy, add and remove of the report (a set gives none), each naming the
// entity's archetype at the boundary as world::has finds it; the batches accumulate in flush
// order until clear(). Seeded command streams, the buffer open order, another world and
// restore_from are covered too.

#include "sl/core/error.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/ecs/change.hpp"
#include "sl/runtime/ecs/command_buffer.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/memory.hpp"
#include "sl/runtime/replay.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace {

namespace ecs = sl::runtime::ecs;
namespace mem = sl::runtime::mem;
namespace det = sl::runtime::det;

using kind = ecs::change_signal_kind;
using op_kind = ecs::flush_report::op_kind;

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
  mem::tracking_resource memory{"chg.signals"};
  mem::tracking_resource arena_memory{"chg.signals.arena"};
  std::unique_ptr<ecs::world> w;
  std::unique_ptr<mem::frame_arena> arena;
  std::unique_ptr<ecs::command_queue> queue;
  ecs::component_id pos_id;
  ecs::component_id vel_id;
  ecs::component_id tag_id;

  rig() {
    pos_id = registry.add<pos>("pos").value_or(ecs::component_id{});
    vel_id = registry.add<vel>("vel").value_or(ecs::component_id{});
    tag_id = registry.add<tag>("tag").value_or(ecs::component_id{});
    REQUIRE(registry.freeze().has_value());
    auto made = ecs::world::make(registry, memory);
    REQUIRE(made.has_value());
    w = std::move(*made);
    arena = std::make_unique<mem::frame_arena>(arena_memory, 0);
    queue = std::make_unique<ecs::command_queue>(registry, *arena);
  }

  ecs::flush_report flush() const {
    auto report = queue->flush(*w);
    REQUIRE(report.has_value());
    return report.value_or(ecs::flush_report{});
  }
};

det::ordered_key key(std::uint16_t system, std::uint32_t chunk) {
  return det::ordered_key{
      .tick = 1, .phase = 3, .system_index = system, .chunk_index = chunk, .local_seq = 0};
}

// The creation index of the archetype `e` is in at the boundary, found through world::has over
// every registered id; no_archetype for a dead entity.
std::uint32_t archetype_of(const ecs::world &w, ecs::entity e) {
  if (!w.is_alive(e)) {
    return ecs::no_archetype;
  }
  std::vector<ecs::component_id> ids;
  for (std::uint16_t i = 0; i < w.registry().count(); ++i) {
    if (w.has(e, ecs::component_id{.value = i})) {
      ids.push_back(ecs::component_id{.value = i});
    }
  }
  const auto archetypes = w.archetypes();
  const auto found = std::ranges::find_if(archetypes, [&ids](const ecs::archetype *a) {
    return std::ranges::equal(a->components(), ids);
  });
  if (found == archetypes.end()) {
    FAIL("no archetype holds the entity");
    return ecs::no_archetype;
  }
  return (*found)->creation_index();
}

// The signals a flush must give: the archetypes from `announced` on, then the report's entries.
std::vector<ecs::change_signal> expected(const ecs::flush_report &report, const ecs::world &w,
                                         std::uint32_t announced) {
  std::vector<ecs::change_signal> out;
  for (auto i = announced; i < static_cast<std::uint32_t>(w.archetypes().size()); ++i) {
    out.push_back({.kind = kind::archetype_created,
                   .target = ecs::entity{},
                   .component = ecs::component_id{},
                   .archetype = i});
  }
  for (const ecs::flush_report::op &op : report.applied) {
    kind k = kind::entity_created;
    switch (op.kind) {
    case op_kind::spawn:
      k = kind::entity_created;
      break;
    case op_kind::destroy:
      k = kind::entity_destroyed;
      break;
    case op_kind::add:
      k = kind::component_added;
      break;
    case op_kind::remove:
      k = kind::component_removed;
      break;
    case op_kind::set:
      continue;
    }
    out.push_back({.kind = k,
                   .target = op.target,
                   .component = op.component,
                   .archetype = archetype_of(w, op.target)});
  }
  return out;
}

std::vector<ecs::change_signal> tail(const ecs::structural_log &log, std::size_t from) {
  const auto all = log.signals();
  return {all.begin() + static_cast<std::ptrdiff_t>(from), all.end()};
}

} // namespace

TEST_CASE("RT-INV-CHG-03: the signals of a flush follow its playback order") {
  rig r;
  auto a = r.w->create_with(pos{.x = 1});
  auto b = r.w->create_with(pos{.x = 2}, vel{.v = 2});
  auto c = r.w->create_with(pos{.x = 3});
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  REQUIRE(c.has_value());
  ecs::structural_log log;

  // Opened out of key order: k2, k0, k1.
  ecs::command_buffer &k2 = r.queue->open(key(2, 0));
  ecs::command_buffer &k0 = r.queue->open(key(1, 0));
  ecs::command_buffer &k1 = r.queue->open(key(1, 1));
  k2.add(*c, vel{.v = 9}); // stale by then: skipped
  k2.destroy(*a);
  const ecs::pending_entity p = k0.spawn_with(pos{.x = 4}, tag{.t = 4});
  k0.add(*a, vel{.v = 1});
  k1.remove<pos>(*b);
  k1.set(*c, pos{.x = 30}); // a write: no signal
  k1.destroy(*c);
  k2.add(p, vel{.v = 5}); // after the spawn in key order
  const ecs::flush_report report = r.flush();
  REQUIRE(report.skipped_stale == 1);
  log.append(report, *r.w);
  const std::vector<ecs::change_signal> first = expected(report, *r.w, 0);
  CHECK(tail(log, 0) == first);

  // The same, spelled out: every archetype of the world first, then the entries in key order.
  const ecs::entity spawned = report.resolved.at(0).second;
  std::vector<kind> kinds;
  std::vector<ecs::entity> targets;
  std::uint32_t archetypes = 0;
  for (const ecs::change_signal &s : log.signals()) {
    if (s.kind == kind::archetype_created) {
      CHECK(s.archetype == archetypes);
      ++archetypes;
      CHECK(kinds.empty());
      continue;
    }
    kinds.push_back(s.kind);
    targets.push_back(s.target);
  }
  CHECK(archetypes == r.w->archetypes().size());
  CHECK(kinds == std::vector<kind>{kind::entity_created, kind::component_added,
                                   kind::component_removed, kind::entity_destroyed,
                                   kind::entity_destroyed, kind::component_added});
  CHECK(targets == std::vector<ecs::entity>{spawned, *a, *b, *c, *a, spawned});
  CHECK(log.signals()[archetypes + 1].component == r.vel_id);
  CHECK(log.signals()[archetypes + 1].archetype == ecs::no_archetype); // a is destroyed later
  CHECK(log.signals()[archetypes + 2].archetype == archetype_of(*r.w, *b));

  // A second flush appends after the first; only its new archetypes are announced.
  const auto announced = static_cast<std::uint32_t>(r.w->archetypes().size());
  ecs::command_buffer &next = r.queue->open(key(0, 0));
  const ecs::pending_entity bare = next.spawn();
  next.remove<tag>(spawned);
  next.add(bare, tag{.t = 1});
  const ecs::flush_report second_report = r.flush();
  log.append(second_report, *r.w);
  const std::vector<ecs::change_signal> second = expected(second_report, *r.w, announced);
  CHECK(tail(log, 0).size() == first.size() + second.size());
  CHECK(tail(log, first.size()) == second);
  CHECK(second.front().kind == kind::archetype_created);

  // clear() drops the signals; a flush of sets only gives none.
  log.clear();
  CHECK(log.signals().empty());
  r.queue->open(key(0, 0)).set(*b, vel{.v = 7});
  log.append(r.flush(), *r.w);
  CHECK(log.signals().empty());
  const auto before_third = static_cast<std::uint32_t>(r.w->archetypes().size());
  r.queue->open(key(0, 0)).add(*b, tag{.t = 2}); // {vel, tag} is new
  const ecs::flush_report third_report = r.flush();
  log.append(third_report, *r.w);
  CHECK(tail(log, 0) == expected(third_report, *r.w, before_third));
  CHECK(log.signals().size() == 2);
}

TEST_CASE("RT-INV-CHG-03: seeded command streams give the flush_report history") {
  for (const std::uint64_t seed : {7ULL, 0x5EEDULL, 0xC0DEULL, 0x095ULL}) {
    CAPTURE(seed);
    rig r;
    splitmix rng{seed};
    ecs::structural_log log;
    std::uint64_t generation = r.w->archetype_generation(); // the first append announces all
    std::vector<ecs::entity> pool;
    for (int i = 0; i < 20; ++i) {
      auto e = r.w->create_with(pos{.x = i});
      REQUIRE(e.has_value());
      pool.push_back(*e);
    }
    std::vector<ecs::change_signal> history;
    std::uint32_t announced = 0;
    for (int flush = 0; flush < 10; ++flush) {
      std::vector<std::uint16_t> systems{0, 1, 2};
      for (std::size_t i = systems.size() - 1; i > 0; --i) {
        std::swap(systems[i], systems[rng.below(static_cast<std::uint32_t>(i + 1))]);
      }
      for (const std::uint16_t system : systems) {
        ecs::command_buffer &b = r.queue->open(key(system, 0));
        for (int i = 0; i < 8; ++i) {
          const ecs::entity e = pool[rng.below(static_cast<std::uint32_t>(pool.size()))];
          switch (rng.below(8)) {
          case 0:
            static_cast<void>(b.spawn_with(pos{.x = i}, vel{.v = i}));
            break;
          case 1:
            b.destroy(e);
            break;
          case 2:
            b.add(e, vel{.v = 1});
            break;
          case 3:
            b.add(e, tag{.t = 1});
            break;
          case 4:
            b.remove<vel>(e);
            break;
          case 5:
            b.remove<pos>(e);
            break;
          case 6:
            b.set(e, pos{.x = 5});
            break;
          default:
            b.add(b.spawn(), tag{.t = 2});
            break;
          }
        }
      }
      const ecs::flush_report report = r.flush();
      for (const auto &[pending, e] : report.resolved) {
        pool.push_back(e);
      }
      const std::size_t before = log.signals().size();
      log.append(report, *r.w);
      const std::vector<ecs::change_signal> batch = expected(report, *r.w, announced);
      CHECK(tail(log, before) == batch);
      const auto created =
          std::ranges::count(batch, kind::archetype_created, &ecs::change_signal::kind);
      CHECK(static_cast<std::uint64_t>(created) == r.w->archetype_generation() - generation);
      generation = r.w->archetype_generation();
      announced = static_cast<std::uint32_t>(r.w->archetypes().size());
      history.insert(history.end(), batch.begin(), batch.end());
    }
    CHECK(tail(log, 0) == history);
    // Every archetype a signal names was announced before it.
    std::set<std::uint32_t> known;
    std::uint64_t unknown = 0;
    for (const ecs::change_signal &s : history) {
      if (s.kind == kind::archetype_created) {
        known.insert(s.archetype);
      } else if (s.archetype != ecs::no_archetype && !known.contains(s.archetype)) {
        ++unknown;
      }
    }
    CHECK(unknown == 0);
    CHECK(history.size() > 60);
  }
}

TEST_CASE("RT-INV-CHG-03: the signals do not depend on the buffer open order") {
  const auto run = [](bool reversed) {
    rig r;
    std::vector<ecs::entity> es;
    for (int i = 0; i < 6; ++i) {
      auto e = r.w->create_with(pos{.x = i});
      REQUIRE(e.has_value());
      es.push_back(*e);
    }
    std::array<ecs::command_buffer *, 3> b{};
    const std::array<std::uint16_t, 3> order =
        reversed ? std::array<std::uint16_t, 3>{2, 1, 0} : std::array<std::uint16_t, 3>{0, 1, 2};
    for (const std::uint16_t at : order) {
      b.at(at) = &r.queue->open(key(at, 0));
    }
    b[2]->destroy(es[0]);
    b[0]->add(es[0], vel{.v = 1});
    b[1]->remove<pos>(es[1]);
    static_cast<void>(b[1]->spawn_with(tag{.t = 3}));
    b[2]->add(es[2], tag{.t = 1});
    b[0]->destroy(es[3]);
    ecs::structural_log log;
    log.append(r.flush(), *r.w);
    return tail(log, 0);
  };
  const std::vector<ecs::change_signal> forward = run(false);
  CHECK(forward.size() > 6);
  CHECK(run(true) == forward);
}

TEST_CASE("RT-INV-CHG-03: another world or a restored world is announced in full") {
  rig r;
  REQUIRE(r.w->create_with(pos{.x = 1}).has_value());
  REQUIRE(r.w->create_with(pos{.x = 1}, vel{.v = 1}).has_value());
  auto other_made = ecs::world::make(r.registry, r.memory);
  REQUIRE(other_made.has_value());
  ecs::world &other = **other_made;
  REQUIRE(other.create_with(tag{.t = 1}).has_value());

  ecs::structural_log log;
  log.append(ecs::flush_report{}, *r.w);
  CHECK(log.signals().size() == r.w->archetypes().size());
  log.clear();
  log.append(ecs::flush_report{}, *r.w);
  CHECK(log.signals().empty());
  log.append(ecs::flush_report{}, other);
  CHECK(tail(log, 0) == expected(ecs::flush_report{}, other, 0));
  log.clear();

  // restore_from replaces every archetype: all of them are announced again.
  const ecs::world_image image = other.capture_image();
  REQUIRE(r.w->restore_from(image).has_value());
  log.append(ecs::flush_report{}, *r.w);
  CHECK(tail(log, 0) == expected(ecs::flush_report{}, *r.w, 0));
  CHECK(log.signals().size() == r.w->archetypes().size());
}
