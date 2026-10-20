// RT-INV-JOB-02 (SL-ARCH-003 ARCH3-R064, `limits` job_nested_wait_depth, ADR-0024): fork-join
// nested to depth 64 completes on 1 worker and on 8. At every level a job forks a leaf and the
// next level, then waits for both (helping). On one worker the owner thread does not help (it
// waits on a cpu_fence the root signals), so all 64 nested waits stack up on that worker, which
// the test sees as the deepest same-thread nesting; on 8 workers and on 0 (the owner runs every
// job) the owner waits through job_system::wait.

#include "sl/core/error.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/sync.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <utility>

namespace {

namespace rt = sl::runtime;
namespace jobs = sl::runtime::jobs;
namespace rtt = sl::runtime::testing;

constexpr std::uint32_t depth = jobs::job_nested_wait_depth;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): per-thread test probe
thread_local std::uint32_t nesting = 0; // test-only: jobs running on this thread now

struct tree {
  jobs::job_system *js = nullptr;
  std::atomic<std::uint32_t> jobs_run{0};
  std::atomic<std::uint32_t> leaves{0};
  std::atomic<std::uint32_t> deepest{0};
  std::atomic<std::uint32_t> wait_errors{0};
  std::atomic<std::uint32_t> worker_threads_seen{0};
};

void enter(tree &t) noexcept {
  ++nesting;
  std::uint32_t seen = t.deepest.load();
  while (nesting > seen && !t.deepest.compare_exchange_weak(seen, nesting)) {
  }
  t.jobs_run.fetch_add(1);
}

void level(tree &t, std::uint32_t d) noexcept {
  enter(t);
  if (d < depth) {
    const jobs::job_handle leaf = t.js->submit({.fn =
                                                    [&t](jobs::job_context &) noexcept {
                                                      enter(t);
                                                      t.leaves.fetch_add(1);
                                                      --nesting;
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
  --nesting;
}

jobs::job_config config(std::uint32_t workers) {
  jobs::job_config c;
  c.workers = workers;
  return c;
}

} // namespace

TEST_CASE("RT-INV-JOB-02: fork-join nested to depth 64 completes on 1 worker") {
  rtt::scripted_platform plat;
  auto js = jobs::job_system::start(plat.get(), config(1));
  REQUIRE(js);
  tree t;
  t.js = js->get();
  rt::cpu_fence root_done(1);
  // depth 1 is the root; the owner waits on the fence, so only the worker runs jobs.
  static_cast<void>((*js)->submit({.fn =
                                       [&t, &root_done](jobs::job_context &) noexcept {
                                         level(t, 1);
                                         root_done.signal();
                                       },
                                   .name = "root"}));
  root_done.wait();
  CHECK(t.wait_errors.load() == 0);
  CHECK(t.leaves.load() == depth - 1);
  CHECK(t.jobs_run.load() == (2 * depth) - 1);
  // the worker held every level at once (the leaves of the deeper levels ran on top of it too)
  CHECK(t.deepest.load() >= depth);
  REQUIRE((*js)->stop());
}

TEST_CASE("RT-INV-JOB-02: fork-join nested to depth 64 completes on 8 workers and on 0") {
  rtt::scripted_platform plat;
  for (const std::uint32_t workers : {8U, 0U}) {
    auto js = jobs::job_system::start(plat.get(), config(workers));
    REQUIRE(js);
    for (int round = 0; round < 4; ++round) {
      tree t;
      t.js = js->get();
      const jobs::job_handle root = (*js)->submit(
          {.fn = [&t](jobs::job_context &) noexcept { level(t, 1); }, .name = "root"});
      REQUIRE((*js)->wait(root));
      INFO("workers " << workers << " round " << round);
      CHECK(t.wait_errors.load() == 0);
      CHECK(t.leaves.load() == depth - 1);
      CHECK(t.jobs_run.load() == (2 * depth) - 1);
    }
    REQUIRE((*js)->stop());
  }
}
