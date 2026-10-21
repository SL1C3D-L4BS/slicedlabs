// RT-INV-CLOCK-01 (SL-ARCH-003 ARCH3-R078, ADR-0026): integer time and a rational tick rate keep
// exact tick counts. 3600 s of seeded jittered frame deltas, sampled through a frame_clock from a
// manual timeline, give exactly 216000 ticks at 60 Hz, and the rates 30, 50, 120, 144 and
// 60000/1001 give exactly floor(3600 x num / den) with the remainder equal to the oracle. Every
// delta stays below max_frame_delta and below max_catchup ticks, so nothing is dropped.

#include "sl/runtime/time.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>

namespace {

namespace rt = sl::runtime;
namespace time = sl::runtime::time;

struct splitmix64 {
  std::uint64_t state = 0;
  std::uint64_t next() noexcept {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
  }
};

constexpr std::int64_t ns_per_s = 1'000'000'000;
constexpr std::int64_t run_seconds = 3600;

struct outcome {
  std::uint64_t ticks = 0;
  std::uint64_t frames = 0;
  time::alpha remainder;
  std::uint64_t dropped_ticks = 0;
  rt::nanos dropped_ns{0};
};

// Frames of 4 to 36 ms (seeded) until exactly 3600 s, the last frame cut to land on it.
outcome run(time::tick_rate rate, std::uint64_t seed) {
  auto stepper = time::fixed_stepper::make(rate);
  REQUIRE(stepper.has_value());
  time::frame_clock clock;
  time::logical_clock logical;
  splitmix64 rng{seed};
  constexpr std::int64_t end = run_seconds * ns_per_s;
  std::int64_t now = 1'000; // an arbitrary non-zero epoch
  CHECK(clock.sample(rt::nanos{now}) == rt::nanos{0});
  std::int64_t elapsed = 0;
  std::uint64_t altered = 0; // samples whose delta is not the timeline's (one assertion, not 2e5)
  while (elapsed < end) {
    std::int64_t delta = 4'000'000 + static_cast<std::int64_t>(rng.next() % 32'000'001U);
    if (elapsed + delta > end) {
      delta = end - elapsed;
    }
    elapsed += delta;
    now += delta;
    const rt::nanos d = clock.sample(rt::nanos{now});
    altered += d == rt::nanos{delta} ? 0U : 1U;
    const std::uint32_t n = stepper->advance(d);
    for (std::uint32_t i = 0; i < n; ++i) {
      logical.advance();
    }
  }
  CHECK(altered == 0);
  return outcome{.ticks = logical.now(),
                 .frames = clock.frame_index(),
                 .remainder = stepper->remainder(),
                 .dropped_ticks = stepper->dropped_ticks(),
                 .dropped_ns = clock.dropped_ns()};
}

void check_rate(std::uint32_t num, std::uint32_t den, std::uint64_t seed) {
  const outcome o = run(time::tick_rate{.num = num, .den = den}, seed);
  // The oracle: 3600 s x num / den ticks; the remainder in units of ns x num over den x 1e9.
  const std::uint64_t scaled = static_cast<std::uint64_t>(run_seconds) * num;
  CHECK(o.ticks == scaled / den);
  CHECK(o.dropped_ticks == 0);
  CHECK(o.dropped_ns == rt::nanos{0});
  CHECK(o.remainder.num < o.remainder.den);
  CHECK(o.remainder.num * den == (scaled % den) * o.remainder.den);
  CHECK(o.frames > 0);
}

} // namespace

TEST_CASE("RT-INV-CLOCK-01: 60 Hz over 3600 s of jittered deltas yields exactly 216000 ticks") {
  const outcome o = run(time::tick_rate{}, 0x5EED0060ULL);
  CHECK(o.ticks == 216000);
  CHECK(o.remainder.num == 0);
  CHECK(o.dropped_ticks == 0);
  const outcome again = run(time::tick_rate{}, 0x5EED0061ULL);
  CHECK(again.ticks == 216000);
  CHECK(again.frames != o.frames); // a different jitter, the same count
}

TEST_CASE("RT-INV-CLOCK-01: 30 and 50 Hz keep exact counts") {
  check_rate(30, 1, 30);
  check_rate(50, 1, 50);
}

TEST_CASE("RT-INV-CLOCK-01: 120 and 144 Hz keep exact counts") {
  check_rate(120, 1, 120);
  check_rate(144, 1, 144);
}

TEST_CASE("RT-INV-CLOCK-01: 60000/1001 Hz keeps the exact count 215784") {
  check_rate(60000, 1001, 59940);
  const outcome o = run(time::tick_rate{.num = 60000, .den = 1001}, 7);
  CHECK(o.ticks == 215784);
}

TEST_CASE("RT-INV-CLOCK-01: a reducible rate is reduced and counts the same") {
  auto a = time::fixed_stepper::make(time::tick_rate{.num = 120, .den = 2});
  REQUIRE(a.has_value());
  CHECK(a->rate().num == 60);
  CHECK(a->rate().den == 1);
  std::uint64_t ticks = 0;
  for (int i = 0; i < 1000; ++i) {
    ticks += a->advance(std::chrono::microseconds(16'667));
  }
  // 1000 x 16.667 ms = 16.667 s -> 1000.02 ticks at 60 Hz.
  CHECK(ticks == 1000);
}
