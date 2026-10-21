// RT-INV-STEAL-04 (SL-ARCH-003 ARCH3-R074, R075, ADR-0024): the stealing policy promotes a
// background job once it waited aging_limit rounds, polls the injection queue once every 61 local
// pops and draws the same victim sequence for the same seed (per-thread xorshift64 seeded with
// splitmix64(seed ^ index), never itself, at most 2 x workers attempts per round). In a running
// job system (one worker, ws_deque, make_steal_policy) whose local queue is kept saturated by
// self-resubmitting frame chains, a background job starts within aging_limit worker jobs and a
// frame job injected from the owner thread within 61 worker jobs of its submit.

#include "sl/core/error.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/scheduler_stats.hpp"
#include "sl/runtime/work_queue.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace jobs = sl::runtime::jobs;
namespace rtt = sl::runtime::testing;
using namespace std::chrono_literals;

std::unique_ptr<jobs::scheduler_policy> policy(std::uint64_t seed) {
  auto made = jobs::make_steal_policy({.seed = seed});
  REQUIRE(made);
  return std::move(*made);
}

// Victims for `rounds` rounds of 2 x workers attempts on every slot (owner included).
std::vector<std::uint32_t> victims(jobs::scheduler_policy &p, std::uint32_t workers,
                                   std::uint32_t rounds) {
  std::vector<std::uint32_t> out;
  for (std::uint32_t r = 0; r < rounds; ++r) {
    for (std::uint32_t self = 0; self <= workers; ++self) {
      for (std::uint32_t a = 0; a < 2 * workers; ++a) {
        out.push_back(p.victim(self, a));
      }
    }
  }
  return out;
}

// Frame chains that keep worker 0's local queue non-empty: each job resubmits itself (from the
// worker, so onto its local queue) until `stop`.
struct chain_state {
  jobs::job_system *sys = nullptr;
  std::atomic<bool> stop{false};
  std::atomic<std::uint32_t> worker_runs{0};
  std::atomic<std::uint32_t> runs_at_target{0};
  std::atomic<bool> target_started{false};
};

struct chain_job {
  chain_state *s;
  void operator()(const jobs::job_context &ctx) noexcept {
    if (ctx.worker() == 0) {
      s->worker_runs.fetch_add(1);
    }
    if (!s->stop.load()) {
      s->sys->submit({.fn = chain_job{s}, .name = "chain"});
    }
  }
};

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

// Worker jobs run between submitting `target_prio` work from the owner thread and its start, with
// worker 0 saturated by chains. Fails the case when the target does not start within 5 s.
std::uint32_t runs_before_start(jobs::priority target_prio) {
  rtt::scripted_platform plat;
  jobs::job_config c;
  c.workers = 1;
  c.make_queue = jobs::make_ws_queue;
  c.policy = policy(0x5EED0880004ULL);
  auto js = jobs::job_system::start(plat.get(), std::move(c));
  REQUIRE(js);
  chain_state s;
  s.sys = js->get();
  for (int i = 0; i < 4; ++i) {
    (*js)->submit({.fn = chain_job{&s}, .name = "chain"});
  }
  REQUIRE(wait_for([&s] { return s.worker_runs.load() >= 256; }));
  const std::uint32_t base = s.worker_runs.load();
  (*js)->submit({.fn =
                     [&s](jobs::job_context &) noexcept {
                       s.runs_at_target.store(s.worker_runs.load());
                       s.target_started.store(true);
                     },
                 .prio = target_prio,
                 .name = "target"});
  const bool started = wait_for([&s] { return s.target_started.load(); });
  s.stop.store(true);
  (*js)->quiesce();
  REQUIRE((*js)->stop());
  REQUIRE(started);
  return s.runs_at_target.load() - base;
}

} // namespace

TEST_CASE("RT-INV-STEAL-04: the victim sequence is identical for the same seed") {
  for (const std::uint32_t workers : {1U, 2U, 7U, 64U}) {
    INFO("workers " << workers);
    auto a = policy(42);
    auto b = policy(42);
    auto other = policy(43);
    a->on_start(workers, 0);
    b->on_start(workers, 0);
    other->on_start(workers, 0);
    const std::vector<std::uint32_t> va = victims(*a, workers, 8);
    CHECK(va == victims(*b, workers, 8));
    if (workers > 2) {
      CHECK(va != victims(*other, workers, 8));
    }
    // never itself (a worker), always in range, and nothing past 2 x workers attempts
    std::size_t i = 0;
    std::uint32_t bad = 0;
    for (std::uint32_t r = 0; r < 8; ++r) {
      for (std::uint32_t self = 0; self <= workers; ++self) {
        for (std::uint32_t at = 0; at < 2 * workers; ++at, ++i) {
          const std::uint32_t v = va[i];
          const bool skip = v == self;
          bad += ((!skip && v >= workers) || (skip && workers > 1)) ? 1U : 0U;
        }
      }
    }
    CHECK(bad == 0);
    CHECK(a->victim(0, 2 * workers) == 0);
  }
  // the job system's seed is mixed in: same steal seed, other start seed, other sequence
  auto a = policy(42);
  auto b = policy(42);
  a->on_start(8, 1);
  b->on_start(8, 2);
  CHECK(victims(*a, 8, 4) != victims(*b, 8, 4));
  CHECK(jobs::make_steal_policy({.injection_poll_every = 0}).error().code ==
        sl::errc::invalid_argument);
  CHECK(jobs::make_steal_policy({.aging_limit = 0}).error().code == sl::errc::invalid_argument);
}

TEST_CASE("RT-INV-STEAL-04: the injection queue is polled once every 61 local pops") {
  auto p = policy(1);
  p->on_start(4, 0);
  std::vector<std::uint64_t> polled;
  for (std::uint64_t pops = 0; pops <= std::uint64_t{61} * 100U; ++pops) {
    if (p->poll_injection(2, pops)) {
      polled.push_back(pops);
    }
    // a second round at the same count (the injection poll took a job) does not poll again
    CHECK_FALSE(p->poll_injection(2, pops));
  }
  REQUIRE(polled.size() == 100);
  for (std::size_t i = 0; i < polled.size(); ++i) {
    CHECK(polled[i] == 61 * (i + 1));
  }
  std::vector<jobs::worker_counters> counters(5);
  p->read_counters(counters);
  CHECK(counters[2].injection_polls == 100);
  CHECK(jobs::collect(*p, 5).total().injection_polls == 100);
  CHECK(p->spin_limit() == 64);
  CHECK(jobs::steal_options{}.injection_poll_every == 61);
}

TEST_CASE("RT-INV-STEAL-04: background work is promoted after aging_limit rounds") {
  auto p = policy(1);
  p->on_start(2, 0);
  for (std::uint32_t waited = 0; waited < jobs::aging_limit; ++waited) {
    CHECK_FALSE(p->promote_background(waited));
  }
  CHECK(p->promote_background(jobs::aging_limit));
  CHECK(p->promote_background(jobs::aging_limit + 1000));
  // one promotion per crossing of aging_limit
  CHECK(jobs::collect(*p, 3).total().aging_promotions == 1);
  CHECK(p->promote_background(jobs::aging_limit));
  CHECK(jobs::collect(*p, 3).total().aging_promotions == 2);
}

TEST_CASE("RT-INV-STEAL-04: under saturation a background job starts within aging_limit rounds") {
  const std::uint32_t runs = runs_before_start(jobs::priority::background);
  INFO("worker jobs before the background job started: " << runs);
  CHECK(runs <= jobs::aging_limit + 1);
}

TEST_CASE("RT-INV-STEAL-04: under saturation an injected frame job starts within 61 local pops") {
  const std::uint32_t runs = runs_before_start(jobs::priority::frame);
  INFO("worker jobs before the injected job started: " << runs);
  CHECK(runs <= 61 + 1);
}
