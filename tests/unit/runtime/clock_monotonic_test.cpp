// RT-INV-CLOCK-03 (SL-ARCH-003 ARCH3-R079, failpoint RFP6 clock.jump): a sample at or before the
// previous one gives delta 0 and non_monotonic + 1, and over a seeded timeline with backward jumps
// the logical tick and the frame index never decrease. RFP6 param 0 forces the backward outcome;
// param p adds a p ms hitch that is clamped to max_frame_delta with the excess in dropped_ns.

#include "sl/runtime/faults.hpp"
#include "sl/runtime/time.hpp"

#include "../../../engine/runtime/testing/faults_arm.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>

namespace {

namespace rt = sl::runtime;
namespace rtt = sl::runtime::testing;
namespace time = sl::runtime::time;

using std::chrono::milliseconds;

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

} // namespace

TEST_CASE("RT-INV-CLOCK-03: an equal or backward sample gives 0 and counts non_monotonic") {
  time::frame_clock clock;
  CHECK(clock.sample(milliseconds(1000)) == rt::nanos{0}); // the first sample
  CHECK(clock.non_monotonic() == 0);
  CHECK(clock.sample(milliseconds(1016)) == milliseconds(16));
  CHECK(clock.sample(milliseconds(1016)) == rt::nanos{0}); // equal
  CHECK(clock.non_monotonic() == 1);
  CHECK(clock.sample(milliseconds(900)) == rt::nanos{0}); // backward
  CHECK(clock.non_monotonic() == 2);
  // The baseline stays at the highest sample: time never runs backward.
  CHECK(clock.sample(milliseconds(1010)) == rt::nanos{0});
  CHECK(clock.non_monotonic() == 3);
  CHECK(clock.sample(milliseconds(1030)) == milliseconds(14));
  CHECK(clock.non_monotonic() == 3);
  CHECK(clock.frame_index() == 6);
  CHECK(clock.dropped_ns() == rt::nanos{0});
}

TEST_CASE("RT-INV-CLOCK-03: tick and frame index never decrease over a jumping timeline") {
  time::frame_clock clock;
  auto stepper = time::fixed_stepper::make(time::tick_rate{});
  REQUIRE(stepper.has_value());
  time::logical_clock logical;
  splitmix64 rng{0xC10C03ULL};
  std::int64_t now = 50'000'000'000;
  std::uint64_t backward = 0;
  std::uint64_t last_tick = 0;
  std::uint64_t last_frame = 0;
  std::uint64_t violations = 0;
  std::int64_t high = now;
  (void)clock.sample(rt::nanos{now});
  for (int i = 0; i < 20000; ++i) {
    const std::uint64_t r = rng.next();
    if (r % 10 == 0) {
      now -= static_cast<std::int64_t>(r % 2'000'000'000U); // a backward jump of up to 2 s
    } else {
      now += static_cast<std::int64_t>(r % 40'000'000U);
    }
    const bool non_monotonic = now <= high;
    if (non_monotonic) {
      ++backward;
    } else {
      high = now;
    }
    const time::frame_time ft{.frame_index = clock.frame_index() + 1,
                              .now = logical.now(),
                              .delta = clock.sample(rt::nanos{now}),
                              .a = stepper->remainder()};
    // Violations are counted and asserted once (2e4 frames would be 1e5 doctest assertions).
    violations += ft.frame_index == clock.frame_index() ? 0U : 1U;
    violations += non_monotonic && ft.delta != rt::nanos{0} ? 1U : 0U;
    violations += ft.delta < rt::nanos{0} ? 1U : 0U;
    const std::uint32_t n = stepper->advance(ft.delta);
    for (std::uint32_t k = 0; k < n; ++k) {
      logical.advance();
    }
    violations += logical.now() < last_tick ? 1U : 0U;
    violations += clock.frame_index() <= last_frame ? 1U : 0U;
    last_tick = logical.now();
    last_frame = clock.frame_index();
  }
  CHECK(violations == 0);
  CHECK(last_tick > 0);
  CHECK(clock.non_monotonic() == backward);
  CHECK(backward > 0);
}

TEST_CASE("RT-INV-CLOCK-03: RFP6 param 0 is a backward sample, param p a clamped hitch") {
  if (!rt::faults_compiled) {
    MESSAGE("skipped: the failpoints are compiled out (Release)");
    return;
  }
  rtt::disarm_all();
  time::frame_clock clock;
  CHECK(clock.sample(milliseconds(0)) == rt::nanos{0});
  CHECK(clock.sample(milliseconds(10)) == milliseconds(10));
  {
    const rtt::scoped_arm arm(rt::failpoint::rfp6, 1);
    REQUIRE(arm.status().has_value());
    CHECK(clock.sample(milliseconds(20)) == rt::nanos{0});
    CHECK(clock.non_monotonic() == 1);
  }
  // The forced sample did not move the baseline.
  CHECK(clock.sample(milliseconds(30)) == milliseconds(20));
  {
    const rtt::scoped_arm arm(rt::failpoint::rfp6, 1, false, 100);
    REQUIRE(arm.status().has_value());
    CHECK(clock.sample(milliseconds(46)) == milliseconds(116)); // 16 ms + a 100 ms hitch
  }
  CHECK(clock.dropped_ns() == rt::nanos{0});
  {
    const rtt::scoped_arm arm(rt::failpoint::rfp6, 1, false, 1000);
    REQUIRE(arm.status().has_value());
    CHECK(clock.sample(milliseconds(62)) == milliseconds(250)); // clamped
  }
  CHECK(clock.dropped_ns() == milliseconds(1016) - milliseconds(250));
  CHECK(clock.non_monotonic() == 1);
  // A hitch is per frame: the next delta is the real one.
  CHECK(clock.sample(milliseconds(78)) == milliseconds(16));
  CHECK(clock.frame_index() == 7);
}
