// RT-INV-CLOCK-04 (SL-ARCH-003 ARCH3-R026, R091): suspend freezes every clock. Across a 10 s
// manual jump of the clock source while frozen no tick accrues, nothing is counted as dropped or
// non-monotonic, and the first delta after resume is 0; the frame loop then continues at its rate.
// The runtime.clock contributor round trips the clocks (frozen or not) and hashes the tick only.

#include "sl/core/error.hpp"
#include "sl/runtime/replay.hpp"
#include "sl/runtime/time.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace rtt = sl::runtime::testing;
namespace time = sl::runtime::time;
namespace det = sl::runtime::det;

using std::chrono::milliseconds;
using std::chrono::seconds;

struct clocks {
  time::fixed_stepper stepper = *time::fixed_stepper::make(time::tick_rate{});
  time::frame_clock frame;
  time::logical_clock logical;

  // One begin_frame: one sample, then the ticks it allows.
  std::uint32_t frame_step(const rt::clock_source &source) {
    const std::uint32_t n = stepper.advance(frame.sample(source.now()));
    for (std::uint32_t i = 0; i < n; ++i) {
      logical.advance();
    }
    return n;
  }
};

} // namespace

TEST_CASE("RT-INV-CLOCK-04: a 10 s jump while suspended accrues no tick; the next delta is 0") {
  rtt::manual_clock source(seconds(5));
  clocks c;
  (void)c.frame_step(source);
  for (int i = 0; i < 60; ++i) {
    source.advance(rt::nanos{1'000'000'000 / 60});
    (void)c.frame_step(source);
  }
  const time::tick before = c.logical.now();
  CHECK(before == 59); // 60 x 16666666 ns = 59.9999976 ticks
  const time::fixed_stepper::state stepper_before = c.stepper.capture();

  c.frame.freeze(); // suspend at end_frame
  CHECK(c.frame.frozen());
  source.advance(seconds(10));
  // A frame pumped while suspended sees no time either.
  CHECK(c.frame.sample(source.now()) == rt::nanos{0});
  c.frame.thaw(); // resume
  CHECK_FALSE(c.frame.frozen());
  CHECK(c.frame.sample(source.now()) == rt::nanos{0}); // the first delta after resume
  CHECK(c.stepper.advance(rt::nanos{0}) == 0);
  CHECK(c.logical.now() == before);
  CHECK(c.stepper.capture().accumulator == stepper_before.accumulator);
  CHECK(c.stepper.dropped_ticks() == 0);
  CHECK(c.frame.dropped_ns() == rt::nanos{0});
  CHECK(c.frame.non_monotonic() == 0);

  source.advance(milliseconds(50));
  CHECK(c.frame_step(source) == 3);
  CHECK(c.logical.now() == before + 3);
}

TEST_CASE("RT-INV-CLOCK-04: the runtime.clock contributor round trips and hashes the tick") {
  rtt::manual_clock source(seconds(1));
  clocks a;
  (void)a.frame_step(source);
  for (int i = 0; i < 37; ++i) {
    source.advance(milliseconds(7 + ((i % 5) * 11)));
    (void)a.frame_step(source);
  }
  source.set(seconds(0)); // one backward sample
  (void)a.frame_step(source);
  source.set(seconds(30)); // one clamped hitch
  (void)a.frame_step(source);
  a.frame.freeze();
  const det::clock_contributor ca(a.stepper, a.logical, a.frame);
  CHECK(ca.name() == "runtime.clock");
  CHECK(ca.version() == 1);
  std::vector<std::byte> bytes{std::byte{0xAA}}; // capture appends
  REQUIRE(ca.capture(bytes).has_value());
  CHECK(bytes.size() == 1 + 58);

  clocks b;
  det::clock_contributor cb(b.stepper, b.logical, b.frame);
  REQUIRE(cb.restore(std::span<const std::byte>(bytes).subspan(1)).has_value());
  CHECK(b.logical.now() == a.logical.now());
  CHECK(b.stepper.capture().accumulator == a.stepper.capture().accumulator);
  CHECK(b.stepper.dropped_ticks() == a.stepper.dropped_ticks());
  CHECK(b.frame.frame_index() == a.frame.frame_index());
  CHECK(b.frame.non_monotonic() == 1);
  CHECK(b.frame.dropped_ns() == a.frame.dropped_ns());
  CHECK(b.frame.frozen());

  det::state_hasher ha;
  ca.hash(ha);
  det::state_hasher hb;
  cb.hash(hb);
  CHECK(ha.finish() == hb.finish());
  det::state_hasher tick_only;
  tick_only.u64(a.logical.now());
  CHECK(ha.finish() == tick_only.finish());

  // Both continue identically after resume.
  a.frame.thaw();
  b.frame.thaw();
  for (int i = 0; i < 50; ++i) {
    source.advance(milliseconds(3 + ((i % 7) * 13)));
    CHECK(a.frame_step(source) == b.frame_step(source));
  }
  CHECK(a.logical.now() == b.logical.now());
}

TEST_CASE("RT-INV-CLOCK-04: corrupt runtime.clock bytes are snapshot_corrupt and change nothing") {
  clocks c;
  c.logical.advance();
  det::clock_contributor cc(c.stepper, c.logical, c.frame);
  std::vector<std::byte> bytes;
  REQUIRE(cc.capture(bytes).has_value());

  std::vector<std::byte> shorter(bytes.begin(), bytes.end() - 1);
  auto r = cc.restore(shorter);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == sl::errc::snapshot_corrupt);

  std::vector<std::byte> bad_flag = bytes;
  bad_flag[32] = std::byte{2}; // has_previous
  bad_flag[0] = std::byte{9};  // a different tick that must not land
  r = cc.restore(bad_flag);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == sl::errc::snapshot_corrupt);
  CHECK(c.logical.now() == 1);

  std::vector<std::byte> bad_acc = bytes;
  bad_acc[15] = std::byte{0xFF}; // accumulator far above one tick
  r = cc.restore(bad_acc);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == sl::errc::snapshot_corrupt);
  CHECK(c.stepper.capture().accumulator == 0);
}
