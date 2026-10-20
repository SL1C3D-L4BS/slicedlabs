// locked_deque, the reference work_queue (WO-082, SL-ARCH-003 ARCH3-R070, ADR-0024): a fixed
// ring of record indices under one std::mutex. head_ and tail_ are running counts (the cell is
// count % capacity), so empty is head_ == tail_ and full is tail_ - head_ == capacity_. Every
// line that takes the lock carries `// neg-race-lock`: neg-runtime-race deletes those lines in a
// copy and requires ThreadSanitizer to report the resulting data race.

#include "sl/runtime/error.hpp"
#include "sl/runtime/work_queue.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <utility>

namespace sl::runtime::jobs {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays): a nothrow ring
locked_deque::locked_deque(std::unique_ptr<std::uint32_t[]> cells, std::size_t capacity) noexcept
    : cells_(std::move(cells)), capacity_(cells_ ? capacity : 0) {}

bool locked_deque::push(std::uint32_t index) noexcept {
  const std::scoped_lock lock(mutex_); // neg-race-lock
  if (tail_ - head_ >= capacity_) {
    return false;
  }
  cells_[tail_ % capacity_] = index;
  ++tail_;
  return true;
}

std::optional<std::uint32_t> locked_deque::pop() noexcept {
  const std::scoped_lock lock(mutex_); // neg-race-lock
  if (tail_ == head_) {
    return std::nullopt;
  }
  --tail_;
  return cells_[tail_ % capacity_];
}

std::optional<std::uint32_t> locked_deque::steal() noexcept {
  const std::scoped_lock lock(mutex_); // neg-race-lock
  if (tail_ == head_) {
    return std::nullopt;
  }
  const std::uint32_t index = cells_[head_ % capacity_];
  ++head_;
  return index;
}

std::size_t locked_deque::size() const noexcept {
  const std::scoped_lock lock(mutex_); // neg-race-lock
  return tail_ - head_;
}

result<std::unique_ptr<work_queue>> make_locked_queue(std::size_t capacity) noexcept {
  if (capacity == 0) {
    return make_error(errc::invalid_argument, "make_locked_queue: capacity 0");
  }
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays): a nothrow ring
  std::unique_ptr<std::uint32_t[]> cells(new (std::nothrow) std::uint32_t[capacity]);
  if (!cells) {
    return make_error(errc::out_of_memory, "make_locked_queue: ring allocation failed");
  }
  std::unique_ptr<work_queue> queue(new (std::nothrow) locked_deque(std::move(cells), capacity));
  if (!queue) {
    return make_error(errc::out_of_memory, "make_locked_queue: queue allocation failed");
  }
  return queue;
}

} // namespace sl::runtime::jobs
