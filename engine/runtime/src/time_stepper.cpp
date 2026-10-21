// The rational fixed stepper (WO-084, SL-ARCH-003 ARCH3-R078, R080). The accumulator is in units
// of ns x num and one tick costs den x 10^9 (below 2^62 for any 32-bit den). delta x num can reach
// 95 bits, so advance multiplies into a pair of 64-bit words and divides by the tick cost with a
// shift-subtract long division: exact for every delta, no unsigned __int128 and no floating point.

#include "sl/core/assert.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/time.hpp"

#include <cstdint>
#include <limits>
#include <numeric>

namespace sl::runtime::time {

namespace {

constexpr std::uint64_t ns_per_second = 1'000'000'000;

struct wide {
  std::uint64_t hi = 0;
  std::uint64_t lo = 0;
};

// a x b as 128 bits from 32-bit halves.
constexpr wide multiply(std::uint64_t a, std::uint64_t b) noexcept {
  constexpr std::uint64_t mask = 0xFFFF'FFFFULL;
  const std::uint64_t a_lo = a & mask;
  const std::uint64_t a_hi = a >> 32U;
  const std::uint64_t b_lo = b & mask;
  const std::uint64_t b_hi = b >> 32U;
  const std::uint64_t ll = a_lo * b_lo;
  const std::uint64_t lh = a_lo * b_hi;
  const std::uint64_t hl = a_hi * b_lo;
  const std::uint64_t hh = a_hi * b_hi;
  const std::uint64_t mid = (ll >> 32U) + (lh & mask) + (hl & mask);
  return wide{.hi = hh + (lh >> 32U) + (hl >> 32U) + (mid >> 32U),
              .lo = (mid << 32U) | (ll & mask)};
}

constexpr wide add(wide a, std::uint64_t b) noexcept {
  const std::uint64_t lo = a.lo + b;
  return wide{.hi = a.hi + (lo < b ? 1U : 0U), .lo = lo};
}

struct quotient {
  std::uint64_t q = 0; // saturated at the uint64 maximum
  std::uint64_t r = 0;
};

// n / d and n % d for d below 2^63 (so the running remainder never overflows when shifted).
constexpr quotient divide(wide n, std::uint64_t d) noexcept {
  if (n.hi == 0) {
    return quotient{.q = n.lo / d, .r = n.lo % d}; // every clamped frame delta
  }
  std::uint64_t r = 0;
  wide q;
  for (unsigned step = 0; step < 128U; ++step) {
    const unsigned bit = 127U - step;
    const unsigned shift = bit & 63U;
    const std::uint64_t word = bit >= 64U ? n.hi : n.lo;
    r = (r << 1U) | ((word >> shift) & 1U);
    if (r >= d) {
      r -= d;
      if (bit >= 64U) {
        q.hi |= std::uint64_t{1} << shift;
      } else {
        q.lo |= std::uint64_t{1} << shift;
      }
    }
  }
  return quotient{.q = q.hi != 0 ? std::numeric_limits<std::uint64_t>::max() : q.lo, .r = r};
}

static_assert(divide(multiply(250'000'000, 60), ns_per_second).q == 15);
static_assert(divide(add(multiply(~std::uint64_t{0}, ~std::uint64_t{0}), 0), 3).r == 0);
constexpr quotient int64_max_at_60_hz =
    divide(multiply(0x7FFF'FFFF'FFFF'FFFFULL, 60), ns_per_second);
static_assert(int64_max_at_60_hz.q == 553'402'322'211ULL);
static_assert(int64_max_at_60_hz.r == 286'548'420ULL);

} // namespace

result<fixed_stepper> fixed_stepper::make(tick_rate rate, std::uint32_t max_catchup) {
  if (rate.num == 0 || rate.den == 0) {
    return make_error(errc::invalid_tick_rate,
                      "fixed_stepper::make: tick_rate num and den must be above 0");
  }
  const std::uint32_t g = std::gcd(rate.num, rate.den);
  return fixed_stepper(tick_rate{.num = rate.num / g, .den = rate.den / g}, max_catchup);
}

fixed_stepper::fixed_stepper(tick_rate rate, std::uint32_t max_catchup) noexcept
    : rate_(rate), max_catchup_(max_catchup), cost_(std::uint64_t{rate.den} * ns_per_second) {}

std::uint32_t fixed_stepper::advance(nanos delta) noexcept {
  if (delta.count() <= 0) {
    return 0;
  }
  const quotient t =
      divide(add(multiply(static_cast<std::uint64_t>(delta.count()), rate_.num), acc_), cost_);
  acc_ = t.r;
  if (t.q <= max_catchup_) {
    return static_cast<std::uint32_t>(t.q);
  }
  const std::uint64_t extra = t.q - max_catchup_;
  dropped_ = extra > std::numeric_limits<std::uint64_t>::max() - dropped_
                 ? std::numeric_limits<std::uint64_t>::max()
                 : dropped_ + extra;
  return max_catchup_;
}

fixed_stepper::state fixed_stepper::capture() const noexcept {
  return state{.accumulator = acc_, .dropped_ticks = dropped_};
}

void fixed_stepper::restore(const state &s) noexcept {
  SL_DEBUG_ASSERT(s.accumulator < cost_,
                  "fixed_stepper::restore: accumulator at or above one tick");
  acc_ = s.accumulator % cost_;
  dropped_ = s.dropped_ticks;
}

} // namespace sl::runtime::time
