#pragma once

// Clocks and logical time (WO-084, SL-ARCH-003 ARCH3-R077 to R083, episode 089, ADR-0026). Time is
// integer nanoseconds and the logical tick a std::uint64_t that advances only by whole fixed steps.
// The fixed stepper keeps a rational tick_rate {num, den} and an accumulator in units of ns x num;
// one tick costs den x 10^9 units, so no floating point enters tick progression and there is no
// drift. The frame clock turns one monotonic clock_source sample per frame into a delta: a sample
// at or before the previous one gives 0 and counts non_monotonic (RFP6), a delta above
// max_frame_delta is clamped and the excess added to dropped_ns, and freeze/thaw makes suspend
// accrue nothing. tick_scope sets the calling thread's sl::log correlation tick and scope.
// Layer L2.
//
// Thread safety (SL-ARCH-003 `thread_safety`): frame_clock, fixed_stepper and logical_clock are
// owner-thread (the frame thread); jobs read the immutable frame_time value (any thread);
// tick_scope is per-thread RAII on any thread.

#include "sl/core/error.hpp"
#include "sl/core/log.hpp"
#include "sl/runtime/platform.hpp"

#include <chrono>
#include <cstdint>
#include <string_view>

namespace sl::runtime::time {

/// The logical tick: 0 after ingest, + 1 per fixed step, never derived from wall time (ARCH3-R077).
using tick = std::uint64_t;

/// Ticks per second num / den (60 Hz by default; 60000/1001 is NTSC 59.94 Hz).
struct tick_rate {
  std::uint32_t num = 60;
  std::uint32_t den = 1;
};

/// The interpolation fraction num / den in [0, 1): the stepper remainder over the cost of a tick.
struct alpha {
  std::uint64_t num = 0;
  std::uint64_t den = 1;
};

/// The `limits` rows max_frame_delta (250 ms) and max_catchup (8 ticks per frame).
inline constexpr nanos max_frame_delta_default = std::chrono::milliseconds(250);
inline constexpr std::uint32_t max_catchup_default = 8;

/// The rational fixed stepper (ARCH3-R078, R080). Owner-thread.
class fixed_stepper {
public:
  /// The capturable state: the accumulator in units of ns x num (below one tick) and the count of
  /// dropped ticks. A state restores only into a stepper of the same rate.
  struct state {
    std::uint64_t accumulator = 0;
    std::uint64_t dropped_ticks = 0;
  };

  /// invalid_tick_rate for num or den 0. The rate is reduced by std::gcd.
  [[nodiscard]] static result<fixed_stepper> make(tick_rate rate,
                                                  std::uint32_t max_catchup = max_catchup_default);

  /// Adds delta x num to the accumulator and returns the whole ticks it now holds, at most
  /// max_catchup; whole ticks beyond the cap are dropped and counted, the remainder stays below one
  /// tick. A delta at or below 0 adds nothing. Exact for every delta (no overflow).
  [[nodiscard]] std::uint32_t advance(nanos delta) noexcept;

  [[nodiscard]] std::uint64_t dropped_ticks() const noexcept { return dropped_; }
  /// The accumulator over the cost of one tick (den x 10^9 of the reduced rate).
  [[nodiscard]] alpha remainder() const noexcept { return alpha{.num = acc_, .den = cost_}; }
  /// The reduced rate.
  [[nodiscard]] tick_rate rate() const noexcept { return rate_; }
  [[nodiscard]] std::uint32_t max_catchup() const noexcept { return max_catchup_; }

  [[nodiscard]] state capture() const noexcept;
  /// Replaces the state; an accumulator at or above one tick (a state of another rate) is a Debug
  /// assertion and is reduced modulo the tick cost.
  void restore(const state &s) noexcept;

private:
  fixed_stepper(tick_rate rate, std::uint32_t max_catchup) noexcept;

  tick_rate rate_;
  std::uint32_t max_catchup_ = max_catchup_default;
  std::uint64_t cost_ = 1;
  std::uint64_t acc_ = 0;
  std::uint64_t dropped_ = 0;
};

/// One clock_source sample per frame becomes the frame delta (ARCH3-R079, R026). Owner-thread.
class frame_clock {
public:
  /// The capturable state.
  struct state {
    std::int64_t previous_ns = 0;
    bool has_previous = false;
    bool frozen = false;
    std::uint64_t non_monotonic = 0;
    std::uint64_t dropped_ns = 0;
    std::uint64_t frame_index = 0;
  };

  /// max_frame_delta must be above 0 (Debug assertion).
  explicit frame_clock(nanos max_frame_delta = max_frame_delta_default) noexcept;

  /// Takes the frame's sample and returns its delta: 0 for the first sample, after thaw and while
  /// frozen; 0 and non_monotonic + 1 for a sample at or before the previous one; clamped to
  /// max_frame_delta with the excess added to dropped_ns. Every call is one frame: frame_index
  /// grows by one. RFP6 (clock.jump): param 0 treats the sample as backward, param p adds a p ms
  /// hitch to the delta before the clamp.
  [[nodiscard]] nanos sample(nanos now) noexcept;
  /// Suspend: samples return 0 until thaw.
  void freeze() noexcept;
  /// Resume: the next sample returns 0 and becomes the new baseline.
  void thaw() noexcept;

  [[nodiscard]] std::uint64_t non_monotonic() const noexcept { return non_monotonic_; }
  [[nodiscard]] nanos dropped_ns() const noexcept;
  /// The number of samples taken: the frame index of the last frame.
  [[nodiscard]] std::uint64_t frame_index() const noexcept { return frame_index_; }
  [[nodiscard]] bool frozen() const noexcept { return frozen_; }
  [[nodiscard]] nanos max_frame_delta() const noexcept { return max_delta_; }

  [[nodiscard]] state capture() const noexcept;
  void restore(const state &s) noexcept;

private:
  nanos max_delta_;
  std::int64_t previous_ns_ = 0;
  bool has_previous_ = false;
  bool frozen_ = false;
  std::uint64_t non_monotonic_ = 0;
  std::uint64_t dropped_ns_ = 0;
  std::uint64_t frame_index_ = 0;
};

/// The logical clock: the current tick, advanced by one per fixed step (ARCH3-R077). Owner-thread.
class logical_clock {
public:
  [[nodiscard]] tick now() const noexcept { return now_; }
  void advance() noexcept { ++now_; }
  /// Snapshot restore only (end_frame).
  void restore(tick t) noexcept { now_ = t; }

private:
  tick now_ = 0;
};

/// The per-frame value handed to jobs (ARCH3-R081): immutable by use, jobs take it by value or
/// const reference. Any thread.
struct frame_time {
  std::uint64_t frame_index = 0;
  tick now = 0;
  nanos delta{0};
  alpha a;
};

/// RAII on any thread (ARCH3-R083): while alive, the thread's sl::log correlation is {id, scope,
/// tick} with id the current correlation id when non-zero, else next_correlation_id(). The scope
/// is runtime/<instance> (Debug assertion) and must outlive the object (the runtime owns it).
class tick_scope {
public:
  tick_scope(tick t, std::string_view scope) noexcept;
  ~tick_scope() = default;
  tick_scope(const tick_scope &) = delete;
  tick_scope &operator=(const tick_scope &) = delete;
  tick_scope(tick_scope &&) = delete;
  tick_scope &operator=(tick_scope &&) = delete;

  [[nodiscard]] const log::correlation &get() const noexcept { return scope_.get(); }

private:
  log::correlation_scope scope_;
};

} // namespace sl::runtime::time
