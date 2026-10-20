#pragma once

// Synchronization primitives (WO-080, SL-ARCH-003 ARCH3-R057 to R060, ADR-0024; episode 087).
// Built only on std::atomic (with wait and notify) and the std synchronization types; no fence,
// no volatile, no inline assembly, no raw futex. Every wait spins at most spin_limit iterations
// (spin_wait) and then parks on std::atomic::wait. cpu_fence is one-shot (released once when its
// count reaches zero, never reset; seam V07-4 puts a gpu_fence beside it); job_counter may be
// reset only while no waiter is registered (invalid_state otherwise); event_count keeps its
// epoch and sleeper count seq_cst so a notify between prepare_wait and wait is never lost.
// Layer L2: includes only L0 runtime headers. Thread safety (`thread_safety`): every member of
// cpu_fence, job_counter, event_count and counting_semaphore is any-thread; a spin_wait is a
// local of one waiting thread.

#include "sl/core/error.hpp"

#include <atomic>
#include <cstdint>
#include <semaphore>
#include <thread>

namespace sl::runtime {

/// SL-ARCH-003 `limits` spin_limit: a wait spins at most this many iterations, then parks.
inline constexpr std::uint32_t spin_limit = 64;

/// The bounded spin of one waiting thread: spin() returns true (after one relax step) for the
/// first `limit` calls and false afterwards, when the caller must park.
class spin_wait {
public:
  explicit spin_wait(std::uint32_t limit = spin_limit) noexcept : limit_(limit) {}

  /// One spin iteration; false once `limit` iterations have passed (park now).
  [[nodiscard]] bool spin() noexcept {
    if (count_ >= limit_) {
      return false;
    }
    ++count_;
    std::this_thread::yield();
    return true;
  }

  /// The iterations spun since construction or the last reset(), at most `limit`.
  [[nodiscard]] std::uint32_t count() const noexcept { return count_; }

  void reset() noexcept { count_ = 0; }

private:
  std::uint32_t limit_;
  std::uint32_t count_ = 0;
};

/// A one-shot CPU fence: released when `count` signals arrived; waiters are released once;
/// signals after the release are ignored. Never reset.
class cpu_fence {
public:
  explicit cpu_fence(std::uint32_t count) noexcept : remaining_(count) {}

  /// Counts `n` signals (clamped to the remaining count); the signal that reaches zero releases
  /// every waiter. A signal after the release has no effect.
  void signal(std::uint32_t n = 1) noexcept;
  /// Returns once the fence is released (spin, then park). Writes before the releasing signal()
  /// happen before the return.
  void wait() const noexcept;
  [[nodiscard]] bool is_signalled() const noexcept;

private:
  std::atomic<std::uint32_t> remaining_;
};

/// A counter of outstanding work: add, sub, wait until zero. reset() sets it to zero and is
/// invalid_state while a waiter is registered in wait_zero().
class job_counter {
public:
  void add(std::uint32_t n = 1) noexcept;
  /// Subtracts `n` (a Debug assertion when n exceeds the value; Release clamps at zero); reaching
  /// zero releases every waiter.
  void sub(std::uint32_t n = 1) noexcept;
  [[nodiscard]] std::uint32_t value() const noexcept;
  /// Returns once the value is observed zero (spin, then park); the waiter is registered for its
  /// whole duration.
  void wait_zero() const noexcept;
  /// Sets the value to zero; invalid_state when a waiter is registered.
  [[nodiscard]] result<void> reset() noexcept;
  /// The number of threads inside wait_zero() now (diagnostics and tests).
  [[nodiscard]] std::uint32_t waiters() const noexcept;

private:
  std::atomic<std::uint32_t> value_{0};
  mutable std::atomic<std::uint32_t> waiters_{0};
};

/// The counting semaphore of ARCH3-R057, used as is.
using counting_semaphore = std::counting_semaphore<>;

/// An event count: a waiter calls prepare_wait(), re-checks its condition, then wait(key) or
/// cancel_wait(); a notifier changes the condition, then notify_one() or notify_all(). The epoch
/// and the sleeper count are seq_cst, so a notify after prepare_wait() always ends that wait.
class event_count {
public:
  using key = std::uint64_t;

  /// Registers the caller as a sleeper and returns the current epoch.
  [[nodiscard]] key prepare_wait() noexcept;
  /// Withdraws the registration of prepare_wait() without waiting.
  void cancel_wait() noexcept;
  /// Returns once the epoch differs from `k` (spin, then park) and withdraws the registration.
  void wait(key k) noexcept;
  /// Advances the epoch and wakes one parked sleeper.
  void notify_one() noexcept;
  /// Advances the epoch and wakes every parked sleeper.
  void notify_all() noexcept;

private:
  std::atomic<std::uint64_t> epoch_{0};
  std::atomic<std::uint32_t> sleepers_{0};
};

} // namespace sl::runtime
