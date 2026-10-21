// RT-INV-QUERY-05 (SL-ARCH-003 ARCH3-R117, ADR-0025): a write<T> term stamps the changed tick of
// T on every visited row (optional<T> where T is present; read and optional<const T> never), and
// changed<T>(since) and added<T>(since) select exactly the rows whose tick is later than since
// under the wrap rule (std::int32_t(tick - since) > 0), with every filter of a query applying
// together; rows a filter skips are not stamped, in for_each and for_each_chunk alike.

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/query.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/memory.hpp"

#include "../../../engine/runtime/testing/world_gen.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace {

namespace ecs = sl::runtime::ecs;
namespace mem = sl::runtime::mem;
namespace rtt = sl::runtime::testing;

using rtt::gen_health;
using rtt::gen_position;
using rtt::gen_velocity;

struct fixture {
  fixture() {
    auto got = rtt::register_components(registry);
    REQUIRE(got.has_value());
    ids = *got;
    REQUIRE(registry.freeze().has_value());
    auto made = ecs::world::make(registry, memory);
    REQUIRE(made.has_value());
    w = std::move(*made);
  }
  [[nodiscard]] ecs::component_id id(std::uint32_t kind) const { return ids.ids.at(kind); }

  ecs::component_registry registry;
  rtt::gen_ids ids;
  mem::tracking_resource memory{"query.change"};
  std::unique_ptr<ecs::world> w;
};

enum class tick_kind : std::uint8_t { added, changed };

// The added or changed tick of component `id` of every entity that has it.
std::map<ecs::entity, std::uint32_t> ticks(const ecs::world &w, ecs::component_id id,
                                           tick_kind kind) {
  std::map<ecs::entity, std::uint32_t> out;
  for (const ecs::archetype *a : w.archetypes()) {
    if (!a->contains(id)) {
      continue;
    }
    for (std::uint32_t c = 0; c < a->chunk_count(); ++c) {
      const std::span<const ecs::entity> es = a->entities(c);
      const std::span<std::uint32_t> ts =
          kind == tick_kind::added ? a->added_ticks(id, c) : a->changed_ticks(id, c);
      for (std::size_t r = 0; r < es.size(); ++r) {
        out[es[r]] = ts[r];
      }
    }
  }
  return out;
}

// The entities of `all` whose tick is later than `since`, in the map's (handle) order.
std::vector<ecs::entity> later(const std::map<ecs::entity, std::uint32_t> &all,
                               std::uint32_t since) {
  std::vector<ecs::entity> out;
  for (const auto &[e, t] : all) {
    if (ecs::tick_later(t, since)) {
      out.push_back(e);
    }
  }
  return out;
}

std::vector<ecs::entity> sorted(std::vector<ecs::entity> v) {
  std::ranges::sort(v);
  return v;
}

} // namespace

TEST_CASE("RT-INV-QUERY-05: write terms stamp the changed tick of each visited row") {
  fixture f;
  f.w->set_change_tick(1);
  std::vector<ecs::entity> plain;
  std::vector<ecs::entity> healthy;
  for (int i = 0; i < 1500; ++i) { // two chunks of the plain archetype
    auto e = f.w->create_with(gen_position{}, gen_velocity{});
    REQUIRE(e.has_value());
    plain.push_back(*e);
  }
  for (int i = 0; i < 40; ++i) {
    auto e = f.w->create_with(gen_position{}, gen_velocity{}, gen_health{});
    REQUIRE(e.has_value());
    healthy.push_back(*e);
  }
  auto q = ecs::query<ecs::read<gen_position>, ecs::write<gen_velocity>,
                      ecs::optional<gen_health>>::make(f.registry);
  REQUIRE(q.has_value());
  std::uint64_t visits = 0;
  q->for_each(*f.w, {.since = 0, .write_tick = 7},
              [&visits](ecs::entity, const gen_position &, gen_velocity &v, gen_health *) noexcept {
                v.dx += 1.0;
                ++visits;
              });
  CHECK(visits == 1540);
  for (const auto &[e, t] : ticks(*f.w, f.id(1), tick_kind::changed)) {
    CHECK(t == 7);
  }
  for (const auto &[e, t] : ticks(*f.w, f.id(7), tick_kind::changed)) {
    CHECK(t == 7); // optional<T> is a write where T is present
  }
  for (const auto &[e, t] : ticks(*f.w, f.id(0), tick_kind::changed)) {
    CHECK(t == 1); // read terms stamp nothing
  }
  for (const auto &[e, t] : ticks(*f.w, f.id(1), tick_kind::added)) {
    CHECK(t == 1); // added ticks are never stamped by a query
  }

  // optional<const T> reads: no stamp.
  auto reader = ecs::query<ecs::optional<const gen_health>>::make(f.registry);
  REQUIRE(reader.has_value());
  reader->for_each(*f.w, {.since = 0, .write_tick = 9},
                   [](ecs::entity, const gen_health *) noexcept {});
  for (const auto &[e, t] : ticks(*f.w, f.id(7), tick_kind::changed)) {
    CHECK(t == 7);
  }

  // for_each_chunk stamps the rows of every run.
  auto chunked = ecs::query<ecs::write<gen_velocity>>::make(f.registry);
  REQUIRE(chunked.has_value());
  std::uint64_t rows = 0;
  chunked->for_each_chunk(*f.w, {.since = 0, .write_tick = 11},
                          [&rows](std::span<const ecs::entity> es,
                                  std::span<gen_velocity>) noexcept { rows += es.size(); });
  CHECK(rows == 1540);
  for (const auto &[e, t] : ticks(*f.w, f.id(1), tick_kind::changed)) {
    CHECK(t == 11);
  }
}

TEST_CASE("RT-INV-QUERY-05: changed and added select exactly the rows with a later tick") {
  fixture f;
  std::vector<ecs::entity> es;
  for (std::uint32_t t = 1; t <= 30; ++t) {
    f.w->set_change_tick(t);
    for (int k = 0; k < 40; ++k) {
      auto e = f.w->create_with(gen_position{}, gen_velocity{});
      REQUIRE(e.has_value());
      es.push_back(*e);
    }
  }
  // In-place runtime writes at tick 50 on every seventh entity: changed moves, added stays.
  f.w->set_change_tick(50);
  for (std::size_t i = 0; i < es.size(); i += 7) {
    REQUIRE(f.w->set(es[i], gen_position{.x = 1.0F}).has_value());
  }
  auto changed = ecs::query<ecs::changed<gen_position>>::make(f.registry);
  auto added = ecs::query<ecs::added<gen_position>>::make(f.registry);
  REQUIRE(changed.has_value());
  REQUIRE(added.has_value());
  const auto changed_ticks = ticks(*f.w, f.id(0), tick_kind::changed);
  const auto added_ticks = ticks(*f.w, f.id(0), tick_kind::added);
  for (const std::uint32_t since : {0U, 1U, 5U, 17U, 29U, 30U, 49U, 50U, 51U}) {
    std::vector<ecs::entity> seen;
    changed->for_each(*f.w, {.since = since},
                      [&seen](ecs::entity e) noexcept { seen.push_back(e); });
    CHECK(sorted(seen) == later(changed_ticks, since));
    seen.clear();
    added->for_each(*f.w, {.since = since}, [&seen](ecs::entity e) noexcept { seen.push_back(e); });
    CHECK(sorted(seen) == later(added_ticks, since));
  }
  // Both filters together: added after 20 and changed after 40.
  auto both = ecs::query<ecs::added<gen_position>, ecs::changed<gen_position>>::make(f.registry);
  REQUIRE(both.has_value());
  std::vector<ecs::entity> seen;
  both->for_each(*f.w, {.since = 20}, [&seen](ecs::entity e) noexcept { seen.push_back(e); });
  std::vector<ecs::entity> want;
  for (const auto &[e, t] : added_ticks) {
    if (t > 20 && changed_ticks.at(e) > 20) {
      want.push_back(e);
    }
  }
  CHECK(sorted(seen) == want);
}

TEST_CASE("RT-INV-QUERY-05: rows a filter skips are not stamped") {
  fixture f;
  std::vector<ecs::entity> es;
  for (std::uint32_t t = 1; t <= 20; ++t) {
    f.w->set_change_tick(t);
    auto e = f.w->create_with(gen_position{}, gen_velocity{});
    REQUIRE(e.has_value());
    es.push_back(*e);
  }
  auto q = ecs::query<ecs::write<gen_velocity>, ecs::changed<gen_position>>::make(f.registry);
  REQUIRE(q.has_value());
  q->for_each(*f.w, {.since = 10, .write_tick = 40}, [](ecs::entity, gen_velocity &) noexcept {});
  const auto velocity = ticks(*f.w, f.id(1), tick_kind::changed);
  for (std::uint32_t t = 1; t <= 20; ++t) {
    CHECK(velocity.at(es[t - 1]) == (t > 10 ? 40U : t));
  }
  // The chunk form stamps only the runs it passes; the runs hold exactly the selected rows.
  std::vector<ecs::entity> runs;
  q->for_each_chunk(*f.w, {.since = 15, .write_tick = 60},
                    [&runs](std::span<const ecs::entity> run, std::span<gen_velocity> vs) noexcept {
                      if (vs.size() == run.size()) {
                        runs.insert(runs.end(), run.begin(), run.end());
                      }
                    });
  CHECK(runs == std::vector<ecs::entity>(es.begin() + 15, es.end()));
  const auto after = ticks(*f.w, f.id(1), tick_kind::changed);
  for (std::uint32_t t = 1; t <= 20; ++t) {
    CHECK(after.at(es[t - 1]) == (t > 15 ? 60U : velocity.at(es[t - 1])));
  }
  // A write term with a filter on the same component: the filter reads the tick before the stamp.
  auto self = ecs::query<ecs::write<gen_velocity>, ecs::changed<gen_velocity>>::make(f.registry);
  REQUIRE(self.has_value());
  std::uint32_t count = 0;
  self->for_each(*f.w, {.since = 50, .write_tick = 70},
                 [&count](ecs::entity, gen_velocity &) noexcept { ++count; });
  CHECK(count == 5);
}

TEST_CASE("RT-INV-QUERY-05: ticks compare under the wrap rule") {
  CHECK(ecs::tick_later(5, 0));
  CHECK_FALSE(ecs::tick_later(0, 0));
  CHECK_FALSE(ecs::tick_later(4, 5));
  CHECK(ecs::tick_later(5, 0xFFFFFFF8U));
  CHECK_FALSE(ecs::tick_later(0xFFFFFFF0U, 0xFFFFFFF8U));
  CHECK(ecs::tick_later(0x7FFFFFFFU, 0));
  CHECK_FALSE(ecs::tick_later(0x80000000U, 0));

  fixture f;
  f.w->set_change_tick(0xFFFFFFF0U);
  auto old = f.w->create_with(gen_position{});
  f.w->set_change_tick(5); // the tick counter wrapped
  auto fresh = f.w->create_with(gen_position{});
  REQUIRE(old.has_value());
  REQUIRE(fresh.has_value());
  auto q = ecs::query<ecs::added<gen_position>>::make(f.registry);
  REQUIRE(q.has_value());
  std::vector<ecs::entity> seen;
  q->for_each(*f.w, {.since = 0xFFFFFFF8U}, [&seen](ecs::entity e) noexcept { seen.push_back(e); });
  CHECK(seen == std::vector<ecs::entity>{*fresh});
}
