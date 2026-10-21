// RT-INV-STEAL-06 (SL-ARCH-003 ARCH3-R074, ADR-0024): with ws_deque and the stealing policy, idle
// workers park on the job system's event_count after spin_limit empty rounds, and every submit
// made after all of them parked is run. 1e4 rounds (scaled by SL_RUNTIME_STRESS_SCALE, 0.1 under
// the sanitizers, floored at 1) on 3 workers: each round waits until every worker is parked (its
// policy counters show one more park than wake) and has parked again since the previous round's
// submit woke it, submits one frame job from the owner thread
// without helping, and waits for it to run. Every wait is bounded at 5 s (a lost wakeup fails the
// case instead of hanging). Afterwards every worker parked at least once per round and the
// counters read through scheduler_stats are consistent (wakes <= parks <= wakes + 1).

#include "sl/runtime/jobs.hpp"
#include "sl/runtime/scheduler_stats.hpp"
#include "sl/runtime/work_queue.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <thread>
#include <utility>

namespace {

namespace jobs = sl::runtime::jobs;
namespace rtt = sl::runtime::testing;
using namespace std::chrono_literals;

constexpr std::uint32_t workers = 3;

std::uint32_t scaled(double base) {
  double scale = 1.0;
  if (const char *env = std::getenv("SL_RUNTIME_STRESS_SCALE"); env != nullptr && *env != '\0') {
    scale = std::strtod(env, nullptr);
  }
  return std::max<std::uint32_t>(1, static_cast<std::uint32_t>(base * scale));
}

bool wait_for(const auto &done) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!done()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

// True when every worker is parked (one more park than wake) and parked again since `before`
// (the parks seen when the previous round was submitted; its notify_all ended every park).
bool all_parked(const jobs::scheduler_policy &policy, std::array<std::uint64_t, workers> &before) {
  const jobs::scheduler_stats s = jobs::collect(policy, workers);
  for (std::uint32_t k = 0; k < workers; ++k) {
    const jobs::worker_counters &w = s.workers[k];
    if (w.parks <= w.wakes || w.parks <= before.at(k)) {
      return false;
    }
  }
  for (std::uint32_t k = 0; k < workers; ++k) {
    before.at(k) = s.workers[k].parks;
  }
  return true;
}

} // namespace

TEST_CASE("RT-INV-STEAL-06: idle workers park after spin_limit and every submit after a park is "
          "run") {
  rtt::scripted_platform plat;
  auto made = jobs::make_steal_policy({.seed = 0x5EED0880006ULL});
  REQUIRE(made);
  const jobs::scheduler_policy *policy = made->get();
  REQUIRE(policy->spin_limit() == 64);
  jobs::job_config c;
  c.workers = workers;
  c.make_queue = jobs::make_ws_queue;
  c.policy = std::move(*made);
  auto js = jobs::job_system::start(plat.get(), std::move(c));
  REQUIRE(js);
  const std::uint32_t rounds = scaled(1e4);
  std::atomic<std::uint32_t> ran{0};
  std::uint32_t not_parked = 0;
  std::uint32_t not_run = 0;
  std::array<std::uint64_t, workers> parks_before{};
  for (std::uint32_t r = 0; r < rounds && not_parked == 0 && not_run == 0; ++r) {
    if (!wait_for([policy, &parks_before] { return all_parked(*policy, parks_before); })) {
      ++not_parked;
      break;
    }
    (*js)->submit(
        {.fn = [&ran](jobs::job_context &) noexcept { ran.fetch_add(1); }, .name = "after_park"});
    if (!wait_for([&ran, r] { return ran.load() == r + 1; })) {
      ++not_run;
    }
  }
  INFO(rounds << " rounds, " << ran.load() << " jobs run");
  CHECK(not_parked == 0);
  CHECK(not_run == 0);
  CHECK(ran.load() == rounds);
  (*js)->quiesce();
  REQUIRE((*js)->stop());
  const jobs::scheduler_stats s = jobs::collect(*policy, workers + 1);
  for (std::uint32_t k = 0; k < workers; ++k) {
    INFO("worker " << k);
    CHECK(s.workers[k].parks >= rounds);
    CHECK(s.workers[k].wakes <= s.workers[k].parks);
    CHECK(s.workers[k].parks <= s.workers[k].wakes + 1);
  }
  CHECK(s.workers[workers].parks == 0); // the owner thread is not a worker: it never parks here
}
