// The frame clock (WO-084, SL-ARCH-003 ARCH3-R079, R026, failpoint RFP6). One monotonic sample
// per frame becomes the frame delta; a sample at or before the previous one is counted and gives
// 0 (the previous sample stays the baseline, so time never runs backward), a delta above
// max_frame_delta is clamped and the excess counted in dropped_ns. Differences are taken in
// unsigned arithmetic and dropped_ns saturates, so no sample sequence overflows.

#include "sl/core/assert.hpp"
#include "sl/runtime/faults.hpp"
#include "sl/runtime/time.hpp"

#include <cstdint>
#include <limits>

namespace sl::runtime::time {

namespace {

constexpr std::uint64_t saturating_add(std::uint64_t a, std::uint64_t b) noexcept {
  constexpr std::uint64_t top = std::numeric_limits<std::uint64_t>::max();
  return b > top - a ? top : a + b;
}

} // namespace

frame_clock::frame_clock(nanos max_frame_delta) noexcept : max_delta_(max_frame_delta) {
  SL_DEBUG_ASSERT(max_frame_delta.count() > 0, "frame_clock: max_frame_delta must be above 0");
}

nanos frame_clock::sample(nanos now) noexcept {
  ++frame_index_;
  if (frozen_) {
    return nanos{0};
  }
  std::uint64_t hitch = 0;
#if SL_RUNTIME_FAULTS
  if (const fault_action jump = visit(failpoint::rfp6); jump.fire) {
    if (jump.param == 0) {
      ++non_monotonic_; // RFP6 param 0: the sample is treated as backward
      return nanos{0};
    }
    hitch = std::uint64_t{jump.param} * 1'000'000U; // RFP6 param p: a p ms hitch
  }
#endif
  const std::int64_t ns = now.count();
  if (!has_previous_) {
    previous_ns_ = ns;
    has_previous_ = true;
    return nanos{0};
  }
  if (ns <= previous_ns_) {
    ++non_monotonic_;
    return nanos{0};
  }
  const std::uint64_t raw = saturating_add(
      static_cast<std::uint64_t>(ns) - static_cast<std::uint64_t>(previous_ns_), hitch);
  previous_ns_ = ns;
  const auto cap = static_cast<std::uint64_t>(max_delta_.count() > 0 ? max_delta_.count() : 0);
  if (raw <= cap) {
    return nanos{static_cast<nanos::rep>(raw)};
  }
  dropped_ns_ = saturating_add(dropped_ns_, raw - cap);
  return nanos{static_cast<nanos::rep>(cap)};
}

void frame_clock::freeze() noexcept { frozen_ = true; }

void frame_clock::thaw() noexcept {
  frozen_ = false;
  has_previous_ = false;
}

nanos frame_clock::dropped_ns() const noexcept {
  constexpr auto rep_max = static_cast<std::uint64_t>(std::numeric_limits<nanos::rep>::max());
  return nanos{static_cast<nanos::rep>(dropped_ns_ < rep_max ? dropped_ns_ : rep_max)};
}

frame_clock::state frame_clock::capture() const noexcept {
  return state{.previous_ns = previous_ns_,
               .has_previous = has_previous_,
               .frozen = frozen_,
               .non_monotonic = non_monotonic_,
               .dropped_ns = dropped_ns_,
               .frame_index = frame_index_};
}

void frame_clock::restore(const state &s) noexcept {
  previous_ns_ = s.previous_ns;
  has_previous_ = s.has_previous;
  frozen_ = s.frozen;
  non_monotonic_ = s.non_monotonic;
  dropped_ns_ = s.dropped_ns;
  frame_index_ = s.frame_index;
}

} // namespace sl::runtime::time
