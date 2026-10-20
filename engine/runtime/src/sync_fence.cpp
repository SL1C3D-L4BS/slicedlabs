// cpu_fence and job_counter (WO-080, SL-ARCH-003 ARCH3-R057, R059, R060, ADR-0024). Every
// atomic operation is seq_cst (the default order), so no `// pairs:` comment is needed; parking
// is std::atomic::wait on the counter itself after spin_limit spins.

#include "sl/core/assert.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/sync.hpp"

#include <atomic>
#include <cstdint>

namespace sl::runtime {

namespace {

// Spins, then parks on `word` until `done(value)` holds. The value is re-read after every wake,
// so a spurious wake or a change that does not satisfy `done` parks again on the new value.
template <class Done>
void spin_then_park(const std::atomic<std::uint32_t> &word, Done done) noexcept {
  spin_wait spinner;
  for (std::uint32_t seen = word.load(); !done(seen); seen = word.load()) {
    if (!spinner.spin()) {
      word.wait(seen);
    }
  }
}

} // namespace

void cpu_fence::signal(std::uint32_t n) noexcept {
  std::uint32_t current = remaining_.load();
  while (current != 0) {
    const std::uint32_t next = n >= current ? 0 : current - n;
    if (remaining_.compare_exchange_weak(current, next)) {
      if (next == 0) {
        remaining_.notify_all();
      }
      return;
    }
  }
}

void cpu_fence::wait() const noexcept {
  spin_then_park(remaining_, [](std::uint32_t value) noexcept { return value == 0; });
}

bool cpu_fence::is_signalled() const noexcept { return remaining_.load() == 0; }

void job_counter::add(std::uint32_t n) noexcept { value_.fetch_add(n); }

void job_counter::sub(std::uint32_t n) noexcept {
  std::uint32_t current = value_.load();
  for (;;) {
    SL_DEBUG_ASSERT(n <= current, "job_counter::sub below zero");
    const std::uint32_t next = n >= current ? 0 : current - n;
    if (value_.compare_exchange_weak(current, next)) {
      if (next == 0 && current != 0) {
        value_.notify_all();
      }
      return;
    }
  }
}

std::uint32_t job_counter::value() const noexcept { return value_.load(); }

void job_counter::wait_zero() const noexcept {
  waiters_.fetch_add(1);
  spin_then_park(value_, [](std::uint32_t value) noexcept { return value == 0; });
  waiters_.fetch_sub(1);
}

result<void> job_counter::reset() noexcept {
  if (waiters_.load() != 0) {
    return make_error(errc::invalid_state, "job_counter::reset with a registered waiter");
  }
  // A waiter that registers after the check sees zero (or is woken by the notify) and returns.
  value_.store(0);
  value_.notify_all();
  return {};
}

std::uint32_t job_counter::waiters() const noexcept { return waiters_.load(); }

} // namespace sl::runtime
