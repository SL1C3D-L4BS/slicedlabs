// RT-INV-CHG-01 (SL-ARCH-003 ARCH3-R128, ARCH3-R117, ADR-0025): changed<T> yields exactly the
// rows written since the reading system's last run and each change is seen once per reader across
// frames. Seeded frames run writers (a write<T> query, mark_changed, a reader that writes) and
// readers (changed<T> queries with since = last_run_ticks::since) in a seeded order, each run at
// its own change tick, with structural churn between frames; an oracle of 64-bit write clocks
// predicts the rows of every reader run, every row a reader sees is a change newer than the last
// one it saw of that entity, and a final read sees the latest write of every entity. Also
// last_run_ticks and mark_changed on their own, and mark_changed from parallel batches.

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/change.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/query.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/memory.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace {

namespace ecs = sl::runtime::ecs;
namespace mem = sl::runtime::mem;
namespace jobs = sl::runtime::jobs;
namespace rtt = sl::runtime::testing;

struct pos {
  std::int32_t x = 0;
};
struct vel {
  std::int32_t v = 0;
};
struct unregistered {
  std::int32_t u = 0;
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
  mem::tracking_resource memory{"chg.changed"};
  std::unique_ptr<ecs::world> w;

  rig() {
    REQUIRE(registry.add<pos>("pos").has_value());
    REQUIRE(registry.add<vel>("vel").has_value());
    REQUIRE(registry.freeze().has_value());
    auto made = ecs::world::make(registry, memory);
    REQUIRE(made.has_value());
    w = std::move(*made);
  }
};

using std_reader = ecs::query<ecs::read<pos>, ecs::changed<pos>>;
using moving_reader = ecs::query<ecs::changed<pos>, ecs::with<vel>>;
using still_reader = ecs::query<ecs::changed<pos>, ecs::without<vel>>;
using writing_reader = ecs::query<ecs::write<pos>, ecs::changed<pos>>;
using writer = ecs::query<ecs::write<pos>, ecs::with<vel>>;

enum class filter : std::uint8_t { any, moving, still };

// Reader 1 selects moving rows, reader 2 still rows, the others every holder of pos.
filter filter_of(std::uint32_t k) {
  if (k == 1) {
    return filter::moving;
  }
  if (k == 2) {
    return filter::still;
  }
  return filter::any;
}

// The oracle and the bookkeeping of one seeded run.
struct sim {
  rig r;
  splitmix rng;
  std::uint64_t clock = 0;
  std::vector<ecs::entity> alive;
  std::map<ecs::entity, std::uint64_t> written; // holders of pos -> clock of the last pos stamp
  ecs::last_run_ticks ticks;
  // Per reader, -1: never ran.
  std::vector<std::int64_t> last_run = std::vector<std::int64_t>(4, -1);
  // Per reader: entity -> clock seen.
  std::vector<std::map<ecs::entity, std::uint64_t>> seen =
      std::vector<std::map<ecs::entity, std::uint64_t>>(4);
  std::uint64_t mismatches = 0;
  std::uint64_t repeats = 0;
  std::uint64_t reads = 0;

  std_reader q0 = std_reader::make(r.registry).value();
  moving_reader q1 = moving_reader::make(r.registry).value();
  still_reader q2 = still_reader::make(r.registry).value();
  writing_reader q3 = writing_reader::make(r.registry).value();
  writer qw = writer::make(r.registry).value();

  explicit sim(std::uint64_t seed) : rng{seed} {}

  std::uint32_t tick() {
    ++clock;
    r.w->set_change_tick(static_cast<std::uint32_t>(clock));
    return static_cast<std::uint32_t>(clock);
  }

  [[nodiscard]] bool keeps(filter f, ecs::entity e) const {
    const bool moving = r.w->get<vel>(e).has_value();
    return f == filter::any || (f == filter::moving) == moving;
  }

  void churn() {
    tick();
    for (int i = 0; i < 12; ++i) {
      const std::uint32_t what = rng.below(6);
      if (what == 0 || alive.empty()) {
        auto e = rng.below(2) == 0 ? r.w->create_with(pos{.x = 1})
                                   : r.w->create_with(pos{.x = 1}, vel{.v = 1});
        REQUIRE(e.has_value());
        alive.push_back(*e);
        written[*e] = clock;
        continue;
      }
      const std::size_t at = rng.below(static_cast<std::uint32_t>(alive.size()));
      const ecs::entity e = alive[at];
      const bool has_pos = written.contains(e);
      const bool has_vel = r.w->get<vel>(e).has_value();
      if (what == 1) {
        REQUIRE(r.w->destroy(e).has_value());
        alive.erase(alive.begin() + static_cast<std::ptrdiff_t>(at));
        written.erase(e);
      } else if (what == 2 && !has_vel) {
        REQUIRE(r.w->add(e, vel{.v = 2}).has_value()); // moves the row; pos keeps its ticks
      } else if (what == 3 && has_vel) {
        REQUIRE(r.w->remove<vel>(e).has_value());
      } else if (what == 4 && has_pos) {
        REQUIRE(r.w->remove<pos>(e).has_value());
        written.erase(e);
      } else if (what == 5 && !has_pos) {
        REQUIRE(r.w->add(e, pos{.x = 3}).has_value());
        written[e] = clock;
      }
    }
  }

  void write_all() {
    const std::uint32_t now = tick();
    qw.for_each(*r.w, {.since = 0, .write_tick = now}, [](ecs::entity, pos &p) noexcept { ++p.x; });
    for (auto &[e, at] : written) {
      if (keeps(filter::moving, e)) {
        at = clock;
      }
    }
  }

  void mark_some() {
    const std::uint32_t now = tick();
    for (auto &[e, at] : written) {
      if (rng.below(4) == 0) {
        REQUIRE(ecs::mark_changed<pos>(*r.w, e, now).has_value());
        at = clock;
      }
    }
  }

  // Runs reader `k` and checks its rows against the oracle.
  void read(std::uint32_t k) {
    const std::uint32_t now = tick();
    const ecs::iter_args args{.since = ticks.since(k), .write_tick = now};
    std::vector<ecs::entity> got;
    const auto note = [&got](ecs::entity e) noexcept { got.push_back(e); };
    if (k == 0) {
      q0.for_each(*r.w, args, [&note](ecs::entity e, const pos &) noexcept { note(e); });
    } else if (k == 1) {
      q1.for_each(*r.w, args, [&note](ecs::entity e) noexcept { note(e); });
    } else if (k == 2) {
      q2.for_each(*r.w, args, [&note](ecs::entity e) noexcept { note(e); });
    } else {
      q3.for_each(*r.w, args, [&note](ecs::entity e, pos &p) noexcept {
        ++p.x;
        note(e);
      });
    }
    const filter f = filter_of(k);
    std::vector<ecs::entity> want;
    for (const auto &[e, at] : written) {
      if (keeps(f, e) && std::cmp_greater(at, last_run[k])) {
        want.push_back(e);
      }
    }
    std::ranges::sort(got);
    mismatches += got == want ? 0U : 1U;
    for (const ecs::entity e : got) {
      ++reads;
      auto &last = seen[k][e];
      repeats += written[e] > last ? 0U : 1U;
      last = written[e];
      if (k == 3) {
        written[e] = clock; // the reader's own write
        last = clock;
      }
    }
    ticks.record(k, now);
    last_run[k] = static_cast<std::int64_t>(clock);
  }
};

constexpr std::array<std::uint64_t, 4> seeds{1ULL, 0x095ULL, 0xC4A9ULL, 0xFEEDULL};

sl::errc code_of(const sl::result<void> &r) { return r ? sl::errc::ok : r.error().code; }

} // namespace

TEST_CASE("RT-INV-CHG-01: changed<T> yields exactly the rows written since each reader's last "
          "run") {
  for (const std::uint64_t seed : seeds) {
    CAPTURE(seed);
    sim s(seed);
    for (int frame = 0; frame < 60; ++frame) {
      s.churn();
      std::vector<std::uint32_t> order{0, 1, 2, 3, 4, 5};
      for (std::size_t i = order.size() - 1; i > 0; --i) {
        std::swap(order[i], order[s.rng.below(static_cast<std::uint32_t>(i + 1))]);
      }
      for (const std::uint32_t step : order) {
        if (step == 4) {
          s.write_all();
        } else if (step == 5) {
          s.mark_some();
        } else if (s.rng.below(3) != 0) { // a reader skips about a third of the frames
          s.read(step);
        }
      }
    }
    CHECK(s.mismatches == 0);
    CHECK(s.repeats == 0);
    CHECK(s.reads > 250);

    // A last read of each read-only reader sees the latest write of every entity it selects.
    for (std::uint32_t k = 0; k < 3; ++k) {
      s.read(k);
    }
    CHECK(s.mismatches == 0);
    std::uint64_t missed = 0;
    for (const auto &[e, at] : s.written) {
      for (std::uint32_t k = 0; k < 3; ++k) {
        const filter f = filter_of(k);
        if (s.keeps(f, e) && s.seen[k][e] != at) {
          ++missed;
        }
      }
    }
    CHECK(missed == 0);
  }
}

TEST_CASE("RT-INV-CHG-01: a write after the reader in a frame is seen at its next run, once") {
  rig r;
  auto e = r.w->create_with(pos{.x = 1});
  REQUIRE(e.has_value());
  ecs::last_run_ticks ticks;
  auto reader = std_reader::make(r.registry);
  REQUIRE(reader.has_value());
  const auto count = [&](std::uint32_t now) {
    std::uint32_t n = 0;
    reader->for_each(*r.w, {.since = ticks.since(7), .write_tick = now},
                     [&n](ecs::entity, const pos &) noexcept { ++n; });
    ticks.record(7, now);
    return n;
  };
  CHECK(count(1) == 1); // the first run sees the creation (stamped at tick 0)
  CHECK(count(2) == 0);
  REQUIRE(ecs::mark_changed<pos>(*r.w, *e, 3).has_value()); // a writer after the reader
  CHECK(count(4) == 1);
  CHECK(count(5) == 0);
  CHECK(count(6) == 0);
}

TEST_CASE("RT-INV-CHG-01: last_run_ticks keeps one tick per system") {
  ecs::last_run_ticks ticks;
  CHECK(ticks.floor() == 0xFFFFFFFFU);
  CHECK(ticks.since(0) == 0xFFFFFFFFU);
  CHECK(ecs::tick_after(0, ticks.since(0)));
  CHECK(ticks.entries().empty());
  ticks.record(ecs::last_run_key(3, 2), 40);
  ticks.record(ecs::last_run_key(1, 0), 10);
  ticks.record(ecs::last_run_key(1, 0), 12);
  ticks.record(ecs::last_run_slots, 99); // out of range: ignored
  CHECK(ecs::last_run_key(7, 255) == ecs::last_run_slots - 1);
  CHECK(ticks.since(ecs::last_run_key(1, 0)) == 12);
  CHECK(ticks.since(ecs::last_run_key(3, 2)) == 40);
  CHECK(ticks.since(ecs::last_run_slots) == ticks.floor());
  CHECK(ticks.since(5) == ticks.floor());
  const std::vector<ecs::last_run_ticks::entry> saved = ticks.entries();
  CHECK(saved == std::vector<ecs::last_run_ticks::entry>{{.system = 256, .tick = 12},
                                                         {.system = 770, .tick = 40}});

  ecs::last_run_ticks other;
  other.record(5, 77);
  other.restore(saved, 3);
  CHECK(other.entries() == saved);
  CHECK(other.since(5) == 3); // not in the restored entries: the floor
  CHECK(other.floor() == 3);
}

TEST_CASE("RT-INV-CHG-01: mark_changed stamps exactly one row and refuses bad handles") {
  rig r;
  std::vector<ecs::entity> es;
  for (int i = 0; i < 2500; ++i) {
    auto e = r.w->create_with(pos{.x = i});
    REQUIRE(e.has_value());
    es.push_back(*e);
  }
  auto bare = r.w->create();
  REQUIRE(bare.has_value());
  REQUIRE(r.w->destroy(es[3]).has_value());
  CHECK(code_of(ecs::mark_changed<pos>(*r.w, es[3], 5)) == sl::errc::stale_entity);
  CHECK(code_of(ecs::mark_changed<pos>(*r.w, *bare, 5)) == sl::errc::component_missing);
  CHECK(code_of(ecs::mark_changed<unregistered>(*r.w, es[0], 5)) == sl::errc::component_unknown);

  // Rows in the first, a middle and the last chunk.
  for (const std::size_t at : {std::size_t{0}, std::size_t{1500}, std::size_t{2499}}) {
    CAPTURE(at);
    const auto tick = static_cast<std::uint32_t>(10 + at);
    REQUIRE(ecs::mark_changed<pos>(*r.w, es[at], tick).has_value());
    auto changed = ecs::query<ecs::changed<pos>>::make(r.registry);
    auto added = ecs::query<ecs::added<pos>>::make(r.registry);
    REQUIRE(changed.has_value());
    REQUIRE(added.has_value());
    std::vector<ecs::entity> got;
    changed->for_each(*r.w, {.since = tick - 1, .write_tick = tick},
                      [&got](ecs::entity e) noexcept { got.push_back(e); });
    CHECK(got == std::vector<ecs::entity>{es[at]});
    std::uint32_t fresh = 0;
    added->for_each(*r.w, {.since = tick - 1, .write_tick = tick},
                    [&fresh](ecs::entity) noexcept { ++fresh; });
    CHECK(fresh == 0);
  }
}

TEST_CASE("RT-INV-CHG-01: mark_changed from parallel batches marks exactly the chosen rows") {
  rig r;
  for (int i = 0; i < 12000; ++i) {
    REQUIRE(r.w->create_with(pos{.x = i}).has_value());
  }
  rtt::scripted_platform plat;
  jobs::job_config c;
  c.workers = 4;
  c.capacity = 256;
  auto js = jobs::job_system::start(plat.get(), std::move(c));
  REQUIRE(js.has_value());
  auto all = ecs::query<ecs::read<pos>>::make(r.registry);
  REQUIRE(all.has_value());
  std::atomic<std::uint32_t> failed{0};
  ecs::world &w = *r.w;
  auto ran = all->par_for_each(w, **js, {.since = 0, .write_tick = 0},
                               [&w, &failed](std::uint32_t, ecs::entity e, const pos &p) noexcept {
                                 if (p.x % 3 == 0 && !ecs::mark_changed<pos>(w, e, 50)) {
                                   failed.fetch_add(1);
                                 }
                               });
  REQUIRE(ran.has_value());
  CHECK(failed.load() == 0);
  REQUIRE((*js)->stop().has_value());
  auto changed = ecs::query<ecs::read<pos>, ecs::changed<pos>>::make(r.registry);
  REQUIRE(changed.has_value());
  std::uint32_t n = 0;
  std::uint32_t wrong = 0;
  changed->for_each(*r.w, {.since = 49, .write_tick = 51},
                    [&n, &wrong](ecs::entity, const pos &p) noexcept {
                      ++n;
                      wrong += p.x % 3 == 0 ? 0 : 1;
                    });
  CHECK(n == 4000);
  CHECK(wrong == 0);
}
