// RT-INV-QUERY-04 (SL-ARCH-003 ARCH3-R114, ARCH3-R112, ADR-0025): a query declares its terms at
// construction. Contradictory terms (with and without of one T, read and write of one T, without
// with any other term of its T, two argument terms of one T), a term given twice and more than 32
// terms are query_invalid; an unregistered type is component_unknown. The derived access set
// equals the terms (reads: read, optional<const T>, changed, added; writes: write, optional<T>;
// with and without in neither), and a structural call from inside for_each, for_each_chunk or
// par_for_each trips the captured Debug iteration guard and is refused.

#include "sl/core/assert.hpp"
#include "sl/core/error.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/query.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/memory.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"
#include "../../../engine/runtime/testing/world_gen.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace ecs = sl::runtime::ecs;
namespace jobs = sl::runtime::jobs;
namespace mem = sl::runtime::mem;
namespace rtt = sl::runtime::testing;

using rtt::gen_aligned;
using rtt::gen_health;
using rtt::gen_large;
using rtt::gen_padded;
using rtt::gen_position;
using rtt::gen_tag;
using rtt::gen_velocity;
using sl::errc;

#ifdef NDEBUG
constexpr bool debug_asserts = false;
#else
constexpr bool debug_asserts = true;
#endif

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
  mem::tracking_resource memory{"query.terms"};
  std::unique_ptr<ecs::world> w;
};

// Not registered with the generator's registry.
struct unregistered {
  std::uint32_t value = 0;
};

template <int N> struct numbered {
  std::uint8_t value = 0;
};

template <class... Terms> errc code_of(const ecs::component_registry &registry) {
  auto q = ecs::query<Terms...>::make(registry);
  return q ? errc::ok : q.error().code;
}

template <int... N> bool register_numbered(ecs::component_registry &r) {
  return (r.add<numbered<N>>("numbered." + std::to_string(N)).has_value() && ...);
}

template <int... N>
void numbered_queries(ecs::component_registry &r, std::integer_sequence<int, N...> /*seq*/,
                      errc &at32, errc &at33) {
  REQUIRE(register_numbered<N...>(r));
  REQUIRE(r.freeze().has_value());
  at32 = code_of<ecs::with<numbered<N>>...>(r);
  at33 = code_of<ecs::with<numbered<N>>..., ecs::with<numbered<0>>>(r);
}

std::vector<ecs::component_id> sorted(std::vector<ecs::component_id> v) {
  std::ranges::sort(v);
  return v;
}

} // namespace

TEST_CASE("RT-INV-QUERY-04: contradictory and duplicate terms are query_invalid") {
  const fixture f;
  const auto &r = f.registry;
  CHECK(code_of<ecs::with<gen_tag>, ecs::without<gen_tag>>(r) == errc::query_invalid);
  CHECK(code_of<ecs::without<gen_tag>, ecs::with<gen_tag>>(r) == errc::query_invalid);
  CHECK(code_of<ecs::read<gen_position>, ecs::write<gen_position>>(r) == errc::query_invalid);
  CHECK(code_of<ecs::write<gen_position>, ecs::read<gen_position>>(r) == errc::query_invalid);
  CHECK(code_of<ecs::read<gen_position>, ecs::read<gen_position>>(r) == errc::query_invalid);
  CHECK(code_of<ecs::write<gen_health>, ecs::write<gen_health>>(r) == errc::query_invalid);
  CHECK(code_of<ecs::with<gen_tag>, ecs::with<gen_tag>>(r) == errc::query_invalid);
  CHECK(code_of<ecs::without<gen_tag>, ecs::without<gen_tag>>(r) == errc::query_invalid);
  CHECK(code_of<ecs::changed<gen_health>, ecs::changed<gen_health>>(r) == errc::query_invalid);
  CHECK(code_of<ecs::read<gen_health>, ecs::without<gen_health>>(r) == errc::query_invalid);
  CHECK(code_of<ecs::write<gen_health>, ecs::without<gen_health>>(r) == errc::query_invalid);
  CHECK(code_of<ecs::changed<gen_health>, ecs::without<gen_health>>(r) == errc::query_invalid);
  CHECK(code_of<ecs::added<gen_health>, ecs::without<gen_health>>(r) == errc::query_invalid);
  CHECK(code_of<ecs::optional<gen_health>, ecs::without<gen_health>>(r) == errc::query_invalid);
  CHECK(code_of<ecs::optional<const gen_health>, ecs::read<gen_health>>(r) == errc::query_invalid);
  CHECK(code_of<ecs::optional<gen_health>, ecs::write<gen_health>>(r) == errc::query_invalid);
  CHECK(code_of<ecs::optional<gen_health>, ecs::optional<const gen_health>>(r) ==
        errc::query_invalid);
  CHECK(code_of<ecs::read<unregistered>>(r) == errc::component_unknown);
  // Not contradictory: a filter next to an argument term of the same T, and distinct components.
  CHECK(code_of<ecs::write<gen_health>, ecs::changed<gen_health>>(r) == errc::ok);
  CHECK(code_of<ecs::read<gen_health>, ecs::with<gen_health>>(r) == errc::ok);
  CHECK(code_of<ecs::changed<gen_health>, ecs::added<gen_health>>(r) == errc::ok);
  CHECK(code_of<ecs::read<gen_position>, ecs::write<gen_velocity>, ecs::without<gen_tag>>(r) ==
        errc::ok);
  CHECK(code_of<>(r) == errc::ok);
}

TEST_CASE("RT-INV-QUERY-04: more than 32 terms are query_invalid") {
  ecs::component_registry r;
  errc at32 = errc::internal;
  errc at33 = errc::internal;
  numbered_queries(r, std::make_integer_sequence<int, 32>{}, at32, at33);
  CHECK(at32 == errc::ok);
  CHECK(at33 == errc::query_invalid);
}

TEST_CASE("RT-INV-QUERY-04: the derived access set equals the terms") {
  const fixture f;
  auto q = ecs::query<ecs::read<gen_position>, ecs::write<gen_velocity>, ecs::with<gen_tag>,
                      ecs::without<gen_large>, ecs::optional<const gen_padded>,
                      ecs::optional<gen_aligned>, ecs::changed<gen_health>,
                      ecs::added<gen_position>>::make(f.registry);
  REQUIRE(q.has_value());
  const ecs::component_access &access = q->access();
  // position 0, velocity 1, padded 2, large 3, tag 4, tracked 5, aligned 6, health 7
  CHECK(access.reads == sorted({f.id(0), f.id(2), f.id(7)}));
  CHECK(access.writes == sorted({f.id(1), f.id(6)}));

  auto filters = ecs::query<ecs::with<gen_tag>, ecs::without<gen_large>>::make(f.registry);
  REQUIRE(filters.has_value());
  CHECK(filters->access().reads.empty());
  CHECK(filters->access().writes.empty());

  // A component written and filtered on is a write only.
  auto both = ecs::query<ecs::write<gen_health>, ecs::changed<gen_health>>::make(f.registry);
  REQUIRE(both.has_value());
  CHECK(both->access().reads.empty());
  CHECK(both->access().writes == std::vector<ecs::component_id>{f.id(7)});

  // The term order does not change the set.
  auto shuffled =
      ecs::query<ecs::added<gen_position>, ecs::optional<gen_aligned>, ecs::changed<gen_health>,
                 ecs::optional<const gen_padded>, ecs::without<gen_large>, ecs::with<gen_tag>,
                 ecs::write<gen_velocity>, ecs::read<gen_position>>::make(f.registry);
  REQUIRE(shuffled.has_value());
  CHECK(shuffled->access() == access);
}

TEST_CASE("RT-INV-QUERY-04: a structural call during iteration trips the guard") {
  fixture f;
  REQUIRE(f.w->create_with(gen_position{}, gen_health{.hp = 1}).has_value());
  REQUIRE(f.w->create_with(gen_position{}).has_value());
  auto q = ecs::query<ecs::read<gen_position>>::make(f.registry);
  REQUIRE(q.has_value());
  int asserts = 0;
  std::vector<errc> refused;
  {
    const sl::scoped_assert_handler handler([&asserts](const sl::assert_report &) { ++asserts; });
    q->for_each(*f.w, {}, [&](ecs::entity e, const gen_position &) noexcept {
      refused.push_back(f.w->create().error().code);
      refused.push_back(f.w->destroy(e).error().code);
    });
    q->for_each_chunk(*f.w, {},
                      [&](std::span<const ecs::entity>, std::span<const gen_position>) noexcept {
                        refused.push_back(f.w->create().error().code);
                      });
  }
  // Two rows in two archetypes: four calls from for_each, one per chunk from for_each_chunk.
  CHECK(refused == std::vector<errc>(6, errc::structural_change_forbidden));
  CHECK(f.w->stats().rows == 2);
  CHECK(f.w->create().has_value()); // the guard ended with the iteration
  if (!debug_asserts) {
    MESSAGE("skipped: SL_DEBUG_ASSERT is compiled out (NDEBUG)");
    CHECK(asserts == 0);
    return;
  }
  CHECK(asserts == 6);
}

TEST_CASE("RT-INV-QUERY-04: par_for_each holds the guard while its batches run") {
  fixture f;
  for (int i = 0; i < 3; ++i) {
    REQUIRE(f.w->create_with(gen_position{}).has_value());
  }
  rtt::scripted_platform plat;
  jobs::job_config c;
  c.workers = 0; // every batch runs on the owner thread inside the helping wait
  c.capacity = 256;
  auto js = jobs::job_system::start(plat.get(), std::move(c));
  REQUIRE(js.has_value());
  auto q = ecs::query<ecs::read<gen_position>>::make(f.registry);
  REQUIRE(q.has_value());
  int asserts = 0;
  std::vector<errc> refused;
  {
    const sl::scoped_assert_handler handler([&asserts](const sl::assert_report &) { ++asserts; });
    auto done = q->par_for_each(*f.w, **js, {},
                                [&](std::uint32_t, ecs::entity, const gen_position &) noexcept {
                                  refused.push_back(f.w->create().error().code);
                                });
    CHECK(done.has_value());
  }
  CHECK(refused == std::vector<errc>(3, errc::structural_change_forbidden));
  CHECK(f.w->create().has_value());
  CHECK(asserts == (debug_asserts ? 3 : 0));
  REQUIRE((*js)->stop().has_value());
}
