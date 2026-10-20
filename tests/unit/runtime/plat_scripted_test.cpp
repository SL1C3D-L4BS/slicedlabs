// RT-INV-PLAT-02 (SL-ARCH-003 ARCH3-R036): a run driven by scripted_platform's manual clock and
// scripted input is bit-identical across two executions. One run is 100 frames: the manual clock
// steps (once backward), scripted events arrive in bursts, a state file is written and read back
// every 10 frames; the recording sink's serialized log of two fresh runs must be equal byte for
// byte, and so must the SHA-256 of it.

#include "sl/fabric/digest.hpp"
#include "sl/runtime/platform.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace rtt = sl::runtime::testing;

constexpr std::uint64_t frame_count = 100;

rt::input_event make_event(std::uint32_t type, std::uint64_t seq) {
  rt::input_event e{};
  e.type = type;
  e.size = 9;
  for (std::size_t i = 0; i < 8; ++i) {
    e.payload.at(i) = static_cast<std::byte>((seq >> (8 * i)) & 0xFFU);
  }
  e.payload.at(8) = static_cast<std::byte>(type & 0xFFU);
  return e;
}

std::vector<std::byte> run_once() {
  rtt::scripted_platform sp(9);
  rt::platform &p = sp.get();
  sp.clock().set(rt::nanos{1'000'000});
  std::array<rt::input_event, 4> buffer{};
  std::uint64_t seq = 0;
  for (std::uint64_t frame = 0; frame < frame_count; ++frame) {
    sp.clock().advance(frame == 37 ? rt::nanos{-2'000'000} : rt::nanos{16'666'667});
    for (std::uint64_t k = 0; k < frame % 6; ++k) {
      sp.input().push(make_event(static_cast<std::uint32_t>(1 + (k % 3)), seq++));
    }
    const std::uint64_t tick = 2 * frame;
    static_cast<void>(rt::record_wall_sample(p.clock, p.sink, frame));
    static_cast<void>(rt::record_input(p.input, p.sink, frame, tick, buffer));
    if (frame % 10 == 0) {
      const std::array<std::byte, 2> state{static_cast<std::byte>(frame),
                                           static_cast<std::byte>(seq)};
      REQUIRE(p.files.atomic_write("state.bin", state).has_value());
      REQUIRE(rt::record_file_read(p.files, p.sink, frame, tick, "state.bin").has_value());
    }
  }
  return sp.sink().serialize();
}

sl::fabric::sha256::value_type digest_of(const std::vector<std::byte> &bytes) {
  return rt::content_digest(bytes).bytes;
}

} // namespace

TEST_CASE("RT-INV-PLAT-02: two scripted runs produce bit-identical recordings") {
  const std::vector<std::byte> first = run_once();
  const std::vector<std::byte> second = run_once();
  CHECK(first.size() > 1000);
  CHECK(first == second);
  CHECK(digest_of(first) == digest_of(second));
}

TEST_CASE("RT-INV-PLAT-02: manual_clock moves only when told, backward allowed") {
  rtt::manual_clock clock;
  CHECK(clock.now() == rt::nanos{0});
  clock.set(rt::nanos{500});
  CHECK(clock.now() == rt::nanos{500});
  clock.advance(rt::nanos{-700});
  CHECK(clock.now() == rt::nanos{-200});
  clock.advance(rt::nanos{1200});
  CHECK(clock.now() == rt::nanos{1000});

  rtt::manual_wall_clock wall;
  wall.set(rt::nanos{42});
  CHECK(wall.now().time_since_epoch() == rt::nanos{42});
}

TEST_CASE("RT-INV-PLAT-02: scripted_input returns events in push order across polls") {
  rtt::scripted_input input;
  for (std::uint64_t i = 0; i < 7; ++i) {
    input.push(make_event(5, i));
  }
  std::array<rt::input_event, 3> buffer{};
  std::vector<rt::input_event> seen;
  for (;;) {
    const std::size_t n = input.poll(buffer);
    if (n == 0) {
      break;
    }
    seen.insert(seen.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(n));
  }
  REQUIRE(seen.size() == 7);
  for (std::uint64_t i = 0; i < 7; ++i) {
    CHECK(seen[i] == make_event(5, i));
  }
  CHECK(input.pending() == 0);
}
