// RT-INV-CHG-02 (SL-ARCH-003 ARCH3-R127, ARCH3-R117, ADR-0025): added<T> includes created and
// component-added rows only. Creation (create_with and a command spawn_with), add and a set that
// adds stamp the added tick; a set that replaces, a write<T> term and mark_changed stamp only the
// changed tick; moving a row (an add or remove of another component) keeps its added tick; a
// remove then add stamps it again; a document add stamps added but not changed. A seeded oracle of
// creations and adds across frames predicts every added<T> reader run.

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
#include <map>
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
  mem::tracking_resource memory{"chg.added"};
  std::unique_ptr<ecs::world> w;

  rig() {
    REQUIRE(registry.add<pos>("pos").has_value());
    REQUIRE(registry.add<vel>("vel").has_value());
    REQUIRE(registry.freeze().has_value());
    auto made = ecs::world::make(registry, memory);
    REQUIRE(made.has_value());
    w = std::move(*made);
  }

  // The entities selected by `Q` with since = tick - 1, sorted.
  template <class Q> std::vector<ecs::entity> select(std::uint32_t tick) {
    auto q = Q::make(registry);
    REQUIRE(q.has_value());
    std::vector<ecs::entity> out;
    q->for_each(*w, {.since = tick - 1, .write_tick = tick},
                [&out](ecs::entity e) noexcept { out.push_back(e); });
    std::ranges::sort(out);
    return out;
  }
};

using added_pos = ecs::query<ecs::added<pos>>;
using changed_pos = ecs::query<ecs::changed<pos>>;

std::vector<ecs::entity> sorted(std::vector<ecs::entity> v) {
  std::ranges::sort(v);
  return v;
}

} // namespace

TEST_CASE("RT-INV-CHG-02: creation, add and a set that adds are added; writes are not") {
  rig r;
  mem::tracking_resource arena_memory{"chg.added.arena"};
  mem::frame_arena arena(arena_memory, 0);
  ecs::command_queue queue(r.registry, arena);

  r.w->set_change_tick(10);
  auto created = r.w->create_with(pos{.x = 1});
  auto bare = r.w->create_with(vel{.v = 1});
  auto other = r.w->create_with(vel{.v = 2});
  REQUIRE(created.has_value());
  REQUIRE(bare.has_value());
  REQUIRE(other.has_value());
  ecs::command_buffer &b = queue.open(det::ordered_key{});
  static_cast<void>(b.spawn_with(pos{.x = 2}));
  auto report = queue.flush(*r.w);
  REQUIRE(report.has_value());
  REQUIRE(report->resolved.size() == 1);
  const ecs::entity spawned = report->resolved[0].second;
  CHECK(r.select<added_pos>(10) == sorted({*created, spawned}));
  CHECK(r.select<changed_pos>(10) == sorted({*created, spawned}));

  r.w->set_change_tick(20);
  REQUIRE(r.w->add(*bare, pos{.x = 3}).has_value());    // add
  REQUIRE(r.w->set(*other, pos{.x = 4}).has_value());   // a set that adds
  REQUIRE(r.w->set(*created, pos{.x = 5}).has_value()); // a set that replaces
  REQUIRE(ecs::mark_changed<pos>(*r.w, spawned, 20).has_value());
  CHECK(r.select<added_pos>(20) == sorted({*bare, *other}));
  CHECK(r.select<changed_pos>(20) == sorted({*created, spawned, *bare, *other}));

  // A write<T> term stamps changed only.
  auto writer = ecs::query<ecs::write<pos>>::make(r.registry);
  REQUIRE(writer.has_value());
  writer->for_each(*r.w, {.since = 0, .write_tick = 25},
                   [](ecs::entity, pos &p) noexcept { ++p.x; });
  CHECK(r.select<added_pos>(25).empty());
  CHECK(r.select<changed_pos>(25).size() == 4);

  // Moving a row keeps its added tick.
  r.w->set_change_tick(30);
  REQUIRE(r.w->add(*created, vel{.v = 3}).has_value());
  REQUIRE(r.w->remove<vel>(*bare).has_value());
  CHECK(r.select<added_pos>(30).empty());
  CHECK(r.select<added_pos>(20) == sorted({*bare, *other})); // bare moved, kept its tick

  // Remove then add stamps again.
  r.w->set_change_tick(40);
  REQUIRE(r.w->remove<pos>(*created).has_value());
  REQUIRE(r.w->add(*created, pos{.x = 6}).has_value());
  CHECK(r.select<added_pos>(40) == std::vector<ecs::entity>{*created});

  // A document add is added but not changed.
  r.w->set_change_tick(50);
  auto doc = r.w->create_with(vel{.v = 4});
  REQUIRE(doc.has_value());
  REQUIRE(r.w->set(*doc, pos{.x = 7}, ecs::write_source::document).has_value());
  CHECK(r.select<added_pos>(50) == std::vector<ecs::entity>{*doc});
  CHECK(r.select<changed_pos>(50).empty());
}

TEST_CASE("RT-INV-CHG-02: added<T> matches a seeded oracle of creations and adds") {
  for (const std::uint64_t seed : {3ULL, 0x5EEDULL, 0xADDULL}) {
    CAPTURE(seed);
    rig r;
    splitmix rng{seed};
    ecs::last_run_ticks ticks;
    auto reader = ecs::query<ecs::read<pos>, ecs::added<pos>>::make(r.registry);
    auto writer = ecs::query<ecs::write<pos>, ecs::with<vel>>::make(r.registry);
    REQUIRE(reader.has_value());
    REQUIRE(writer.has_value());
    std::vector<ecs::entity> alive;
    std::map<ecs::entity, std::uint64_t> added; // holders of pos -> clock of the add
    std::int64_t last_run = -1;
    std::uint64_t clock = 0;
    std::uint64_t mismatches = 0;
    std::uint64_t seen = 0;
    for (int frame = 0; frame < 80; ++frame) {
      ++clock;
      const auto now = static_cast<std::uint32_t>(clock);
      r.w->set_change_tick(now);
      for (int i = 0; i < 10; ++i) {
        const std::uint32_t what = rng.below(8);
        if (what == 0 || alive.empty()) {
          const bool with_pos = rng.below(2) == 0;
          auto e = with_pos ? r.w->create_with(pos{}, vel{}) : r.w->create_with(vel{});
          REQUIRE(e.has_value());
          alive.push_back(*e);
          if (with_pos) {
            added[*e] = clock;
          }
          continue;
        }
        const std::size_t at = rng.below(static_cast<std::uint32_t>(alive.size()));
        const ecs::entity e = alive[at];
        const bool has_pos = added.contains(e);
        if (what == 1) {
          REQUIRE(r.w->destroy(e).has_value());
          alive.erase(alive.begin() + static_cast<std::ptrdiff_t>(at));
          added.erase(e);
        } else if (what == 2 && !has_pos) {
          REQUIRE(r.w->add(e, pos{}).has_value());
          added[e] = clock;
        } else if (what == 3 && !has_pos) {
          REQUIRE(r.w->set(e, pos{}).has_value());
          added[e] = clock;
        } else if (what == 4 && has_pos) {
          REQUIRE(r.w->remove<pos>(e).has_value());
          added.erase(e);
        } else if (what == 5 && has_pos) {
          REQUIRE(r.w->set(e, pos{.x = 9}).has_value()); // replaces: not added
        } else if (what == 6 && has_pos) {
          REQUIRE(ecs::mark_changed<pos>(*r.w, e, now).has_value());
        } else if (what == 7) {
          const bool moving = r.w->get<vel>(e).has_value();
          REQUIRE((moving ? r.w->remove<vel>(e) : r.w->add(e, vel{})).has_value());
        }
      }
      if (rng.below(2) == 0) {
        writer->for_each(*r.w, {.since = 0, .write_tick = now},
                         [](ecs::entity, pos &p) noexcept { ++p.x; });
      }
      if (rng.below(3) == 0) {
        continue; // the reader skips this frame
      }
      std::vector<ecs::entity> got;
      reader->for_each(*r.w, {.since = ticks.since(0), .write_tick = now},
                       [&got](ecs::entity e, const pos &) noexcept { got.push_back(e); });
      ticks.record(0, now);
      std::vector<ecs::entity> want;
      for (const auto &[e, at] : added) {
        if (std::cmp_greater(at, last_run)) {
          want.push_back(e);
        }
      }
      last_run = static_cast<std::int64_t>(clock);
      std::ranges::sort(got);
      mismatches += got == want ? 0U : 1U;
      seen += got.size();
    }
    CHECK(mismatches == 0);
    CHECK(seen > 50);
  }
}
