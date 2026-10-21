// RT-INV-SCHED-01 (SL-ARCH-003 ARCH3-R122): no two conflicting systems overlap in time. 1000
// seeded schedules (scaled by SL_RUNTIME_STRESS_SCALE), each 2 to 14 systems over two phases
// with random component and resource reads and writes, some exclusive, some after-constraints on
// earlier systems, on 1, 2 or 4 workers, a quarter of them serial_canonical. Every body records
// its interval as two ticks of one shared sequence (start and end, with a few yields between);
// over three frames, for every pair of systems of a phase that conflict by the declared access
// (computed here, independently), the intervals of each tick are disjoint and ordered by
// system_index; exclusive systems ran on the owner thread.

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/frame.hpp"

#include "../../../engine/runtime/testing/sim_fixture.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace ecs = sl::runtime::ecs;
namespace rtt = sl::runtime::testing;

class splitmix {
public:
  explicit splitmix(std::uint64_t seed) noexcept : state_(seed) {}
  std::uint64_t next() noexcept {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
  }
  // In [0, n).
  std::uint32_t below(std::uint32_t n) noexcept { return static_cast<std::uint32_t>(next() % n); }

private:
  std::uint64_t state_;
};

double stress_scale() {
  const char *text = std::getenv("SL_RUNTIME_STRESS_SCALE");
  if (text == nullptr) {
    return 1.0;
  }
  char *end = nullptr;
  const double v = std::strtod(text, &end);
  return end != text && v > 0.0 ? v : 1.0;
}

std::uint32_t scaled(std::uint32_t full) {
  const double n = static_cast<double>(full) * stress_scale();
  return n < 1.0 ? 1U : static_cast<std::uint32_t>(n);
}

constexpr std::uint32_t frames_per_schedule = 3;
constexpr std::uint32_t max_systems = 14;

struct interval {
  std::uint64_t tick = 0;
  std::uint64_t start = 0;
  std::uint64_t end = 0;
  bool on_owner = false;
};

struct trace {
  std::atomic<std::uint64_t> seq{0};
  std::thread::id owner = std::this_thread::get_id();
  // One list per system, written only by that system (which never overlaps itself).
  std::array<std::vector<interval>, max_systems> runs;
};

struct timed_body {
  trace *t = nullptr;
  std::uint32_t self = 0;

  // cppcheck-suppress constParameterReference ; the system body signature takes system_context &
  void operator()(ecs::system_context &ctx) const noexcept {
    interval i;
    i.tick = ctx.tick();
    i.on_owner = std::this_thread::get_id() == t->owner;
    i.start = t->seq.fetch_add(1);
    for (int k = 0; k < 8; ++k) {
      std::this_thread::yield();
    }
    i.end = t->seq.fetch_add(1);
    t->runs.at(self).push_back(i);
  }
};

struct planned {
  std::string name;
  rt::phase ph = rt::phase::tick;
  ecs::access_set access;
  std::vector<std::string> after;
};

// A random subset of `pool` (each element with probability 1/4).
template <class T> std::vector<T> pick(splitmix &rng, const std::vector<T> &pool) {
  std::vector<T> out;
  for (const T &v : pool) {
    if (rng.below(4) == 0) {
      out.push_back(v);
    }
  }
  return out;
}

bool meets(const std::vector<ecs::component_id> &a, const std::vector<ecs::component_id> &b) {
  return std::ranges::any_of(
      a, [&b](ecs::component_id c) { return std::ranges::find(b, c) != b.end(); });
}

bool meets(const std::vector<ecs::resource_key> &a, const std::vector<ecs::resource_key> &b) {
  return std::ranges::any_of(
      a, [&b](ecs::resource_key k) { return std::ranges::find(b, k) != b.end(); });
}

// The conflict rule, from the declared sets as planned (before add normalises them).
bool conflicts(const ecs::access_set &x, const ecs::access_set &y) {
  if (x.exclusive || y.exclusive) {
    return true;
  }
  const auto into = [](const ecs::access_set &w, const ecs::access_set &o) {
    return meets(w.components.writes, o.components.reads) ||
           meets(w.components.writes, o.components.writes) ||
           meets(w.resource_writes, o.resource_reads) ||
           meets(w.resource_writes, o.resource_writes);
  };
  return into(x, y) || into(y, x);
}

std::vector<planned> plan(splitmix &rng, const rtt::gen_ids &ids) {
  const std::vector<ecs::component_id> components(ids.ids.begin(), ids.ids.begin() + 5);
  const std::vector<ecs::resource_key> resources{ecs::resource_key{1}, ecs::resource_key{2}};
  const std::uint32_t n = 2 + rng.below(max_systems - 1);
  std::vector<planned> out;
  for (std::uint32_t i = 0; i < n; ++i) {
    planned p;
    p.name = "s" + std::to_string(i);
    p.ph = rng.below(2) == 0 ? rt::phase::tick : rt::phase::post_tick;
    p.access.components.reads = pick(rng, components);
    p.access.components.writes = pick(rng, components);
    p.access.resource_reads = pick(rng, resources);
    p.access.resource_writes = pick(rng, resources);
    p.access.exclusive = rng.below(10) == 0;
    for (const planned &earlier : out) {
      if (earlier.ph == p.ph && rng.below(5) == 0) {
        p.after.push_back(earlier.name);
      }
    }
    out.push_back(std::move(p));
  }
  return out;
}

struct schedule_outcome {
  std::uint64_t pairs = 0;
  std::uint64_t overlaps = 0;
  std::uint64_t misordered = 0;
  std::uint64_t exclusive_off_owner = 0;
};

// Job systems on 1, 2 and 4 workers, shared by the schedules (starting threads per schedule
// would dominate the run).
struct shared_jobs {
  std::array<rtt::scripted_platform, 3> plats;
  std::array<std::unique_ptr<rt::jobs::job_system>, 3> js;
};

void run_schedule(std::uint64_t seed, shared_jobs &pool, schedule_outcome &out) {
  splitmix rng(seed);
  const std::uint32_t choice = rng.below(3);
  const bool serial = rng.below(4) == 0;
  auto made = rtt::sched_rig::make_on(*pool.js.at(choice));
  REQUIRE(made.has_value());
  rtt::sched_rig &r = **made;
  const std::vector<planned> systems = plan(rng, r.ids);
  auto t = std::make_unique<trace>();
  for (std::uint32_t i = 0; i < systems.size(); ++i) {
    const planned &p = systems[i];
    std::vector<std::string_view> after(p.after.begin(), p.after.end());
    auto id = r.systems->add(ecs::system_desc{.name = p.name,
                                              .ph = p.ph,
                                              .access = p.access,
                                              .before = {},
                                              .after = std::move(after),
                                              .fn = timed_body{.t = t.get(), .self = i},
                                              .chunked = false,
                                              .queries = {}});
    REQUIRE(id.has_value());
  }
  r.systems->set_policy(serial ? ecs::execution_policy::serial_canonical
                               : ecs::execution_policy::parallel);
  REQUIRE(r.build().has_value());
  for (std::uint32_t f = 0; f < frames_per_schedule; ++f) {
    REQUIRE(r.step().has_value());
  }
  REQUIRE(r.systems->failures() == 0);
  for (std::uint32_t a = 0; a < systems.size(); ++a) {
    for (const interval &i : t->runs.at(a)) {
      if (systems[a].access.exclusive && !i.on_owner) {
        ++out.exclusive_off_owner;
      }
    }
    for (std::uint32_t b = a + 1; b < systems.size(); ++b) {
      if (systems[a].ph != systems[b].ph || !conflicts(systems[a].access, systems[b].access)) {
        continue;
      }
      const auto ia = r.systems->system_index(a);
      const auto ib = r.systems->system_index(b);
      REQUIRE(ia.has_value());
      REQUIRE(ib.has_value());
      const bool a_first = ia.value_or(0) < ib.value_or(0);
      const std::vector<interval> &ra = t->runs.at(a);
      const std::vector<interval> &rb = t->runs.at(b);
      REQUIRE(ra.size() == rb.size());
      for (std::size_t k = 0; k < ra.size(); ++k) {
        REQUIRE(ra[k].tick == rb[k].tick);
        ++out.pairs;
        const bool disjoint = ra[k].end < rb[k].start || rb[k].end < ra[k].start;
        if (!disjoint) {
          ++out.overlaps;
        } else if (a_first != (ra[k].end < rb[k].start)) {
          ++out.misordered;
        }
      }
    }
  }
}

} // namespace

TEST_CASE("RT-INV-SCHED-01: no two conflicting systems overlap over 1000 seeded schedules") {
  const std::uint32_t schedules = scaled(1000);
  auto pool = std::make_unique<shared_jobs>();
  const std::array<std::uint32_t, 3> worker_choices{1, 2, 4};
  for (std::size_t k = 0; k < pool->js.size(); ++k) {
    auto started = rtt::sched_rig::start_jobs(pool->plats.at(k).get(), worker_choices.at(k));
    REQUIRE(started.has_value());
    pool->js.at(k) = std::move(*started);
  }
  schedule_outcome out;
  for (std::uint64_t seed = 1; seed <= schedules; ++seed) {
    run_schedule(seed, *pool, out);
    if (out.overlaps != 0 || out.misordered != 0 || out.exclusive_off_owner != 0) {
      INFO("seed " << seed);
      FAIL("a conflicting overlap, order or exclusive thread");
    }
  }
  MESSAGE("conflicting pairs checked: " << out.pairs);
  CHECK(out.pairs > schedules);
  CHECK(out.overlaps == 0);
  CHECK(out.misordered == 0);
  CHECK(out.exclusive_off_owner == 0);
}
