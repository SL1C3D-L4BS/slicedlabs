// RT-INV-CLOCK-02 (SL-ARCH-003 ARCH3-R080, R079, `limits` max_frame_delta 250 ms and max_catchup
// 8): a 30 s hitch is clamped to max_frame_delta with dropped_ns = 30 s - 250 ms; the clamped delta
// runs at most max_catchup ticks, the whole ticks beyond are counted in dropped_ticks and the
// remainder stays below one tick. Also: a zero num or den is invalid_tick_rate, and a delta that
// does not fit 64 bits once multiplied by num is still exact.

#include "sl/core/error.hpp"
#include "sl/runtime/time.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <limits>

namespace {

namespace rt = sl::runtime;
namespace time = sl::runtime::time;

using std::chrono::milliseconds;
using std::chrono::seconds;

time::fixed_stepper stepper(std::uint32_t num, std::uint32_t den, std::uint32_t max_catchup = 8) {
  auto s = time::fixed_stepper::make(time::tick_rate{.num = num, .den = den}, max_catchup);
  REQUIRE(s.has_value());
  return *s;
}

} // namespace

TEST_CASE("RT-INV-CLOCK-02: a 30 s delta runs max_catchup ticks and counts the rest") {
  CHECK(time::max_frame_delta_default == milliseconds(250));
  CHECK(time::max_catchup_default == 8);
  time::frame_clock clock;
  time::fixed_stepper s = stepper(60, 1);
  CHECK(clock.sample(seconds(100)) == rt::nanos{0});
  const rt::nanos d = clock.sample(seconds(130));
  CHECK(d == milliseconds(250));
  CHECK(clock.dropped_ns() == seconds(30) - milliseconds(250));
  CHECK(s.advance(d) == 8);
  CHECK(s.dropped_ticks() == 7); // 250 ms at 60 Hz is 15 ticks
  CHECK(s.remainder().num == 0);
  CHECK(s.remainder().num < s.remainder().den);
  // The next normal frame is unaffected.
  CHECK(clock.sample(seconds(130) + milliseconds(20)) == milliseconds(20));
  CHECK(s.advance(milliseconds(20)) == 1);
  CHECK(s.dropped_ticks() == 7);
  CHECK(clock.dropped_ns() == seconds(30) - milliseconds(250));
}

TEST_CASE("RT-INV-CLOCK-02: other rates and caps keep the remainder below one tick") {
  time::fixed_stepper s144 = stepper(144, 1);
  CHECK(s144.advance(milliseconds(250)) == 8);
  CHECK(s144.dropped_ticks() == 28); // 36 ticks
  CHECK(s144.remainder().num == 0);

  time::fixed_stepper ntsc = stepper(60000, 1001);
  CHECK(ntsc.advance(milliseconds(250)) == 8);
  CHECK(ntsc.dropped_ticks() == 6); // 14.985 ticks
  CHECK(ntsc.remainder().num == 986'000'000'000ULL);
  CHECK(ntsc.remainder().den == 1'001'000'000'000ULL);

  time::fixed_stepper capped = stepper(60, 1, 4);
  CHECK(capped.max_catchup() == 4);
  CHECK(capped.advance(milliseconds(250)) == 4);
  CHECK(capped.dropped_ticks() == 11);

  time::fixed_stepper none = stepper(60, 1, 0);
  CHECK(none.advance(milliseconds(250)) == 0);
  CHECK(none.dropped_ticks() == 15);
}

TEST_CASE("RT-INV-CLOCK-02: an extreme delta is exact and never overflows") {
  time::fixed_stepper s = stepper(60, 1);
  const rt::nanos huge{std::numeric_limits<rt::nanos::rep>::max()};
  CHECK(s.advance(huge) == 8);
  // (2^63 - 1) x 60 = 553402322211 x 1e9 + 286548420.
  CHECK(s.dropped_ticks() == 553'402'322'211ULL - 8);
  CHECK(s.remainder().num == 286'548'420ULL);
  CHECK(s.remainder().den == 1'000'000'000ULL);

  time::fixed_stepper wide = stepper(std::numeric_limits<std::uint32_t>::max(), 1);
  CHECK(wide.advance(huge) == 8);
  CHECK(wide.remainder().num < wide.remainder().den);

  CHECK(s.advance(rt::nanos{0}) == 0);
  CHECK(s.advance(rt::nanos{-5}) == 0);
}

TEST_CASE("RT-INV-CLOCK-02: num or den 0 is invalid_tick_rate") {
  const auto zero_num = time::fixed_stepper::make(time::tick_rate{.num = 0, .den = 1});
  REQUIRE_FALSE(zero_num.has_value());
  CHECK(zero_num.error().code == sl::errc::invalid_tick_rate);
  const auto zero_den = time::fixed_stepper::make(time::tick_rate{.num = 60, .den = 0});
  REQUIRE_FALSE(zero_den.has_value());
  CHECK(zero_den.error().code == sl::errc::invalid_tick_rate);
}

TEST_CASE("RT-INV-CLOCK-02: capture and restore keep the accumulator and the dropped count") {
  time::fixed_stepper a = stepper(60000, 1001);
  CHECK(a.advance(milliseconds(250)) == 8);
  CHECK(a.advance(milliseconds(7)) == 1);
  const time::fixed_stepper::state st = a.capture();
  time::fixed_stepper b = stepper(60000, 1001);
  b.restore(st);
  for (int i = 0; i < 100; ++i) {
    const rt::nanos d = milliseconds(3 + ((i * 7) % 40));
    CHECK(a.advance(d) == b.advance(d));
  }
  CHECK(a.remainder().num == b.remainder().num);
  CHECK(a.dropped_ticks() == b.dropped_ticks());
}
