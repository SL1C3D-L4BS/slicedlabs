// RT-INV-STRESS-01 (SL-ARCH-003 ARCH3-R162, R061, R064, R069, `stress_matrix` S1 and S3): S1 job
// DAG fuzz and S3 nested fork-join hold exactly-once and ordering in every cell with the watchdog
// silent. S1: workers {1, 2, 4, 8, 16} on the Chase-Lev deque with the stealing policy, 20
// iterations (scaled) of a random DAG of 1e4 jobs (scaled) per worker count, the iterations
// cycling 8 seeds; each job depends on up to 8 earlier jobs, every 97th on a job_counter that
// closes the group before it. Every job runs exactly once and only after all its dependencies.
// S3: fork-join nested to depth 16, 32 and 64 on 1, 2 and 8 workers. Both run on the steady clock
// with the job watchdog live (watchdog_ms 5000): no deadlock_suspected event and no assertion.
// The work runs under the harness watchdog (30 s); checks run on the test thread afterwards.

#include "sl/core/assert.hpp"
#include "sl/core/log.hpp"
#include "sl/runtime/headless_platform.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/platform.hpp"
#include "sl/runtime/scheduler_stats.hpp"
#include "sl/runtime/sync.hpp"
#include "sl/runtime/work_queue.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"
#include "../../../engine/runtime/testing/stress_harness.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace jobs = sl::runtime::jobs;
namespace rtt = sl::runtime::testing;

constexpr const char *test_name = "runtime_stress_jobs_test";

std::uint64_t splitmix64(std::uint64_t &state) {
  state += 0x9E3779B97F4A7C15ULL;
  std::uint64_t z = state;
  z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31U);
}

// A platform on the steady clock, so the job watchdog runs live.
struct live_platform {
  rtt::scripted_platform base;
  rt::steady_clock_source steady;
  rt::platform plat{.clock = steady,
                    .wall = base.get().wall,
                    .threads = base.get().threads,
                    .input = base.get().input,
                    .files = base.get().files,
                    .win = base.get().win,
                    .sink = base.get().sink,
                    .instance_id = 1};
};

// Counts watchdog reports and assertions for the whole case.
struct silence {
  std::shared_ptr<sl::log::memory_sink> events = std::make_shared<sl::log::memory_sink>(256);
  std::atomic<std::uint32_t> asserts{0};
  sl::scoped_assert_handler handler{[this](const sl::assert_report &) { asserts.fetch_add(1); }};

  silence() { sl::log::default_logger().add_sink(events); }
  silence(const silence &) = delete;
  silence &operator=(const silence &) = delete;
  silence(silence &&) = delete;
  silence &operator=(silence &&) = delete;
  ~silence() { sl::log::default_logger().remove_sink(events.get()); }

  [[nodiscard]] std::uint32_t reports() const {
    const auto records = events->records();
    return static_cast<std::uint32_t>(std::ranges::count_if(
        records, [](const auto &e) { return e.message == "deadlock_suspected"; }));
  }
};

sl::result<std::unique_ptr<jobs::job_system>> start(rt::platform &plat, std::uint32_t workers,
                                                    std::uint64_t seed) {
  jobs::job_config c;
  c.workers = workers;
  c.make_queue = jobs::make_ws_queue;
  auto policy = jobs::make_steal_policy({.seed = seed});
  if (!policy) {
    return std::unexpected(policy.error());
  }
  c.policy = std::move(*policy);
  c.seed = seed;
  return jobs::job_system::start(plat, std::move(c));
}

// --- S1 ---

struct dag {
  std::vector<std::vector<std::uint32_t>> deps;
  std::vector<std::atomic<std::uint32_t>> runs;
  std::vector<std::atomic<bool>> finished;
  std::atomic<std::uint64_t> early{0};

  explicit dag(std::uint32_t n) : deps(n), runs(n), finished(n) {}
};

struct dag_outcome {
  std::uint64_t seed = 0;
  std::uint32_t workers = 0;
  std::uint32_t not_once = 0;
  std::uint64_t early = 0;
  bool waited = false;
};

void body(dag &g, std::uint32_t i, rt::job_counter *group) noexcept {
  for (const std::uint32_t d : g.deps.at(i)) {
    if (!g.finished.at(d).load()) {
      g.early.fetch_add(1);
    }
  }
  g.runs.at(i).fetch_add(1);
  g.finished.at(i).store(true);
  if (group != nullptr) {
    group->sub(1);
  }
}

dag_outcome run_dag(jobs::job_system &js, std::uint32_t n, std::uint64_t seed) {
  dag g(n);
  std::vector<jobs::job_handle> handles(n);
  rt::job_counter all;
  std::vector<rt::job_counter> groups((n / 97) + 1);
  std::size_t current = 0;
  std::uint32_t group_first = 0;
  std::uint64_t state = seed;
  for (std::uint32_t i = 0; i < n; ++i) {
    std::vector<jobs::dependency> deps;
    const auto fan_in = i == 0 ? 0U : static_cast<std::uint32_t>(splitmix64(state) % 9U);
    for (std::uint32_t k = 0; k < fan_in && deps.size() < 7; ++k) {
      const auto d = static_cast<std::uint32_t>(splitmix64(state) % i);
      if (std::ranges::find(g.deps.at(i), d) == g.deps.at(i).end()) {
        g.deps.at(i).push_back(d);
        deps.push_back(jobs::dependency::on(handles.at(d)));
      }
    }
    rt::job_counter *group = &groups.at(current);
    if (i % 97 == 96) {
      for (std::uint32_t d = group_first; d < i; ++d) {
        if (std::ranges::find(g.deps.at(i), d) == g.deps.at(i).end()) {
          g.deps.at(i).push_back(d);
        }
      }
      deps.push_back(jobs::dependency::on(*group));
      group = nullptr;
      ++current;
      group_first = i + 1;
    } else {
      group->add(1);
    }
    handles.at(i) = js.submit(
        {.fn = [&g, i, group](jobs::job_context &) noexcept { body(g, i, group); }, .name = "dag"},
        deps, &all);
  }
  dag_outcome out;
  out.seed = seed;
  out.workers = js.worker_count();
  out.waited = js.wait(all).has_value();
  for (std::uint32_t i = 0; i < n; ++i) {
    out.not_once += g.runs.at(i).load() == 1 ? 0U : 1U;
  }
  out.early = g.early.load();
  return out;
}

// --- S3 ---

struct tree {
  jobs::job_system *js = nullptr;
  std::uint32_t depth = 0;
  std::atomic<std::uint32_t> jobs_run{0};
  std::atomic<std::uint32_t> leaves{0};
  std::atomic<std::uint32_t> wait_errors{0};
};

void level(tree &t, std::uint32_t d) noexcept {
  t.jobs_run.fetch_add(1);
  if (d < t.depth) {
    const jobs::job_handle leaf = t.js->submit({.fn =
                                                    [&t](jobs::job_context &) noexcept {
                                                      t.jobs_run.fetch_add(1);
                                                      t.leaves.fetch_add(1);
                                                    },
                                                .name = "leaf"});
    const jobs::job_handle next = t.js->submit(
        {.fn = [&t, d](jobs::job_context &) noexcept { level(t, d + 1); }, .name = "level"});
    if (!t.js->wait(next)) {
      t.wait_errors.fetch_add(1);
    }
    if (!t.js->wait(leaf)) {
      t.wait_errors.fetch_add(1);
    }
  }
}

struct tree_outcome {
  std::uint32_t workers = 0;
  std::uint32_t depth = 0;
  std::uint32_t jobs_run = 0;
  std::uint32_t leaves = 0;
  std::uint32_t wait_errors = 0;
  bool waited = false;
};

} // namespace

TEST_CASE("RT-INV-STRESS-01: S1 job DAG fuzz runs every job exactly once after its dependencies "
          "on 1, 2, 4, 8 and 16 workers with the watchdog silent") {
  const rtt::cap_timer cap;
  live_platform live;
  const silence quiet;
  const auto n = static_cast<std::uint32_t>(rtt::scaled(10000));
  const auto iterations = static_cast<std::uint32_t>(rtt::scaled(20));
  const std::uint64_t base_seed = rtt::stress_seed(0x5EED0990001ULL);
  SL_STRESS_SEED(guard, test_name, base_seed);
  std::vector<dag_outcome> outcomes;
  bool started = true;
  auto watched = rtt::run_watched([&] {
    for (const std::uint32_t workers : {1U, 2U, 4U, 8U, 16U}) {
      auto js = start(live.plat, workers, base_seed);
      if (!js) {
        started = false;
        return;
      }
      for (std::uint32_t it = 0; it < iterations; ++it) {
        outcomes.push_back(run_dag(**js, n, base_seed + (it % 8)));
      }
      started = started && (*js)->stop().has_value();
    }
  });
  REQUIRE(watched);
  REQUIRE(started);
  CHECK(outcomes.size() == 5U * iterations);
  for (const dag_outcome &o : outcomes) {
    INFO("workers " << o.workers << " dag seed " << o.seed << " jobs " << n);
    CHECK(o.waited);
    CHECK(o.not_once == 0);
    CHECK(o.early == 0);
  }
  CHECK(quiet.reports() == 0);
  CHECK(quiet.asserts.load() == 0);
  SL_STRESS_CAP(cap);
}

TEST_CASE("RT-INV-STRESS-01: S3 nested fork-join to depth 16, 32 and 64 completes on 1, 2 and 8 "
          "workers with the watchdog silent") {
  const rtt::cap_timer cap;
  live_platform live;
  const silence quiet;
  const std::uint64_t seed = rtt::stress_seed(0x5EED0990003ULL);
  SL_STRESS_SEED(guard, test_name, seed);
  const auto rounds = static_cast<std::uint32_t>(rtt::scaled(16));
  std::vector<tree_outcome> outcomes;
  bool started = true;
  auto watched = rtt::run_watched([&] {
    for (const std::uint32_t workers : {1U, 2U, 8U}) {
      auto js = start(live.plat, workers, seed);
      if (!js) {
        started = false;
        return;
      }
      for (std::uint32_t r = 0; r < rounds; ++r) {
        for (const std::uint32_t depth : {16U, 32U, 64U}) {
          tree t;
          t.js = js->get();
          t.depth = depth;
          const jobs::job_handle root = (*js)->submit(
              {.fn = [&t](jobs::job_context &) noexcept { level(t, 1); }, .name = "root"});
          tree_outcome o;
          o.waited = (*js)->wait(root).has_value();
          o.workers = workers;
          o.depth = depth;
          o.jobs_run = t.jobs_run.load();
          o.leaves = t.leaves.load();
          o.wait_errors = t.wait_errors.load();
          outcomes.push_back(o);
        }
      }
      started = started && (*js)->stop().has_value();
    }
  });
  REQUIRE(watched);
  REQUIRE(started);
  CHECK(outcomes.size() == 9U * rounds);
  for (const tree_outcome &o : outcomes) {
    INFO("workers " << o.workers << " depth " << o.depth);
    CHECK(o.waited);
    CHECK(o.wait_errors == 0);
    CHECK(o.leaves == o.depth - 1);
    CHECK(o.jobs_run == (2 * o.depth) - 1);
  }
  CHECK(quiet.reports() == 0);
  CHECK(quiet.asserts.load() == 0);
  SL_STRESS_CAP(cap);
}
