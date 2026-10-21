// ws_deque, the Chase-Lev work-stealing deque, and make_ws_queue (WO-086, SL-ARCH-003 ARCH3-R072,
// R073, ADR-0024; episode 088). Every atomic operation is seq_cst except the two of push: the cell
// store is relaxed and the bottom_ store release, paired with the seq_cst bottom_ load of steal.
// pop publishes bottom - 1 before it reads top_ (seq_cst store then seq_cst load, so the owner
// and a thief cannot both miss each other), and the last element is claimed by a seq_cst
// compare_exchange_strong on top_, as every steal is. No standalone fence (TSan).
//
// Growth: when bottom - top reaches the ring size the owner allocates a ring twice as large,
// copies [top, bottom) and publishes it through ring_ (seq_cst) before the bottom_ release store
// of the push, so a thief that sees the new bottom loads the new ring. The old ring stays intact
// (the owner never writes it again) for a thief that loaded it earlier. A thief loads ring_ only
// after it found the deque non-empty, between incrementing and decrementing readers_; quiesce()
// frees the retired rings only when readers_ is 0 (seq_cst), so no thief holds one.

#include "sl/runtime/ws_deque.hpp"

#include "sl/core/assert.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/work_queue.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <thread>
#include <utility>

namespace sl::runtime::jobs {

namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays): a nothrow ring
std::unique_ptr<std::atomic<std::uint32_t>[]> allocate_cells(std::size_t n) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays): a nothrow ring
  return std::unique_ptr<std::atomic<std::uint32_t>[]>(new (std::nothrow)
                                                           std::atomic<std::uint32_t>[n]);
}

std::size_t slot(std::int64_t count, std::size_t mask) noexcept {
  return static_cast<std::size_t>(count) & mask;
}

} // namespace

result<std::unique_ptr<ws_deque>> ws_deque::create(std::size_t capacity) noexcept {
  if (capacity == 0) {
    return make_error(errc::invalid_argument, "ws_deque::create: capacity 0");
  }
  const std::size_t max_cells = std::bit_ceil(std::min(capacity, ws_deque_max_cells));
  const std::size_t initial = std::min(ws_deque_initial_cells, max_cells);
  std::unique_ptr<ws_deque> deque(new (std::nothrow) ws_deque(max_cells));
  if (!deque) {
    return make_error(errc::out_of_memory, "ws_deque::create: deque allocation failed");
  }
  ring &first = deque->rings_[0];
  first.cells = allocate_cells(initial);
  if (!first.cells) {
    return make_error(errc::out_of_memory, "ws_deque::create: ring allocation failed");
  }
  first.size = initial;
  first.mask = initial - 1;
  deque->ring_.store(&first);
  return deque;
}

bool ws_deque::owned_by_caller() noexcept {
  const std::thread::id me = std::this_thread::get_id();
  std::thread::id expected{};
  if (owner_.compare_exchange_strong(expected, me)) {
    return true;
  }
  return expected == me;
}

ws_deque::ring *ws_deque::grow(ring &old, std::int64_t top, std::int64_t bottom) noexcept {
  if (old.size >= max_cells_ || current_ + 1 >= max_rings) {
    return nullptr;
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): current_ + 1 < max_rings
  ring &next = rings_[current_ + 1];
  next.cells = allocate_cells(old.size * 2);
  if (!next.cells) {
    return nullptr;
  }
  next.size = old.size * 2;
  next.mask = next.size - 1;
  for (std::int64_t i = top; i < bottom; ++i) {
    next.cells[slot(i, next.mask)].store(old.cells[slot(i, old.mask)].load());
  }
  ring_.store(&next);
  ++current_;
  return &next;
}

bool ws_deque::push(std::uint32_t index) noexcept {
  SL_DEBUG_ASSERT(owned_by_caller(), "ws_deque push off its owner worker");
  const std::int64_t b = bottom_.load();
  const std::int64_t t = top_.load();
  ring *r = ring_.load();
  if (std::cmp_greater_equal(b - t, r->size)) {
    r = grow(*r, t, b);
    if (r == nullptr) {
      return false;
    }
  }
  // pairs: bottom_ release store below (published to steal's seq_cst bottom_ load)
  r->cells[slot(b, r->mask)].store(index, std::memory_order_relaxed);
  // pairs: steal's seq_cst bottom_ load (and pop's own seq_cst loads)
  bottom_.store(b + 1, std::memory_order_release);
  return true;
}

std::optional<std::uint32_t> ws_deque::pop() noexcept {
  SL_DEBUG_ASSERT(owned_by_caller(), "ws_deque pop off its owner worker");
  const std::int64_t b = bottom_.load() - 1;
  const ring *r = ring_.load();
  bottom_.store(b);
  std::int64_t t = top_.load();
  if (t > b) {
    bottom_.store(b + 1); // empty: restore
    return std::nullopt;
  }
  const std::uint32_t value = r->cells[slot(b, r->mask)].load();
  if (t == b) {
    // the last element: race the thieves for it
    const bool won = top_.compare_exchange_strong(t, t + 1);
    bottom_.store(b + 1);
    if (!won) {
      return std::nullopt;
    }
  }
  return value;
}

std::optional<std::uint32_t> ws_deque::steal() noexcept {
  std::int64_t t = top_.load();
  const std::int64_t b = bottom_.load();
  if (t >= b) {
    return std::nullopt;
  }
  readers_.fetch_add(1);
  const ring *r = ring_.load();
  const std::uint32_t value = r->cells[slot(t, r->mask)].load();
  readers_.fetch_sub(1);
  if (!top_.compare_exchange_strong(t, t + 1)) {
    return std::nullopt;
  }
  return value;
}

void ws_deque::quiesce() noexcept {
  if (readers_.load() != 0) {
    return;
  }
  for (; freed_ < current_; ++freed_) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): freed_ < current_
    rings_[freed_].cells.reset();
  }
}

std::size_t ws_deque::cells() const noexcept { return ring_.load()->size; }

std::size_t ws_deque::retired_rings() const noexcept { return current_ - freed_; }

result<std::unique_ptr<work_queue>> make_ws_queue(std::size_t capacity) noexcept {
  auto deque = ws_deque::create(capacity);
  if (!deque) {
    return std::unexpected(std::move(deque.error()));
  }
  return std::unique_ptr<work_queue>(std::move(*deque));
}

} // namespace sl::runtime::jobs
