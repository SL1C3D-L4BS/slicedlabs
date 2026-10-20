// RT-INV-JOB-07 (SL-ARCH-003 ARCH3-R069, `limits` watchdog_ms; ADR-0024): a planted lost wakeup
// makes the watchdog report deadlock_suspected naming the stuck job within watchdog_ms (500 ms
// here). The plant is a work_queue factory whose queues hide what is pushed while armed: a job
// running on a worker submits "stuck_job", which lands hidden in that worker's local queue, so no
// thread is ever woken for it. The owner then waits for it. The watchdog (on the steady clock)
// must log one fatal runtime.jobs event whose worker list names stuck_job as the owner's wait
// target and then fail SL_ASSERT with "deadlock_suspected", captured by a scoped_assert_handler;
// a rescuer thread then reveals the hidden job and submits a job to wake the owner. Skipped when
// the watchdog is compiled out (Release).

#include "sl/core/assert.hpp"
#include "sl/core/error.hpp"
#include "sl/core/log.hpp"
#include "sl/runtime/faults.hpp"
#include "sl/runtime/headless_platform.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/platform.hpp"
#include "sl/runtime/sync.hpp"
#include "sl/runtime/work_queue.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace jobs = sl::runtime::jobs;
namespace rtt = sl::runtime::testing;
using namespace std::chrono_literals;

struct plant {
  std::atomic<bool> hide{false};
};

// A locked_deque that diverts pushes into a hidden list while the plant hides; reveal() moves
// them back.
class hiding_queue final : public jobs::work_queue {
public:
  hiding_queue(plant &p, std::unique_ptr<jobs::work_queue> inner)
      : plant_(&p), inner_(std::move(inner)) {}

  bool push(std::uint32_t index) noexcept override {
    if (plant_->hide.load()) {
      const std::scoped_lock lock(mutex_);
      hidden_.push_back(index);
      return true;
    }
    return inner_->push(index);
  }
  std::optional<std::uint32_t> pop() noexcept override { return inner_->pop(); }
  std::optional<std::uint32_t> steal() noexcept override { return inner_->steal(); }
  [[nodiscard]] std::size_t capacity() const noexcept override { return inner_->capacity(); }

  void reveal() {
    const std::scoped_lock lock(mutex_);
    for (const std::uint32_t index : hidden_) {
      static_cast<void>(inner_->push(index));
    }
    hidden_.clear();
  }

private:
  plant *plant_;
  std::unique_ptr<jobs::work_queue> inner_;
  std::mutex mutex_;
  std::vector<std::uint32_t> hidden_;
};

struct capture {
  std::atomic<bool> fired{false};
  std::atomic<std::int64_t> at_ns{0};
  std::mutex mutex;
  std::string message;
};

std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

} // namespace

TEST_CASE("RT-INV-JOB-07: a planted lost wakeup makes the watchdog report deadlock_suspected "
          "naming the stuck job within watchdog_ms") {
  if (!rt::faults_compiled) {
    MESSAGE("watchdog compiled out (Release)");
    return;
  }
  rtt::scripted_platform base;
  rt::steady_clock_source steady;
  rt::platform plat(steady, base.get().wall, base.get().threads, base.get().input, base.get().files,
                    base.get().win, base.get().sink, 1);

  plant planted;
  std::mutex queues_mutex;
  std::vector<hiding_queue *> queues;
  jobs::job_config c;
  c.workers = 2;
  c.watchdog_ms = 500;
  c.make_queue = [&](std::size_t capacity) -> sl::result<std::unique_ptr<jobs::work_queue>> {
    auto inner = jobs::make_locked_queue(capacity);
    if (!inner) {
      return std::unexpected(inner.error());
    }
    auto q = std::make_unique<hiding_queue>(planted, std::move(*inner));
    const std::scoped_lock lock(queues_mutex);
    queues.push_back(q.get());
    return std::unique_ptr<jobs::work_queue>(std::move(q));
  };

  auto events = std::make_shared<sl::log::memory_sink>(64);
  sl::log::default_logger().add_sink(events);
  capture got;
  {
    const sl::scoped_assert_handler handler([&got](const sl::assert_report &report) {
      const std::scoped_lock lock(got.mutex);
      got.message = std::string(report.message);
      got.at_ns.store(now_ns());
      got.fired.store(true);
    });
    auto js = jobs::job_system::start(plat, std::move(c));
    REQUIRE(js);
    jobs::job_system *sys = js->get();

    std::atomic<std::uint64_t> stuck_packed{0};
    rt::cpu_fence spawned(1);
    planted.hide.store(true);
    // the owner waits on a fence, not through the job system, so it cannot run the spawner
    // itself: the spawner runs on a worker and its child lands in that worker's hidden queue
    sys->submit({.fn =
                     [sys, &stuck_packed, &spawned](jobs::job_context &) noexcept {
                       const jobs::job_handle h = sys->submit(
                           {.fn = [](jobs::job_context &) noexcept {}, .name = "stuck_job"});
                       stuck_packed.store((std::uint64_t{h.generation} << 32U) | h.index);
                       spawned.signal();
                     },
                 .name = "spawner"});
    spawned.wait();
    const std::int64_t last_completion = now_ns();
    planted.hide.store(false);
    const std::uint64_t packed = stuck_packed.load();
    const jobs::job_handle stuck{.index = static_cast<std::uint32_t>(packed & 0xFFFFFFFFU),
                                 .generation = static_cast<std::uint32_t>(packed >> 32U)};
    REQUIRE(stuck.generation != 0);

    std::jthread rescuer([&] {
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while (!got.fired.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
      }
      {
        const std::scoped_lock lock(queues_mutex);
        for (hiding_queue *q : queues) {
          q->reveal();
        }
      }
      // a push wakes every parked thread; the owner's helping wait then steals the stuck job
      static_cast<void>(sys->submit({.fn = [](jobs::job_context &) noexcept {}, .name = "wake"}));
    });
    CHECK(sys->wait(stuck));
    rescuer.join();
    REQUIRE((*js)->stop());

    REQUIRE(got.fired.load());
    const std::int64_t after_ms = (got.at_ns.load() - last_completion) / 1000000;
    INFO("reported " << after_ms << " ms after the last completion");
    CHECK(after_ms >= 400); // the last completion was a little before last_completion
    CHECK(after_ms <= 1500);
    const std::scoped_lock lock(got.mutex);
    CHECK(got.message == "deadlock_suspected");
  }
  sl::log::default_logger().remove_sink(events.get());

  int reports = 0;
  for (const auto &e : events->records()) {
    if (e.system != "runtime.jobs" || e.message != "deadlock_suspected") {
      continue;
    }
    ++reports;
    CHECK(e.level == sl::log::level::fatal);
    const auto *workers = e.find("workers");
    REQUIRE(workers != nullptr);
    const auto &text = std::get<std::string>(*workers);
    INFO(text);
    CHECK(text.find("owner job=- depth=0 wait=stuck_job") != std::string::npos);
    CHECK(text.find("w0 job=") != std::string::npos);
    CHECK(text.find("w1 job=") != std::string::npos);
    const auto *code = e.find("code");
    REQUIRE(code != nullptr);
    CHECK(std::get<std::string>(*code) == "deadlock_suspected");
  }
  CHECK(reports == 1);
}

TEST_CASE("RT-INV-JOB-07: the watchdog stays silent while jobs complete") {
  if (!rt::faults_compiled) {
    MESSAGE("watchdog compiled out (Release)");
    return;
  }
  rtt::scripted_platform base;
  rt::steady_clock_source steady;
  rt::platform plat(steady, base.get().wall, base.get().threads, base.get().input, base.get().files,
                    base.get().win, base.get().sink, 1);
  std::atomic<bool> fired{false};
  const sl::scoped_assert_handler handler(
      [&fired](const sl::assert_report &) { fired.store(true); });
  jobs::job_config c;
  c.workers = 2;
  c.watchdog_ms = 200;
  auto js = jobs::job_system::start(plat, std::move(c));
  REQUIRE(js);
  // 400 ms of a steady stream of short jobs, then 400 ms idle: neither is a deadlock
  const auto end = std::chrono::steady_clock::now() + 400ms;
  while (std::chrono::steady_clock::now() < end) {
    REQUIRE((*js)->wait(
        (*js)->submit({.fn = [](jobs::job_context &) noexcept { std::this_thread::sleep_for(1ms); },
                       .name = "tick"})));
  }
  std::this_thread::sleep_for(400ms);
  CHECK_FALSE(fired.load());
  REQUIRE((*js)->stop());
}
