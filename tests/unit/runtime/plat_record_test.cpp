// RT-INV-PLAT-05 (SL-ARCH-003 ARCH3-R039): every external input passes record_sink exactly once,
// in arrival order, stamped with the consuming frame index and tick. The frame scheduler (WO-089)
// does not exist yet, so the test drives the frames by hand: events arrive in bursts larger than
// the poll buffer, so some are consumed one frame later than they arrived, and each record must
// carry the frame and tick of the poll that consumed it.

#include "sl/fabric/digest.hpp"
#include "sl/runtime/platform.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace rtt = sl::runtime::testing;

constexpr std::uint64_t frame_count = 40;
constexpr std::uint64_t tick_base = 1000;

rt::input_event numbered(std::uint32_t id) {
  rt::input_event e{};
  e.type = 7;
  e.size = 4;
  for (std::size_t i = 0; i < 4; ++i) {
    e.payload.at(i) = static_cast<std::byte>((id >> (8 * i)) & 0xFFU);
  }
  return e;
}

std::uint32_t id_of(const rtt::record &r) {
  std::uint32_t id = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    id |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(r.data[i])) << (8 * i);
  }
  return id;
}

} // namespace

TEST_CASE(
    "RT-INV-PLAT-05: every input passes record_sink once, in order, with its consuming frame") {
  rtt::scripted_platform sp;
  rt::platform &p = sp.get();
  std::array<rt::input_event, 3> buffer{};
  std::deque<std::uint32_t> model; // the arrival queue the test expects
  std::vector<std::uint64_t> expected_frame;
  std::uint32_t next_id = 0;
  for (std::uint64_t frame = 0; frame < frame_count; ++frame) {
    const std::uint64_t burst = (frame * 7) % 6; // 0..5 events, more than the buffer at times
    for (std::uint64_t k = 0; k < burst; ++k) {
      sp.input().push(numbered(next_id));
      model.push_back(next_id++);
    }
    const std::size_t consumed =
        rt::record_input(p.input, p.sink, frame, tick_base + frame, buffer);
    CHECK(consumed == std::min<std::size_t>(buffer.size(), model.size()));
    for (std::size_t i = 0; i < consumed; ++i) {
      model.pop_front();
      expected_frame.push_back(frame);
    }
  }
  // drain the tail in one more frame
  std::uint64_t frame = frame_count;
  while (sp.input().pending() > 0) {
    const std::size_t consumed =
        rt::record_input(p.input, p.sink, frame, tick_base + frame, buffer);
    for (std::size_t i = 0; i < consumed; ++i) {
      model.pop_front();
      expected_frame.push_back(frame);
    }
    ++frame;
  }
  const std::vector<rtt::record> &log = sp.sink().records();
  REQUIRE(log.size() == next_id);
  REQUIRE(expected_frame.size() == next_id);
  for (std::uint32_t i = 0; i < next_id; ++i) {
    CHECK(log[i].kind == rtt::record_kind::input);
    CHECK(id_of(log[i]) == i); // exactly once, in arrival order
    CHECK(log[i].frame == expected_frame[i]);
    CHECK(log[i].tick == tick_base + expected_frame[i]);
  }
}

TEST_CASE("RT-INV-PLAT-05: wall samples, file reads and background results are recorded once") {
  rtt::scripted_platform sp;
  rt::platform &p = sp.get();
  const std::array<std::byte, 4> content{std::byte{9}, std::byte{8}, std::byte{7}, std::byte{6}};
  REQUIRE(p.files.atomic_write("in.bin", content).has_value());
  sp.clock().set(rt::nanos{100});

  const rt::nanos sample = rt::record_wall_sample(p.clock, p.sink, 4);
  auto read = rt::record_file_read(p.files, p.sink, 4, 12, "in.bin");
  REQUIRE(read.has_value());
  auto missing = rt::record_file_read(p.files, p.sink, 4, 12, "absent.bin");
  REQUIRE_FALSE(missing.has_value());
  const std::array<std::byte, 2> result{std::byte{0xAB}, std::byte{0xCD}};
  p.sink.on_external(5, 13, 77, result);

  const std::vector<rtt::record> &log = sp.sink().records();
  REQUIRE(log.size() == 3); // the failed read passed nothing
  CHECK(log[0].kind == rtt::record_kind::wall_sample);
  CHECK(log[0].frame == 4);
  CHECK(log[0].sample == sample.count());
  CHECK(log[1].kind == rtt::record_kind::file_read);
  CHECK(log[1].frame == 4);
  CHECK(log[1].tick == 12);
  CHECK(log[1].path == "in.bin");
  const sl::fabric::digest d = rt::content_digest(content);
  REQUIRE(log[1].data.size() == d.bytes.size() + 1);
  for (std::size_t i = 0; i < d.bytes.size(); ++i) {
    CHECK(std::to_integer<std::uint8_t>(log[1].data[i + 1]) == d.bytes.at(i));
  }
  CHECK(log[2].kind == rtt::record_kind::external);
  CHECK(log[2].frame == 5);
  CHECK(log[2].tick == 13);
  CHECK(log[2].code == 77);
  CHECK(log[2].data == std::vector<std::byte>(result.begin(), result.end()));

  // the default sink accepts every call and records nothing
  rt::null_record_sink null_sink;
  static_cast<void>(rt::record_wall_sample(p.clock, null_sink, 6));
  CHECK(sp.sink().records().size() == 3);
}
