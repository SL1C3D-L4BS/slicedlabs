// RT-INV-CHG-05 (SL-ARCH-003 ARCH3-R131, ADR-0025): after a fast-forwarded tick wrap changed<T>
// and added<T> stay correct. A clock fast-forwarded in steps of 2^28 ticks over more than three
// u32 cycles, with sweep_ticks and last_run_ticks::sweep whenever sweep_due, drives writes,
// creations, re-adds and readers that run at different rates (some idle for more than 2^31
// ticks); a 64-bit oracle predicts every read: a row is selected when its stamp is later than the
// reader's last run, both raised to the newest sweep bound (now - change_tick_max_age at that
// sweep). A short run across the u32 wrap, the clamp bounds, sweep_due and an aged stamp that the
// plain compare would take for fresh are covered too.

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/change.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/query.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/memory.hpp"

#include "../../../engine/runtime/testing/sim_fixture.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace {

namespace ecs = sl::runtime::ecs;
namespace mem = sl::runtime::mem;

struct pos {
  std::int32_t x = 0;
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
  mem::tracking_resource memory{"chg.wrap"};
  std::unique_ptr<ecs::world> w;
  ecs::component_id pos_id;

  rig() {
    pos_id = registry.add<pos>("pos").value_or(ecs::component_id{});
    REQUIRE(registry.freeze().has_value());
    auto made = ecs::world::make(registry, memory);
    REQUIRE(made.has_value());
    w = std::move(*made);
  }
};

constexpr std::int64_t never = -1;
constexpr std::int64_t no_bound = std::numeric_limits<std::int64_t>::min();

// One reader: a changed<pos> or added<pos> query, its rate and its last run (64-bit).
struct reader {
  bool added = false;
  std::uint32_t every = 1;
  std::int64_t last_run = never;
};

// The 64-bit model: per holder of pos, the clocks of its last changed and added stamps.
struct model {
  // {changed, added}
  std::map<ecs::entity, std::pair<std::int64_t, std::int64_t>> stamps;
  // The newest sweep bound.
  std::int64_t bound = no_bound;

  [[nodiscard]] std::int64_t raised(std::int64_t t) const { return std::max(t, bound); }
  [[nodiscard]] std::vector<ecs::entity> want(const reader &r) const {
    std::vector<ecs::entity> out;
    for (const auto &[e, s] : stamps) {
      const std::int64_t stamp = r.added ? s.second : s.first;
      if (raised(stamp) > raised(r.last_run)) {
        out.push_back(e);
      }
    }
    return out;
  }
};

std::vector<ecs::entity> read(ecs::world &w, const ecs::component_registry &registry, bool added,
                              std::uint32_t since, std::uint32_t now) {
  std::vector<ecs::entity> out;
  const ecs::iter_args args{.since = since, .write_tick = now};
  if (added) {
    auto q = ecs::query<ecs::added<pos>>::make(registry);
    REQUIRE(q.has_value());
    q->for_each(w, args, [&out](ecs::entity e) noexcept { out.push_back(e); });
  } else {
    auto q = ecs::query<ecs::changed<pos>>::make(registry);
    REQUIRE(q.has_value());
    q->for_each(w, args, [&out](ecs::entity e) noexcept { out.push_back(e); });
  }
  std::ranges::sort(out);
  return out;
}

// The changed tick of component `id` of `e` (the entity must be alive and hold it).
std::uint32_t changed_tick_of(const ecs::world &w, ecs::entity e, ecs::component_id id) {
  for (const ecs::archetype *a : w.archetypes()) {
    for (std::uint32_t c = 0; c < a->chunk_count(); ++c) {
      const auto es = a->entities(c);
      const auto at = std::ranges::find(es, e);
      if (at != es.end() && a->contains(id)) {
        return a->changed_ticks(id, c)[static_cast<std::size_t>(at - es.begin())];
      }
    }
  }
  FAIL("entity not found");
  return 0;
}

// Runs `steps` steps of `stride` ticks from `start` (64-bit), sweeping when due.
struct wrap_run {
  rig r;
  splitmix rng;
  model m;
  ecs::last_run_ticks ticks;
  std::vector<ecs::entity> alive;
  std::array<reader, 6> readers{
      {{.added = false, .every = 1, .last_run = never},
       {.added = false, .every = 5, .last_run = never},
       {.added = false, .every = 13, .last_run = never}, // idle for 13 x 2^28 > 2^31 ticks
       {.added = true, .every = 3, .last_run = never},
       {.added = true, .every = 17, .last_run = never},
       {.added = false, .every = 1000, .last_run = never}}}; // only the final read
  std::uint64_t mismatches = 0;
  std::uint64_t selected = 0;
  std::uint64_t sweeps = 0;
  std::int64_t last_sweep = 0;

  explicit wrap_run(std::uint64_t seed) : rng{seed} {}

  void sweep_if_due(std::int64_t now) {
    if (!ecs::sweep_due(static_cast<std::uint32_t>(last_sweep), static_cast<std::uint32_t>(now))) {
      return;
    }
    ecs::sweep_ticks(*r.w, static_cast<std::uint32_t>(now));
    ticks.sweep(static_cast<std::uint32_t>(now));
    m.bound = now - std::int64_t{ecs::change_tick_max_age};
    last_sweep = now;
    ++sweeps;
  }

  void churn(std::int64_t now) {
    const auto t = static_cast<std::uint32_t>(now);
    r.w->set_change_tick(t);
    for (int i = 0; i < 3; ++i) {
      auto e = r.w->create_with(pos{.x = i});
      REQUIRE(e.has_value());
      alive.push_back(*e);
      m.stamps[*e] = {now, now};
    }
    if (alive.size() > 4) {
      const std::size_t at = rng.below(static_cast<std::uint32_t>(alive.size()));
      REQUIRE(r.w->destroy(alive[at]).has_value());
      m.stamps.erase(alive[at]);
      alive.erase(alive.begin() + static_cast<std::ptrdiff_t>(at));
      const ecs::entity readd = alive[rng.below(static_cast<std::uint32_t>(alive.size()))];
      REQUIRE(r.w->remove<pos>(readd).has_value());
      REQUIRE(r.w->add(readd, pos{.x = 9}).has_value());
      m.stamps[readd] = {now, now};
    }
    for (const ecs::entity e : alive) {
      if (rng.below(5) == 0) {
        REQUIRE(ecs::mark_changed<pos>(*r.w, e, t).has_value());
        m.stamps[e].first = now;
      }
    }
  }

  void read_due(std::int64_t now, std::uint64_t step, bool all) {
    for (std::uint32_t k = 0; k < readers.size(); ++k) {
      reader &rd = readers.at(k);
      if (!all && step % rd.every != 0) {
        continue;
      }
      const std::vector<ecs::entity> got =
          read(*r.w, r.registry, rd.added, ticks.since(k), static_cast<std::uint32_t>(now));
      mismatches += got == m.want(rd) ? 0U : 1U;
      selected += got.size();
      ticks.record(k, static_cast<std::uint32_t>(now));
      rd.last_run = now;
    }
  }

  void run(std::int64_t start, std::int64_t stride, std::uint64_t steps) {
    last_sweep = start;
    for (std::uint64_t step = 1; step <= steps; ++step) {
      const std::int64_t now = start + (static_cast<std::int64_t>(step) * stride);
      sweep_if_due(now);
      churn(now);
      read_due(now, step, false);
    }
    read_due(start + (static_cast<std::int64_t>(steps + 1) * stride), steps + 1, true);
  }
};

} // namespace

TEST_CASE("RT-INV-CHG-05: changed and added stay correct over three fast-forwarded wraps") {
  for (const std::uint64_t seed : {5ULL, 0x2A2AULL, 0x095ULL}) {
    CAPTURE(seed);
    wrap_run run(seed);
    constexpr auto stride = static_cast<std::int64_t>(1ULL << 28U);
    run.run(0, stride, 52); // 52 x 2^28 = 3.25 x 2^32 ticks
    CHECK(run.mismatches == 0);
    CHECK(run.sweeps == 13);
    CHECK(run.selected > 200);
  }
}

TEST_CASE("RT-INV-CHG-05: a short run across the u32 wrap") {
  wrap_run run(0x77ULL);
  constexpr std::int64_t start = 0xFFFFFE00LL; // 512 ticks before the wrap
  // The clock was fast-forwarded: sweep there first, as every boundary 2^30 ticks apart would.
  ecs::sweep_ticks(*run.r.w, static_cast<std::uint32_t>(start));
  run.ticks.sweep(static_cast<std::uint32_t>(start));
  run.m.bound = start - std::int64_t{ecs::change_tick_max_age};
  run.run(start, 1, 1024);
  CHECK(run.mismatches == 0);
  CHECK(run.sweeps == 0);
  CHECK(run.selected > 1000);
}

TEST_CASE("RT-INV-CHG-05: the sweep clamps aged stamps the plain compare takes for fresh") {
  rig r;
  r.w->set_change_tick(5);
  auto old = r.w->create_with(pos{.x = 1});
  REQUIRE(old.has_value());
  const std::uint32_t now = 5U + (1U << 31U) + 10U; // the stamp is 2^31 + 10 old
  // Without a sweep the stamp aliases into the future of since = now - 1.
  CHECK(ecs::tick_after(5, now - 1));
  CHECK(read(*r.w, r.registry, false, now - 1, now).size() == 1);
  CHECK(read(*r.w, r.registry, true, now - 1, now).size() == 1);
  ecs::sweep_ticks(*r.w, now);
  CHECK(changed_tick_of(*r.w, *old, r.pos_id) == now - ecs::change_tick_max_age);
  CHECK(read(*r.w, r.registry, false, now - 1, now).empty());
  CHECK(read(*r.w, r.registry, true, now - 1, now).empty());
  // A fresh write is seen.
  REQUIRE(ecs::mark_changed<pos>(*r.w, *old, now).has_value());
  CHECK(read(*r.w, r.registry, false, now - 1, now).size() == 1);
}

TEST_CASE("RT-INV-CHG-05: clamp bounds, sweep_due and last_run_ticks::sweep") {
  rig r;
  const std::uint32_t now = 0x90000020U; // above 2^31: tick 5 and the initial floor are old
  r.w->set_change_tick(now - ecs::change_tick_max_age);
  auto at_bound = r.w->create_with(pos{.x = 1});
  r.w->set_change_tick(now - ecs::change_tick_max_age - 1);
  auto past_bound = r.w->create_with(pos{.x = 2});
  r.w->set_change_tick(now - 3);
  auto young = r.w->create_with(pos{.x = 3});
  REQUIRE(at_bound.has_value());
  REQUIRE(past_bound.has_value());
  REQUIRE(young.has_value());
  ecs::sweep_ticks(*r.w, now);
  CHECK(changed_tick_of(*r.w, *at_bound, r.pos_id) == now - ecs::change_tick_max_age);
  CHECK(changed_tick_of(*r.w, *past_bound, r.pos_id) == now - ecs::change_tick_max_age);
  CHECK(changed_tick_of(*r.w, *young, r.pos_id) == now - 3);

  CHECK(ecs::change_tick_sweep_every == 1U << 30U);
  CHECK(ecs::change_tick_max_age == (1U << 30U) - 1U);
  CHECK_FALSE(ecs::sweep_due(0, (1U << 30U) - 1U));
  CHECK(ecs::sweep_due(0, 1U << 30U));
  CHECK(ecs::sweep_due(0xFFFFFFF0U, (1U << 30U) - 16U)); // across the wrap
  CHECK_FALSE(ecs::sweep_due(0xFFFFFFF0U, 5));

  ecs::last_run_ticks ticks;
  ticks.record(1, 5);
  ticks.record(2, now - 3);
  ticks.sweep(now);
  CHECK(ticks.since(1) == now - ecs::change_tick_max_age);
  CHECK(ticks.since(2) == now - 3);
  CHECK(ticks.floor() == now - ecs::change_tick_max_age);
  CHECK(ticks.since(9) == ticks.floor());
}

TEST_CASE("RT-INV-CHG-05: the sweep clamps a scheduler last-run tick") {
  struct noop_body {
    void operator()(sl::runtime::ecs::system_context & /*ctx*/) const noexcept {}
  };
  namespace ecs = sl::runtime::ecs;
  auto made = sl::runtime::testing::sched_rig::make(1);
  REQUIRE(made.has_value());
  sl::runtime::testing::sched_rig &r = **made;
  ecs::access_set access;
  access.components.writes.push_back(r.ids.ids.at(0));
  REQUIRE(r.systems
              ->add(ecs::system_desc{.name = "wrap.writer",
                                     .ph = sl::runtime::phase::tick,
                                     .access = access,
                                     .before = {},
                                     .after = {},
                                     .fn = noop_body{},
                                     .chunked = false,
                                     .queries = {}})
              .has_value());
  REQUIRE(r.build().has_value());
  bool ran = false;
  for (int f = 0; f < 20 && !ran; ++f) {
    auto st = r.step();
    REQUIRE(st.has_value());
    ran = st->ticks_run > 0;
  }
  REQUIRE(ran);
  ecs::last_run_ticks &runs = r.systems->last_runs();
  const std::uint32_t key =
      ecs::last_run_key(static_cast<std::uint8_t>(sl::runtime::phase::tick), 0);
  const std::uint32_t last = runs.since(key);
  CHECK(last != runs.floor());
  const std::uint32_t now = last + ecs::change_tick_sweep_every + 5U;
  runs.sweep(now);
  CHECK(runs.since(key) == now - ecs::change_tick_max_age);
  CHECK(runs.floor() == now - ecs::change_tick_max_age);
}
