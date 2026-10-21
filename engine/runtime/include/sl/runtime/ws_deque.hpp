#pragma once

// The Chase-Lev work-stealing deque (WO-086, SL-ARCH-003 ARCH3-R072, R073, ADR-0024; episode 088).
// top_ and bottom_ are std::atomic<std::int64_t> running counts on separate 64-byte lines; the
// ring holds std::atomic<std::uint32_t> cells (cell = count & mask). push stores the cell relaxed
// and then bottom_ release; pop and steal use seq_cst on bottom_, top_ and the claiming
// compare_exchange_strong; there is no standalone fence (ThreadSanitizer does not model fences).
// The ring starts at min(4096, max) cells and doubles up to max = min(bit_ceil(capacity), 65536)
// (`limits` ws_deque_initial_cells, ws_deque_max_cells); push returns false at the maximum (the
// job system then uses its injection queue). A grown ring is retired, not freed: quiesce() frees
// the retired rings when no thief is reading one (job_system::quiesce calls it with no job in
// flight). Layer L3: includes work_queue.hpp only. Source: src/queue_ws.cpp.
//
// Thread safety (`thread_safety`): push and pop are worker-only, from the owner worker (the first
// thread that pushes or pops; another thread is a Debug assertion); steal, capacity and cells are
// any-thread; quiesce is called with no push or pop in flight (job_system::quiesce).

#include "sl/core/error.hpp"
#include "sl/runtime/work_queue.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>

namespace sl::runtime::jobs {

// SL-ARCH-003 `limits` (ARCH3-R174).
inline constexpr std::size_t ws_deque_initial_cells = 4096;
inline constexpr std::size_t ws_deque_max_cells = 65536;

class ws_deque final : public work_queue {
public:
  /// A deque that holds up to min(bit_ceil(capacity), 65536) indices; invalid_argument for
  /// capacity 0, out_of_memory when the first ring cannot be allocated.
  [[nodiscard]] static result<std::unique_ptr<ws_deque>> create(std::size_t capacity) noexcept;

  ws_deque(const ws_deque &) = delete;
  ws_deque(ws_deque &&) = delete;
  ws_deque &operator=(const ws_deque &) = delete;
  ws_deque &operator=(ws_deque &&) = delete;
  ~ws_deque() override = default;

  /// Owner worker: appends `index` at the bottom; grows the ring when full; false at the maximum
  /// or when the grown ring cannot be allocated (nothing changes).
  [[nodiscard]] bool push(std::uint32_t index) noexcept override;
  /// Owner worker: takes the newest index; nullopt when empty or when a thief won the last one.
  [[nodiscard]] std::optional<std::uint32_t> pop() noexcept override;
  /// Any thread: takes the oldest index; nullopt when empty or when the race was lost.
  [[nodiscard]] std::optional<std::uint32_t> steal() noexcept override;
  /// The maximum number of indices held (the largest ring).
  [[nodiscard]] std::size_t capacity() const noexcept override { return max_cells_; }
  /// Frees the retired rings unless a thief is reading one (they stay for the next quiesce).
  void quiesce() noexcept override;

  /// Any thread: the cells of the current ring.
  [[nodiscard]] std::size_t cells() const noexcept;
  /// Owner or quiescent: the rings retired by growth and not yet freed.
  [[nodiscard]] std::size_t retired_rings() const noexcept;

private:
  // One ring: written by the owner before ring_ publishes it, then read-only until freed.
  struct ring {
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays): a nothrow ring
    std::unique_ptr<std::atomic<std::uint32_t>[]> cells;
    std::size_t size = 0;
    std::size_t mask = 0;
  };
  // 1 to 65536 cells by doubling: at most 17 rings over the deque's life.
  static constexpr std::size_t max_rings = 17;

  explicit ws_deque(std::size_t max_cells) noexcept : max_cells_(max_cells) {}
  [[nodiscard]] ring *grow(ring &old, std::int64_t top, std::int64_t bottom) noexcept;
  [[nodiscard]] bool owned_by_caller() noexcept;

  alignas(64) std::atomic<std::int64_t> top_{0};
  alignas(64) std::atomic<std::int64_t> bottom_{0};
  alignas(64) std::atomic<ring *> ring_{nullptr};
  std::size_t max_cells_;
  std::size_t current_ = 0; // index of the current ring in rings_ (owner only)
  std::size_t freed_ = 0;   // rings_[0, freed_) were freed by quiesce
  std::atomic<std::thread::id> owner_{};
  alignas(64) std::atomic<std::uint32_t> readers_{0}; // thieves between the ring load and the read
  std::array<ring, max_rings> rings_{};
};

} // namespace sl::runtime::jobs
