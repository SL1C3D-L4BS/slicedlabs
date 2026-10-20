// The job system watchdog (WO-082, SL-ARCH-003 ARCH3-R069, `limits` watchdog_ms; ADR-0024). In
// SL_RUNTIME_FAULTS builds (Debug and sanitizers) a thread from platform.threads (named
// slrt<instance>-wd) checks every 100 ms: when jobs are pending and no job completed for
// watchdog_ms on the platform clock, it logs one fatal event (system runtime.jobs, message
// deadlock_suspected) listing each worker's current job, queue depth and wait target, plus the
// owner thread and the shared queues, then SL_ASSERT fails with "deadlock_suspected". It reports
// again only after a completion. Release builds start no watchdog thread.

#include "sl/core/assert.hpp"
#include "sl/core/log.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/faults.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/platform.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace sl::runtime::jobs {

result<void> job_system::start_watchdog() noexcept {
#if SL_RUNTIME_FAULTS
  if (watchdog_ms_ == 0) {
    return {};
  }
  auto handle = platform_->threads.spawn("wd", [this] noexcept { watchdog_main(); });
  if (!handle) {
    return std::unexpected(std::move(handle.error()));
  }
  watchdog_ = std::move(*handle);
#endif
  return {};
}

void job_system::stop_watchdog() noexcept {
  {
    const std::scoped_lock lock(watchdog_mutex_);
    watchdog_exit_ = true;
  }
  watchdog_cv_.notify_all();
  watchdog_.join();
}

void job_system::watchdog_main() noexcept {
  std::uint64_t seen = completions_.load();
  nanos last = platform_->clock.now();
  bool reported = false;
  std::unique_lock lock(watchdog_mutex_);
  while (!watchdog_exit_) {
    if (watchdog_cv_.wait_for(lock, std::chrono::milliseconds(watchdog_period_ms),
                              [this] noexcept { return watchdog_exit_; })) {
      break;
    }
    const nanos now = platform_->clock.now();
    const std::uint64_t completed = completions_.load();
    if (pending_.load() == 0 || completed != seen) {
      seen = completed;
      last = now;
      reported = false;
      continue;
    }
    const auto silent = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
    if (!reported && std::cmp_greater_equal(silent, watchdog_ms_)) {
      reported = true;
      lock.unlock();
      watchdog_report(static_cast<std::uint64_t>(silent));
      lock.lock();
    }
  }
}

void job_system::watchdog_report(std::uint64_t silent_ms) noexcept {
  std::string text;
  for (std::uint32_t k = 0; k <= workers_; ++k) {
    const thread_slot &s = slots_[k];
    const bool owner = k == workers_;
    text += owner ? std::string("owner") : "w" + std::to_string(k);
    text += " job=";
    text += record_name(s.current.load());
    text += " depth=";
    text += owner ? std::to_string(main_queue_->size()) : std::to_string(s.depth.load());
    text += " wait=";
    text += record_name(s.waiting.load());
    text += "; ";
  }
  text += "frame_queue=" + std::to_string(frame_queue_->size());
  text += " background_queue=" + std::to_string(background_queue_->size());
  sl::log::emit(sl::log::level::fatal, "runtime.jobs", "deadlock_suspected",
                {{"code", sl::to_string(errc::deadlock_suspected)},
                 {"instance", instance_id_},
                 {"pending", pending_.load()},
                 {"silent_ms", silent_ms},
                 {"watchdog_ms", watchdog_ms_},
                 {"workers", std::string_view{text}}});
  const bool progressing = false;
  SL_ASSERT(progressing, "deadlock_suspected");
}

} // namespace sl::runtime::jobs
