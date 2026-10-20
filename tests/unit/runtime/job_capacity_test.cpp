// RT-INV-JOB-03 (SL-ARCH-003 ARCH3-R062, R063, R065, `limits` job_record_bytes,
// job_max_dependencies): submit at capacity helps and progresses; only a job without
// predecessors runs inline on the caller, and only when no ready job exists; try_submit at
// capacity is job_capacity; the status of a stale handle is stale_job and waiting on it is ok;
// after stop submits are refused with scheduler_stopped; more than 8 dependencies are
// invalid_argument. Records are kept busy by jobs blocked on a cpu_fence the test signals.
// Threads other than the job system's are std::jthread joined at scope end.

#include "sl/core/error.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/sync.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace jobs = sl::runtime::jobs;
namespace rtt = sl::runtime::testing;
using namespace std::chrono_literals;

jobs::job_config config(std::uint32_t workers, std::uint32_t capacity) {
  jobs::job_config c;
  c.workers = workers;
  c.capacity = capacity;
  return c;
}

jobs::job_desc blocker(rt::cpu_fence &open, std::atomic<int> &started) {
  return {.fn =
              [&open, &started](jobs::job_context &) noexcept {
                started.fetch_add(1);
                open.wait();
              },
          .name = "blocker"};
}

jobs::job_desc noop(std::string_view name = "noop") {
  return {.fn = [](jobs::job_context &) noexcept {}, .name = name};
}

bool eventually(const auto &condition) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!condition()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

} // namespace

static_assert(sizeof(jobs::job_fn) == 64);
static_assert(std::is_nothrow_move_constructible_v<jobs::job_fn>);
static_assert(!std::is_copy_constructible_v<jobs::job_fn>);
static_assert(jobs::job_max_dependencies == 8 && jobs::job_record_bytes == 128);

TEST_CASE("RT-INV-JOB-03: try_submit at capacity is job_capacity") {
  rtt::scripted_platform plat;
  auto js = jobs::job_system::start(plat.get(), config(1, 4));
  REQUIRE(js);
  rt::cpu_fence open(1);
  std::atomic<int> started{0};
  std::vector<jobs::job_handle> held;
  for (int i = 0; i < 4; ++i) {
    auto h = (*js)->try_submit(blocker(open, started));
    REQUIRE(h);
    held.push_back(*h);
  }
  auto refused = (*js)->try_submit(noop());
  REQUIRE_FALSE(refused);
  CHECK(refused.error().code == sl::errc::job_capacity);
  open.signal();
  for (const jobs::job_handle h : held) {
    CHECK((*js)->wait(h));
  }
  CHECK((*js)->try_submit(noop()));
  REQUIRE((*js)->stop());
}

TEST_CASE("RT-INV-JOB-03: submit at capacity helps and progresses") {
  rtt::scripted_platform plat;
  auto js = jobs::job_system::start(plat.get(), config(1, 8));
  REQUIRE(js);
  rt::cpu_fence open(1);
  std::atomic<int> started{0};
  const jobs::job_handle b = (*js)->submit(blocker(open, started));
  REQUIRE(eventually([&] { return started.load() == 1; })); // the only worker is blocked
  std::atomic<int> ran{0};
  rt::job_counter all;
  const std::thread::id owner = std::this_thread::get_id();
  std::atomic<int> on_owner{0};
  for (int i = 0; i < 200; ++i) {
    (*js)->submit({.fn =
                       [&ran, &on_owner, owner](jobs::job_context &) noexcept {
                         ran.fetch_add(1);
                         if (std::this_thread::get_id() == owner) {
                           on_owner.fetch_add(1);
                         }
                       },
                   .name = "fill"},
                  {}, &all);
  }
  // the owner had to help: 7 free records for 200 jobs while the worker is blocked
  CHECK((*js)->stats().capacity_waits > 0);
  CHECK(on_owner.load() > 0);
  open.signal();
  REQUIRE((*js)->wait(all));
  CHECK(ran.load() == 200);
  CHECK((*js)->wait(b));
  REQUIRE((*js)->stop());
}

TEST_CASE("RT-INV-JOB-03: only a job without predecessors runs inline on the caller") {
  rtt::scripted_platform plat;
  auto js = jobs::job_system::start(plat.get(), config(1, 2));
  REQUIRE(js);
  rt::cpu_fence open(1);
  std::atomic<int> started{0};
  const jobs::job_handle b1 = (*js)->submit(blocker(open, started));
  REQUIRE(eventually([&] { return started.load() == 1; }));
  // b2 waits for b1: both records are used and no job is ready
  const std::array after_b1{jobs::dependency::on(b1)};
  const jobs::job_handle b2 = (*js)->submit(noop("b2"), after_b1);
  REQUIRE((*js)->status(b2).value() == jobs::job_status::pending);

  // no predecessor: runs inline on the owner thread, the handle is null
  std::thread::id inline_thread{};
  const jobs::job_handle inline_handle = (*js)->submit(
      {.fn = [&inline_thread](
                 jobs::job_context &) noexcept { inline_thread = std::this_thread::get_id(); },
       .name = "inline"});
  CHECK(inline_handle == jobs::job_handle{});
  CHECK(inline_thread == std::this_thread::get_id());
  CHECK((*js)->stats().inline_runs == 1);
  CHECK((*js)->wait(inline_handle)); // the null handle

  // with a predecessor: blocks until a record frees, never inline
  std::atomic<bool> returned{false};
  std::thread::id ran_on{};
  std::atomic<bool> ran{false};
  std::thread::id submitter{};
  {
    std::jthread t([&] {
      submitter = std::this_thread::get_id();
      const jobs::job_handle h = (*js)->submit({.fn =
                                                    [&ran_on, &ran](jobs::job_context &) noexcept {
                                                      ran_on = std::this_thread::get_id();
                                                      ran.store(true);
                                                    },
                                                .name = "with-pred"},
                                               after_b1);
      CHECK((*js)->wait(h));
      returned.store(true);
    });
    std::this_thread::sleep_for(50ms);
    CHECK_FALSE(returned.load());
    CHECK_FALSE(ran.load());
    open.signal(); // b1 finishes, b2 runs, records free
    REQUIRE(eventually([&] { return returned.load(); }));
  }
  CHECK(ran.load());
  CHECK(ran_on != submitter);
  CHECK((*js)->stats().inline_runs == 1);
  CHECK((*js)->wait(b2));
  REQUIRE((*js)->stop());
}

TEST_CASE("RT-INV-JOB-03: a stale handle is stale_job and waiting on it is ok") {
  rtt::scripted_platform plat;
  auto js = jobs::job_system::start(plat.get(), config(1, 1));
  REQUIRE(js);
  const jobs::job_handle first = (*js)->submit(noop());
  CHECK(first.generation == 1);
  REQUIRE((*js)->wait(first));
  CHECK((*js)->status(first).value() == jobs::job_status::done);
  const jobs::job_handle second = (*js)->submit(noop());
  CHECK(second.index == first.index);
  CHECK(second.generation == first.generation + 1);
  REQUIRE((*js)->wait(second));
  auto stale = (*js)->status(first);
  REQUIRE_FALSE(stale);
  CHECK(stale.error().code == sl::errc::stale_job);
  CHECK((*js)->wait(first));
  CHECK((*js)->status(jobs::job_handle{}).error().code == sl::errc::stale_job);
  CHECK((*js)->wait(jobs::job_handle{}));
  REQUIRE((*js)->stop());
}

TEST_CASE("RT-INV-JOB-03: more than 8 dependencies, an empty body and submits after stop are "
          "refused") {
  rtt::scripted_platform plat;
  auto js = jobs::job_system::start(plat.get(), config(1, 64));
  REQUIRE(js);
  const jobs::job_handle h = (*js)->submit(noop());
  std::array<jobs::dependency, 9> nine{};
  nine.fill(jobs::dependency::on(h));
  auto too_many = (*js)->try_submit(noop(), nine);
  REQUIRE_FALSE(too_many);
  CHECK(too_many.error().code == sl::errc::invalid_argument);
  CHECK((*js)->try_submit(noop(), std::span(nine).first(8)));
  auto empty = (*js)->try_submit(jobs::job_desc{});
  REQUIRE_FALSE(empty);
  CHECK(empty.error().code == sl::errc::invalid_argument);
  REQUIRE((*js)->stop());
  CHECK((*js)->stop()); // a second stop is ok
  auto late = (*js)->try_submit(noop());
  REQUIRE_FALSE(late);
  CHECK(late.error().code == sl::errc::scheduler_stopped);
  const std::uint64_t rejected = (*js)->stats().rejected;
  CHECK((*js)->submit(noop()) == jobs::job_handle{});
  CHECK((*js)->stats().rejected == rejected + 1);
  CHECK(jobs::job_system::start(plat.get(), config(65, 64)).error().code ==
        sl::errc::invalid_argument);
  CHECK(jobs::job_system::start(plat.get(), config(1, 0)).error().code ==
        sl::errc::invalid_argument);
}
