// RT-INV-STEAL-05 (SL-ARCH-003 ARCH3-R073, R076, ADR-0024): the DAG workload of RT-INV-JOB-01
// (seeded random DAGs, fan-in up to 7 earlier jobs; 4000 jobs scaled by SL_RUNTIME_STRESS_SCALE,
// floored at 1) gives identical results on locked_deque with the reference policy and on ws_deque
// with the stealing policy, on 1, 2 and 4 workers: every job runs once, after its dependencies,
// and computes the same value (a splitmix64 of its index and the sum of its dependencies'
// values). The jobs counted per running thread sum to the jobs run (= stats().completed), the
// policy's successful steals never exceed them, and scheduler_stats::total() is the field-wise
// sum of the per-slot counters.

#include "sl/core/error.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/scheduler_stats.hpp"
#include "sl/runtime/work_queue.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

namespace {

namespace jobs = sl::runtime::jobs;
namespace rtt = sl::runtime::testing;

std::uint32_t scaled(std::uint32_t base) {
  double scale = 1.0;
  if (const char *env = std::getenv("SL_RUNTIME_STRESS_SCALE"); env != nullptr && *env != '\0') {
    scale = std::strtod(env, nullptr);
  }
  return std::max<std::uint32_t>(1, static_cast<std::uint32_t>(base * scale));
}

std::uint64_t splitmix64(std::uint64_t x) {
  std::uint64_t z = x + 0x9E3779B97F4A7C15ULL;
  z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31U);
}

struct dag {
  std::vector<std::vector<std::uint32_t>> deps;
  std::vector<std::atomic<std::uint64_t>> value;
  std::vector<std::atomic<std::uint32_t>> runs;
  std::vector<std::atomic<bool>> finished;
  std::atomic<std::uint64_t> early{0};
  std::array<std::atomic<std::uint64_t>, jobs::max_workers + 2> per_thread{};

  explicit dag(std::uint32_t n) : deps(n), value(n), runs(n), finished(n) {}
};

void body(dag &g, std::uint32_t i, std::uint32_t worker) noexcept {
  std::uint64_t sum = 0;
  for (const std::uint32_t d : g.deps[i]) {
    if (!g.finished[d].load()) {
      g.early.fetch_add(1);
    }
    sum += g.value[d].load();
  }
  g.value[i].store(splitmix64(i + sum));
  g.runs[i].fetch_add(1);
  const std::size_t slot = worker <= jobs::max_workers ? worker : jobs::max_workers + 1;
  g.per_thread.at(slot).fetch_add(1);
  g.finished[i].store(true);
}

struct outcome {
  std::vector<std::uint64_t> values;
  std::uint32_t not_once = 0;
  std::uint64_t early = 0;
  std::uint64_t per_thread_sum = 0;
  std::uint64_t completed = 0;
  jobs::scheduler_stats stats;
};

outcome run_dag(jobs::job_config config, std::uint32_t n, std::uint64_t seed) {
  rtt::scripted_platform plat;
  const jobs::scheduler_policy *policy = config.policy.get();
  const std::uint32_t workers = config.workers;
  auto js = jobs::job_system::start(plat.get(), std::move(config));
  REQUIRE(js);
  dag g(n);
  std::vector<jobs::job_handle> handles(n);
  std::uint64_t state = seed;
  for (std::uint32_t i = 0; i < n; ++i) {
    std::vector<jobs::dependency> deps;
    state = splitmix64(state);
    const auto fan_in = i == 0 ? 0U : static_cast<std::uint32_t>(state % 8U);
    for (std::uint32_t k = 0; k < fan_in; ++k) {
      state = splitmix64(state);
      const auto d = static_cast<std::uint32_t>(state % i);
      if (std::ranges::find(g.deps[i], d) == g.deps[i].end()) {
        g.deps[i].push_back(d);
        deps.push_back(jobs::dependency::on(handles[d]));
      }
    }
    jobs::job_desc desc{
        .fn = [&g, i](jobs::job_context &ctx) noexcept { body(g, i, ctx.worker()); },
        .name = "dag"};
    handles[i] = (*js)->submit(std::move(desc), deps);
  }
  (*js)->quiesce();
  outcome out;
  out.completed = (*js)->stats().completed;
  for (std::uint32_t i = 0; i < n; ++i) {
    out.values.push_back(g.value[i].load());
    out.not_once += g.runs[i].load() == 1 ? 0U : 1U;
  }
  out.early = g.early.load();
  for (const auto &c : g.per_thread) {
    out.per_thread_sum += c.load();
  }
  if (policy != nullptr) {
    out.stats = jobs::collect(*policy, workers + 1);
  }
  REQUIRE((*js)->stop());
  return out;
}

} // namespace

TEST_CASE("RT-INV-STEAL-05: the DAG workload gives identical results on locked_deque and "
          "ws_deque and the counters sum to the jobs run") {
  const std::uint32_t n = scaled(4000);
  for (const std::uint32_t workers : {1U, 2U, 4U}) {
    const std::uint64_t seed = 0x5EED0880005ULL + workers;
    INFO("workers " << workers << " seed " << seed);
    jobs::job_config locked;
    locked.workers = workers;
    const outcome a = run_dag(std::move(locked), n, seed);

    jobs::job_config ws;
    ws.workers = workers;
    ws.make_queue = jobs::make_ws_queue;
    auto policy = jobs::make_steal_policy({.seed = seed});
    REQUIRE(policy);
    ws.policy = std::move(*policy);
    const outcome b = run_dag(std::move(ws), n, seed);

    CHECK(a.not_once == 0);
    CHECK(b.not_once == 0);
    CHECK(a.early == 0);
    CHECK(b.early == 0);
    CHECK(a.values == b.values);
    CHECK(a.per_thread_sum == n);
    CHECK(b.per_thread_sum == n);
    CHECK(a.completed == n);
    CHECK(b.completed == n);

    REQUIRE(b.stats.workers.size() == workers + 1);
    const jobs::worker_counters total = b.stats.total();
    jobs::worker_counters sum;
    for (const jobs::worker_counters &w : b.stats.workers) {
      sum.steals += w.steals;
      sum.failed_steals += w.failed_steals;
      sum.parks += w.parks;
      sum.wakes += w.wakes;
      sum.injection_polls += w.injection_polls;
      sum.aging_promotions += w.aging_promotions;
    }
    CHECK(total == sum);
    CHECK(total.steals <= b.per_thread_sum);
    CHECK(total.jobs_run == 0); // the scheduler_policy seam has no run hook (deviation)
  }
}
