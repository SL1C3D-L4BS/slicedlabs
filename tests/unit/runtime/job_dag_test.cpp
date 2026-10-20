// RT-INV-JOB-01 (SL-ARCH-003 ARCH3-R061, R063, R070, ADR-0024): every job of seeded random DAGs
// (1e4 jobs scaled by SL_RUNTIME_STRESS_SCALE, fan-in up to 8 earlier jobs, every 97th job also
// gated on the job_counter of the jobs before it) runs exactly once and only after all of its
// dependencies, on 0, 1, 2 and 8 workers and under RFP4 worker stalls; locked_deque passes the
// queue contract suite, and make_ws_queue is unsupported until WO-086 (start fails with it).

#include "sl/core/error.hpp"
#include "sl/runtime/faults.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/sync.hpp"
#include "sl/runtime/work_queue.hpp"

#include "../../../engine/runtime/testing/faults_arm.hpp"
#include "../../../engine/runtime/testing/queue_contract.hpp"
#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace jobs = sl::runtime::jobs;
namespace rtt = sl::runtime::testing;

std::uint32_t scaled(std::uint32_t base) {
  double scale = 1.0;
  if (const char *env = std::getenv("SL_RUNTIME_STRESS_SCALE"); env != nullptr && *env != '\0') {
    scale = std::strtod(env, nullptr);
  }
  return std::max<std::uint32_t>(64, static_cast<std::uint32_t>(base * scale));
}

std::uint64_t splitmix64(std::uint64_t &state) {
  state += 0x9E3779B97F4A7C15ULL;
  std::uint64_t z = state;
  z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31U);
}

// One DAG: deps[i] are earlier job indices; finished[i] is set at the end of job i's body.
struct dag {
  std::vector<std::vector<std::uint32_t>> deps;
  std::vector<std::atomic<std::uint32_t>> runs;
  std::vector<std::atomic<bool>> finished;
  std::atomic<std::uint64_t> early{0}; // a job that started before a dependency finished

  explicit dag(std::uint32_t n) : deps(n), runs(n), finished(n) {}
};

void body(dag &g, std::uint32_t i, rt::job_counter *group) noexcept {
  for (const std::uint32_t d : g.deps[i]) {
    if (!g.finished[d].load()) {
      g.early.fetch_add(1);
    }
  }
  g.runs[i].fetch_add(1);
  g.finished[i].store(true);
  if (group != nullptr) {
    group->sub(1);
  }
}

// Builds and runs one seeded DAG of `n` jobs on `js`. Jobs [first, i) of a group count in the
// group's own job_counter (added before the submit, subtracted at the end of the body); every
// 97th job closes the group: it depends on that counter, so all of the group are its dependencies.
void run_dag(jobs::job_system &js, std::uint32_t n, std::uint64_t seed) {
  dag g(n);
  std::vector<jobs::job_handle> handles(n);
  rt::job_counter all;
  // one counter per group, sized once and never grown, so a counter's address is stable
  std::vector<rt::job_counter> groups((n / 97) + 1);
  std::size_t current = 0;
  std::uint32_t group_first = 0;
  std::uint64_t state = seed;
  for (std::uint32_t i = 0; i < n; ++i) {
    std::vector<jobs::dependency> deps;
    const auto fan_in = i == 0 ? 0U : static_cast<std::uint32_t>(splitmix64(state) % 9U);
    for (std::uint32_t k = 0; k < fan_in && deps.size() < 7; ++k) {
      const auto d = static_cast<std::uint32_t>(splitmix64(state) % i);
      if (std::ranges::find(g.deps[i], d) == g.deps[i].end()) {
        g.deps[i].push_back(d);
        deps.push_back(jobs::dependency::on(handles[d]));
      }
    }
    rt::job_counter *group = &groups.at(current);
    const bool closes = i % 97 == 96;
    if (closes) {
      for (std::uint32_t d = group_first; d < i; ++d) {
        if (std::ranges::find(g.deps[i], d) == g.deps[i].end()) {
          g.deps[i].push_back(d);
        }
      }
      deps.push_back(jobs::dependency::on(*group));
      group = nullptr;
      ++current;
      group_first = i + 1;
    } else {
      group->add(1);
    }
    handles[i] = js.submit(
        {.fn = [&g, i, group](jobs::job_context &) noexcept { body(g, i, group); }, .name = "dag"},
        deps, &all);
  }
  REQUIRE(js.wait(all));
  std::uint32_t not_once = 0;
  for (std::uint32_t i = 0; i < n; ++i) {
    not_once += g.runs[i].load() == 1 ? 0U : 1U;
  }
  INFO("seed " << seed << " workers " << js.worker_count());
  CHECK(not_once == 0);
  CHECK(g.early.load() == 0);
}

jobs::job_config config(std::uint32_t workers) {
  jobs::job_config c;
  c.workers = workers;
  return c;
}

} // namespace

TEST_CASE("RT-INV-JOB-01: every job of seeded random DAGs runs exactly once after its "
          "dependencies") {
  rtt::scripted_platform plat;
  const std::uint32_t n = scaled(10000);
  for (const std::uint32_t workers : {0U, 1U, 2U, 8U}) {
    auto js = jobs::job_system::start(plat.get(), config(workers));
    REQUIRE(js);
    for (const std::uint64_t seed : {0x5EED0860001ULL, 0x5EED0860002ULL}) {
      run_dag(**js, n, seed + workers);
    }
    CHECK((*js)->stats().pending == 0);
    REQUIRE((*js)->stop());
  }
}

TEST_CASE("RT-INV-JOB-01: counter and fence dependencies hold a job until they are satisfied") {
  rtt::scripted_platform plat;
  auto js = jobs::job_system::start(plat.get(), config(2));
  REQUIRE(js);
  rt::cpu_fence fence(1);
  rt::job_counter counter;
  counter.add(1);
  std::atomic<int> ran{0};
  const std::array deps{jobs::dependency::on(fence), jobs::dependency::on(counter)};
  const jobs::job_handle h = (*js)->submit(
      {.fn = [&ran](jobs::job_context &) noexcept { ran.fetch_add(1); }, .name = "gated"}, deps);
  CHECK((*js)->status(h).value() == jobs::job_status::pending);
  fence.signal();
  CHECK(ran.load() == 0);
  counter.sub(1);
  REQUIRE((*js)->wait(h));
  CHECK(ran.load() == 1);
  REQUIRE((*js)->stop());
}

TEST_CASE("RT-INV-JOB-01: the DAG runs exactly once under RFP4 worker stalls") {
  if (!rt::faults_compiled) {
    MESSAGE("failpoints compiled out (Release): RFP4 is not visited");
    return;
  }
  rtt::scripted_platform plat;
  auto js = jobs::job_system::start(plat.get(), config(4));
  REQUIRE(js);
  {
    const rtt::scoped_arm stalls(rt::failpoint::rfp4, 1, true, 20);
    REQUIRE(stalls.status());
    const std::uint64_t before = rt::visits(rt::failpoint::rfp4);
    run_dag(**js, 400, 0x5EED0860004ULL);
    CHECK(rt::visits(rt::failpoint::rfp4) > before);
  }
  REQUIRE((*js)->stop());
}

TEST_CASE("RT-INV-JOB-01: locked_deque passes the queue contract suite") {
  rtt::queue_contract_options options;
  options.thieves = 3;
  options.items = scaled(50000);
  options.seed = 0x5EED0860003ULL;
  options.capacity = 512;
  const std::vector<std::string> failures =
      rtt::run_queue_contract(jobs::make_locked_queue, options);
  for (const std::string &f : failures) {
    MESSAGE(f);
  }
  CHECK(failures.empty());
  options.owners = 2;
  CHECK_FALSE(rtt::run_queue_contract(jobs::make_locked_queue, options).empty());
  CHECK(jobs::make_locked_queue(0).error().code == sl::errc::invalid_argument);
}

TEST_CASE("RT-INV-JOB-01: make_ws_queue is unsupported until WO-086 and start fails with it") {
  CHECK(jobs::make_ws_queue(16).error().code == sl::errc::unsupported);
  rtt::scripted_platform plat;
  jobs::job_config c = config(1);
  c.make_queue = jobs::make_ws_queue;
  auto js = jobs::job_system::start(plat.get(), std::move(c));
  REQUIRE_FALSE(js);
  CHECK(js.error().code == sl::errc::unsupported);
  auto policy = jobs::make_reference_policy();
  REQUIRE(policy);
  policy->on_start(4, 1);
  CHECK(policy->victim(1, 0) == 2);
  CHECK(policy->victim(3, 0) == 0);
  CHECK(policy->victim(3, 2) == 2);
  CHECK_FALSE(policy->poll_injection(0, 61));
  CHECK_FALSE(policy->promote_background(1000));
  CHECK(policy->spin_limit() == rt::spin_limit);
  std::array<jobs::worker_counters, 4> counters{};
  counters[1].steals = 7;
  policy->read_counters(counters);
  CHECK(std::ranges::all_of(
      counters, [](const jobs::worker_counters &w) { return w == jobs::worker_counters{}; }));
}
