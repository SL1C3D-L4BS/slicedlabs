#pragma once

// Job queues and the scheduling policy seam (WO-082, SL-ARCH-003 ARCH3-R070, ADR-0024; episode
// 086). The job system takes its per-worker queues through `work_queue` (built by a queue
// factory) and its victim, injection-poll, aging and park decisions through `scheduler_policy`.
// `locked_deque` (a mutex and a ring; src/queue_locked.cpp) with the reference policy
// (src/policy_simple.cpp) is the default; make_ws_queue (src/queue_ws.cpp) builds the Chase-Lev
// ws_deque and make_steal_policy (scheduler_stats.hpp) the stealing policy (WO-086). Layer L3:
// includes only L0 runtime headers.
//
// Thread safety (`thread_safety`): work_queue push and pop are worker-only (the owner worker of
// the queue), steal any-thread; locked_deque accepts every call from any thread (it is also the
// multi-producer injection queue). A scheduler_policy is called by the workers of one job system:
// on_start before any worker runs, the other members concurrently from those workers (each passes
// its own index), read_counters from any thread.

#include "sl/core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>

namespace sl::runtime::jobs {

/// A queue of job record indices. push and pop are the owner worker's end (LIFO), steal the other
/// end (FIFO). Not copyable or movable.
class work_queue {
public:
  work_queue() = default;
  work_queue(const work_queue &) = delete;
  work_queue(work_queue &&) = delete;
  work_queue &operator=(const work_queue &) = delete;
  work_queue &operator=(work_queue &&) = delete;
  virtual ~work_queue() = default;

  /// Appends `index` at the owner end; false when the queue is full (nothing changes).
  [[nodiscard]] virtual bool push(std::uint32_t index) noexcept = 0;
  /// Takes the newest index from the owner end; nullopt when empty.
  [[nodiscard]] virtual std::optional<std::uint32_t> pop() noexcept = 0;
  /// Takes the oldest index from the other end; nullopt when empty (or when a race was lost).
  [[nodiscard]] virtual std::optional<std::uint32_t> steal() noexcept = 0;
  /// The number of indices the queue holds when full.
  [[nodiscard]] virtual std::size_t capacity() const noexcept = 0;
  /// Called by job_system::quiesce with no job in flight (a deque may free retired buffers).
  virtual void quiesce() noexcept {}
};

/// The reference queue: a fixed ring under one mutex. Every member is any-thread. The lock lines
/// in src/queue_locked.cpp carry `// neg-race-lock` (neg-runtime-race strips them).
class locked_deque final : public work_queue {
public:
  /// A ring of `capacity` cells; capacity 0 holds nothing (every push fails).
  explicit locked_deque(std::unique_ptr<std::uint32_t[]> cells, std::size_t capacity) noexcept;

  [[nodiscard]] bool push(std::uint32_t index) noexcept override;
  [[nodiscard]] std::optional<std::uint32_t> pop() noexcept override;
  [[nodiscard]] std::optional<std::uint32_t> steal() noexcept override;
  [[nodiscard]] std::size_t capacity() const noexcept override { return capacity_; }
  /// The number of indices held now.
  [[nodiscard]] std::size_t size() const noexcept;

private:
  mutable std::mutex mutex_;
  std::unique_ptr<std::uint32_t[]> cells_;
  std::size_t capacity_;
  std::size_t head_ = 0; // the oldest cell (steal end), as a running count
  std::size_t tail_ = 0; // one past the newest cell (owner end), as a running count
};

/// Builds a queue holding at least the given number of indices (job_config::make_queue).
using queue_factory = std::function<result<std::unique_ptr<work_queue>>(std::size_t)>;

/// A locked_deque of `capacity` cells; invalid_argument for 0, out_of_memory when the ring cannot
/// be allocated.
[[nodiscard]] result<std::unique_ptr<work_queue>> make_locked_queue(std::size_t capacity) noexcept;

/// The work-stealing deque (ws_deque.hpp, src/queue_ws.cpp): at most min(bit_ceil(capacity),
/// 65536) indices; invalid_argument for 0, out_of_memory when the ring cannot be allocated.
[[nodiscard]] result<std::unique_ptr<work_queue>> make_ws_queue(std::size_t capacity) noexcept;

/// Per-worker scheduler counters (diagnostic; never asserted as times).
struct worker_counters {
  std::uint64_t jobs_run = 0;
  std::uint64_t steals = 0;
  std::uint64_t failed_steals = 0;
  std::uint64_t parks = 0;
  std::uint64_t wakes = 0;
  std::uint64_t injection_polls = 0;
  std::uint64_t aging_promotions = 0;

  friend bool operator==(const worker_counters &, const worker_counters &) = default;
};

/// The decisions of the scheduler loop. Not copyable or movable.
class scheduler_policy {
public:
  scheduler_policy() = default;
  scheduler_policy(const scheduler_policy &) = delete;
  scheduler_policy(scheduler_policy &&) = delete;
  scheduler_policy &operator=(const scheduler_policy &) = delete;
  scheduler_policy &operator=(scheduler_policy &&) = delete;
  virtual ~scheduler_policy() = default;

  /// Called once by job_system::start before any worker runs.
  virtual void on_start(std::uint32_t workers, std::uint64_t seed) noexcept = 0;
  /// The worker `self` steals from on its attempt-th try of a round (0-based; a value equal to
  /// `self` or out of range skips the attempt).
  [[nodiscard]] virtual std::uint32_t victim(std::uint32_t self,
                                             std::uint32_t attempt) noexcept = 0;
  /// True when `self` must poll the injection queues before its non-empty local queue;
  /// `local_pops` counts its local pops so far. With an empty local queue the injection queues are
  /// always polled.
  [[nodiscard]] virtual bool poll_injection(std::uint32_t self,
                                            std::uint64_t local_pops) noexcept = 0;
  /// True when a background job that waited `waited_rounds` scheduler rounds goes ahead of frame
  /// jobs at the next pop.
  [[nodiscard]] virtual bool promote_background(std::uint32_t waited_rounds) const noexcept = 0;
  /// Empty scheduling rounds before an idle worker parks.
  [[nodiscard]] virtual std::uint32_t spin_limit() const noexcept = 0;
  virtual void on_steal(std::uint32_t self, bool ok) noexcept = 0;
  virtual void on_park(std::uint32_t self) noexcept = 0;
  virtual void on_wake(std::uint32_t self) noexcept = 0;
  /// Copies the counters of workers [0, out.size()) into `out` (zero for absent workers).
  virtual void read_counters(std::span<worker_counters> out) const noexcept = 0;
};

/// The reference policy (src/policy_simple.cpp): victims (self + 1 + attempt) % workers, injection
/// polled only when the local queue is empty, no aging, spin_limit 64, counters zero. Null when
/// the allocation fails.
[[nodiscard]] std::unique_ptr<scheduler_policy> make_reference_policy() noexcept;

} // namespace sl::runtime::jobs
