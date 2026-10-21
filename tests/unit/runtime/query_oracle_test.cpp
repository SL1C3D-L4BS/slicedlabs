// RT-INV-QUERY-01 (SL-ARCH-003 ARCH3-R114, ARCH3-R111, ADR-0025): query results equal a brute
// force filter. Seeded worlds (testing::op_generator churn checked against the world_oracle) are
// queried with term combinations including optional and without; the entities a query visits,
// in order, equal the oracle's modelled rows (archetype creation, then row) of every archetype
// holding all required and none of the excluded components, every read value equals the oracle's
// value, an optional pointer is set exactly when the entity has the component, and for_each_chunk
// visits the same rows as for_each.

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/query.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/memory.hpp"

#include "../../../engine/runtime/testing/world_gen.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

namespace {

namespace ecs = sl::runtime::ecs;
namespace mem = sl::runtime::mem;
namespace rtt = sl::runtime::testing;

// Kinds of world_gen: 0 position, 1 velocity, 2 padded, 3 large, 4 tag, 5 tracked, 6 aligned,
// 7 health.
constexpr std::uint32_t bit(std::uint32_t kind) { return 1U << kind; }

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
  mem::tracking_resource memory{"query.oracle"};
  std::unique_ptr<rtt::world_oracle> oracle; // outlives the world (gen_tracked counts into it)
  std::unique_ptr<ecs::world> w;
};

void churn(fixture &f, std::uint64_t seed, int ops) {
  rtt::op_generator gen(seed);
  int accepted = 0;
  for (int i = 0; i < ops; ++i) {
    accepted += rtt::apply(gen.next(), *f.w, *f.oracle).has_value() ? 1 : 0;
  }
  REQUIRE(accepted > ops / 2);
  REQUIRE(f.oracle->compare(*f.w).empty());
}

// The brute force filter: the oracle's rows of every modelled archetype with all of `need` and
// none of `refuse`, in creation order.
std::vector<ecs::entity> expected(const fixture &f, std::uint32_t need, std::uint32_t refuse) {
  std::vector<ecs::entity> out;
  for (const auto &m : f.oracle->order()) {
    const std::uint32_t mask = std::accumulate(
        m.components.begin(), m.components.end(), 0U,
        [&f](std::uint32_t acc, ecs::component_id id) { return acc | bit(f.ids.kind_of(id)); });
    if ((mask & need) == need && (mask & refuse) == 0) {
      out.insert(out.end(), m.rows.begin(), m.rows.end());
    }
  }
  return out;
}

// Counts the visits whose value or presence disagrees with the oracle.
struct checker {
  const fixture *f = nullptr;
  std::uint64_t wrong = 0;

  void value(ecs::entity e, std::uint32_t kind, const void *p) noexcept {
    const auto &v = f->oracle->entities().find(e)->second.values.at(kind);
    if (!v.has_value() || !rtt::value_equals(kind, p, *v)) {
      ++wrong;
    }
  }
  void maybe(ecs::entity e, std::uint32_t kind, const void *p) noexcept {
    const auto &v = f->oracle->entities().find(e)->second.values.at(kind);
    if (v.has_value() != (p != nullptr)) {
      ++wrong;
    } else if (p != nullptr) {
      value(e, kind, p);
    }
  }
};

constexpr std::initializer_list<std::uint64_t> seeds{1ULL, 0x5EEDULL, 0xC0FFEEULL, 0x093ULL};

} // namespace

TEST_CASE("RT-INV-QUERY-01: read and write terms visit exactly the brute-force rows") {
  for (const std::uint64_t seed : seeds) {
    fixture f;
    churn(f, seed, 900);
    auto q = ecs::query<ecs::read<rtt::gen_position>>::make(f.registry);
    REQUIRE(q.has_value());
    checker c{.f = &f};
    std::vector<ecs::entity> seen;
    q->for_each(*f.w, {}, [&](ecs::entity e, const rtt::gen_position &p) noexcept {
      seen.push_back(e);
      c.value(e, 0, &p);
    });
    CHECK(seen == expected(f, bit(0), 0));
    CHECK(c.wrong == 0);

    auto rw =
        ecs::query<ecs::read<rtt::gen_position>, ecs::write<rtt::gen_velocity>>::make(f.registry);
    REQUIRE(rw.has_value());
    seen.clear();
    rw->for_each(*f.w, {},
                 [&](ecs::entity e, const rtt::gen_position &p, rtt::gen_velocity &v) noexcept {
                   seen.push_back(e);
                   c.value(e, 0, &p);
                   c.value(e, 1, &v);
                 });
    CHECK(seen == expected(f, bit(0) | bit(1), 0));
    CHECK(c.wrong == 0);
  }
}

TEST_CASE("RT-INV-QUERY-01: without and optional terms match the brute-force filter") {
  for (const std::uint64_t seed : seeds) {
    fixture f;
    churn(f, seed, 900);
    checker c{.f = &f};
    std::vector<ecs::entity> seen;

    auto health =
        ecs::query<ecs::read<rtt::gen_health>, ecs::without<rtt::gen_tag>>::make(f.registry);
    REQUIRE(health.has_value());
    health->for_each(*f.w, {}, [&](ecs::entity e, const rtt::gen_health &h) noexcept {
      seen.push_back(e);
      c.value(e, 7, &h);
    });
    CHECK(seen == expected(f, bit(7), bit(4)));

    auto padded = ecs::query<ecs::with<rtt::gen_padded>, ecs::optional<const rtt::gen_position>,
                             ecs::optional<rtt::gen_health>>::make(f.registry);
    REQUIRE(padded.has_value());
    seen.clear();
    padded->for_each(*f.w, {},
                     [&](ecs::entity e, const rtt::gen_position *p, rtt::gen_health *h) noexcept {
                       seen.push_back(e);
                       c.maybe(e, 0, p);
                       c.maybe(e, 7, h);
                     });
    CHECK(seen == expected(f, bit(2), 0));

    auto none =
        ecs::query<ecs::without<rtt::gen_velocity>, ecs::without<rtt::gen_large>>::make(f.registry);
    REQUIRE(none.has_value());
    seen.clear();
    none->for_each(*f.w, {}, [&](ecs::entity e) noexcept { seen.push_back(e); });
    CHECK(seen == expected(f, 0, bit(1) | bit(3)));

    auto mixed = ecs::query<ecs::read<rtt::gen_aligned>, ecs::with<rtt::gen_tag>,
                            ecs::without<rtt::gen_padded>,
                            ecs::optional<const rtt::gen_velocity>>::make(f.registry);
    REQUIRE(mixed.has_value());
    seen.clear();
    mixed->for_each(
        *f.w, {},
        [&](ecs::entity e, const rtt::gen_aligned &a, const rtt::gen_velocity *v) noexcept {
          seen.push_back(e);
          c.value(e, 6, &a);
          c.maybe(e, 1, v);
        });
    CHECK(seen == expected(f, bit(6) | bit(4), bit(2)));

    auto all = ecs::query<>::make(f.registry);
    REQUIRE(all.has_value());
    seen.clear();
    all->for_each(*f.w, {}, [&](ecs::entity e) noexcept { seen.push_back(e); });
    CHECK(seen == expected(f, 0, 0));
    CHECK(seen.size() == f.w->entities().alive());
    CHECK(c.wrong == 0);
  }
}

TEST_CASE("RT-INV-QUERY-01: for_each_chunk visits the rows for_each visits") {
  for (const std::uint64_t seed : seeds) {
    fixture f;
    churn(f, seed, 900);
    auto q = ecs::query<ecs::read<rtt::gen_health>, ecs::optional<const rtt::gen_position>,
                        ecs::without<rtt::gen_tag>>::make(f.registry);
    REQUIRE(q.has_value());
    checker c{.f = &f};
    std::vector<ecs::entity> rows;
    q->for_each(*f.w, {},
                [&](ecs::entity e, const rtt::gen_health &, const rtt::gen_position *) noexcept {
                  rows.push_back(e);
                });
    std::vector<ecs::entity> runs;
    std::uint64_t bad_spans = 0;
    q->for_each_chunk(*f.w, {},
                      [&](std::span<const ecs::entity> es, std::span<const rtt::gen_health> hs,
                          std::span<const rtt::gen_position> ps) noexcept {
                        if (hs.size() != es.size() || (!ps.empty() && ps.size() != es.size())) {
                          ++bad_spans;
                          return;
                        }
                        for (std::size_t i = 0; i < es.size(); ++i) {
                          runs.push_back(es[i]);
                          c.value(es[i], 7, &hs[i]);
                          c.maybe(es[i], 0, ps.empty() ? nullptr : &ps[i]);
                        }
                      });
    CHECK(bad_spans == 0);
    CHECK(runs == rows);
    CHECK(rows == expected(f, bit(7), bit(4)));
    CHECK(c.wrong == 0);
  }
}
