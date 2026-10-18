// FAB-INV-JRN-01 (SL-ARCH-002 ARCH2-R29): the frame codec. CRC-32C against the standard vectors
// (RFC 3720 appendix B.4 and the "123456789" check value), the frame layout [len][crc][type]
// [payload], length bounds, the stop offset and reason of the reader, the segment header and the
// segment file names.

#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/journal.hpp"
#include "sl/fabric/vfs.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using sl::errc;
using sl::fabric::crc32c;
using sl::fabric::crc32c_extend;
using sl::fabric::frame_fault;
using sl::fabric::frame_reader;
using sl::fabric::frame_type;
using sl::fabric::frame_view;

std::vector<std::uint8_t> bytes_of(std::string_view s) { return {s.begin(), s.end()}; }

std::vector<std::uint8_t> frame(frame_type t, std::string_view payload) {
  const auto f = sl::fabric::encode_frame(t, bytes_of(payload));
  REQUIRE(f.has_value());
  return *f;
}

void append(std::vector<std::uint8_t> &out, const std::vector<std::uint8_t> &more) {
  out.insert(out.end(), more.begin(), more.end());
}

void put_u32le(std::vector<std::uint8_t> &out, std::size_t at, std::uint32_t v) {
  for (std::size_t i = 0; i < 4; ++i) {
    out[at + i] = static_cast<std::uint8_t>(v >> (8U * i));
  }
}

std::string payload_text(const frame_view &f) { return {f.payload.begin(), f.payload.end()}; }

} // namespace

TEST_CASE("FAB-INV-JRN-01: CRC-32C matches the standard check vectors") {
  CHECK(crc32c(bytes_of("123456789")) == 0xE3069283U);
  CHECK(crc32c({}) == 0U);
  std::array<std::uint8_t, 32> zeros{};
  CHECK(crc32c(zeros) == 0x8A9136AAU);
  std::array<std::uint8_t, 32> ones{};
  ones.fill(0xFF);
  CHECK(crc32c(ones) == 0x62A8AB43U);
  std::array<std::uint8_t, 32> up{};
  std::array<std::uint8_t, 32> down{};
  for (std::size_t i = 0; i < 32; ++i) {
    up.at(i) = static_cast<std::uint8_t>(i);
    down.at(i) = static_cast<std::uint8_t>(31 - i);
  }
  CHECK(crc32c(up) == 0x46DD794EU);
  CHECK(crc32c(down) == 0x113FDB5CU);
  // RFC 3720 B.4: an iSCSI SCSI Read (10) command PDU.
  const std::array<std::uint8_t, 48> pdu = {
      0x01, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
      0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x18, 0x28, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  CHECK(crc32c(pdu) == 0xD9963A56U);
}

TEST_CASE("FAB-INV-JRN-01: CRC-32C extends across any split") {
  const std::vector<std::uint8_t> all = bytes_of("123456789");
  const sl::fabric::byte_span span(all);
  for (std::size_t cut = 0; cut <= all.size(); ++cut) {
    CHECK(crc32c_extend(crc32c(span.first(cut)), span.subspan(cut)) == 0xE3069283U);
  }
}

TEST_CASE("FAB-INV-JRN-01: a frame is len u32le, crc32c over type and payload, type, payload") {
  const std::vector<std::uint8_t> f = frame(frame_type::commit, "abc");
  REQUIRE(f.size() == 12);
  CHECK(f[0] == 4); // len = 1 + payload
  CHECK(f[1] == 0);
  CHECK(f[2] == 0);
  CHECK(f[3] == 0);
  const std::array<std::uint8_t, 4> covered = {2, 'a', 'b', 'c'};
  const std::uint32_t crc = crc32c(covered);
  CHECK(f[4] == static_cast<std::uint8_t>(crc));
  CHECK(f[5] == static_cast<std::uint8_t>(crc >> 8U));
  CHECK(f[6] == static_cast<std::uint8_t>(crc >> 16U));
  CHECK(f[7] == static_cast<std::uint8_t>(crc >> 24U));
  CHECK(f[8] == 2);
  CHECK(f[9] == 'a');

  std::vector<std::uint8_t> log = f;
  append(log, frame(frame_type::object, ""));
  frame_reader r(log, 24);
  const std::optional<frame_view> first = r.next();
  REQUIRE(first.has_value());
  CHECK(first.value_or(frame_view{}).offset == 24);
  CHECK(first.value_or(frame_view{}).type == frame_type::commit);
  CHECK(payload_text(first.value_or(frame_view{})) == "abc");
  const std::optional<frame_view> second = r.next();
  REQUIRE(second.has_value());
  CHECK(second.value_or(frame_view{}).offset == 36);
  CHECK(second.value_or(frame_view{}).type == frame_type::object);
  CHECK(second.value_or(frame_view{}).payload.empty());
  CHECK_FALSE(r.next().has_value());
  CHECK(r.stop().clean());
  CHECK(r.stop().end == 24 + log.size());
}

TEST_CASE("FAB-INV-JRN-01: the frame cap is 64 MiB including the type byte") {
  CHECK(sl::fabric::max_frame_len == 64U * 1024U * 1024U);
  const std::vector<std::uint8_t> too_big(sl::fabric::max_frame_len);
  const auto refused = sl::fabric::encode_frame(frame_type::object, too_big);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().code == errc::encode_invalid);
}

TEST_CASE("FAB-INV-JRN-01: length bounds stop the reader with the offset and the reason") {
  std::vector<std::uint8_t> good = frame(frame_type::object, "one");
  append(good, frame(frame_type::commit, "two"));
  const std::uint64_t bad_at = good.size();

  const auto stop_of = [](const std::vector<std::uint8_t> &bytes) {
    frame_reader r(bytes);
    int frames = 0;
    while (r.next()) {
      ++frames;
    }
    CHECK(frames == 2);
    return r.stop();
  };

  SUBCASE("len 0") {
    std::vector<std::uint8_t> log = good;
    std::vector<std::uint8_t> zero = frame(frame_type::note, "x");
    put_u32le(zero, 0, 0);
    append(log, zero);
    const auto stop = stop_of(log);
    CHECK(stop.fault == frame_fault::bad_length);
    CHECK(stop.offset == bad_at);
    CHECK(stop.end == bad_at);
    CHECK(stop.message.find("at offset " + std::to_string(bad_at)) != std::string::npos);
  }
  SUBCASE("len above 64 MiB") {
    std::vector<std::uint8_t> log = good;
    std::vector<std::uint8_t> huge = frame(frame_type::note, "x");
    put_u32le(huge, 0, sl::fabric::max_frame_len + 1);
    append(log, huge);
    CHECK(stop_of(log).fault == frame_fault::bad_length);
  }
  SUBCASE("len at the cap but the input ends first") {
    std::vector<std::uint8_t> log = good;
    std::vector<std::uint8_t> big = frame(frame_type::note, "x");
    put_u32le(big, 0, sl::fabric::max_frame_len);
    append(log, big);
    const auto stop = stop_of(log);
    CHECK(stop.fault == frame_fault::truncated_payload);
    CHECK(stop.offset == bad_at);
  }
  SUBCASE("fewer than 9 bytes left") {
    for (std::size_t n = 1; n < sl::fabric::frame_prefix_size; ++n) {
      std::vector<std::uint8_t> log = good;
      const std::vector<std::uint8_t> next = frame(frame_type::note, "x");
      log.insert(log.end(), next.begin(), next.begin() + static_cast<std::ptrdiff_t>(n));
      const auto stop = stop_of(log);
      CHECK(stop.fault == frame_fault::truncated_prefix);
      CHECK(stop.offset == bad_at);
    }
  }
  SUBCASE("a payload cut short") {
    const std::vector<std::uint8_t> next = frame(frame_type::note, "payload");
    for (std::size_t n = sl::fabric::frame_prefix_size; n < next.size(); ++n) {
      std::vector<std::uint8_t> log = good;
      log.insert(log.end(), next.begin(), next.begin() + static_cast<std::ptrdiff_t>(n));
      CHECK(stop_of(log).fault == frame_fault::truncated_payload);
    }
  }
}

TEST_CASE("FAB-INV-JRN-01: every single-bit flip of a frame stops the reader at that frame") {
  const std::vector<std::uint8_t> clean = frame(frame_type::commit, "commit payload bytes");
  for (std::size_t i = 0; i < clean.size(); ++i) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      std::vector<std::uint8_t> bad = clean;
      bad[i] ^= static_cast<std::uint8_t>(1U << bit);
      frame_reader r(bad);
      CHECK_FALSE(r.next().has_value());
      CHECK(r.stop().fault != frame_fault::none);
      CHECK(r.stop().offset == 0);
      CHECK(r.stop().end == 0);
      if (i >= 4) { // the length is intact: the CRC catches it
        CHECK(r.stop().fault == frame_fault::crc_mismatch);
      }
    }
  }
}

TEST_CASE("FAB-INV-JRN-01: the segment header is 24 bytes with a CRC over bytes 0-19") {
  const sl::fabric::segment_header h{.major = 1, .minor = 0, .segment = 7, .first_seq = 42};
  const auto bytes = sl::fabric::encode_segment_header(h);
  const std::array<std::uint8_t, 20> expect_front = {'S', 'L', 'G', 'F', 1, 0, 0, 0, 7, 0,
                                                     0,   0,   42,  0,   0, 0, 0, 0, 0, 0};
  for (std::size_t i = 0; i < expect_front.size(); ++i) {
    CHECK(bytes.at(i) == expect_front.at(i));
  }
  const std::uint32_t crc = crc32c(sl::fabric::byte_span(bytes).first(20));
  CHECK(bytes[20] == static_cast<std::uint8_t>(crc));
  CHECK(bytes[23] == static_cast<std::uint8_t>(crc >> 24U));
  const auto parsed = sl::fabric::parse_segment_header(bytes);
  REQUIRE(parsed.has_value());
  CHECK(*parsed == h);

  for (std::size_t i = 0; i < bytes.size(); ++i) {
    auto bad = bytes;
    bad.at(i) ^= 0x01U;
    const auto r = sl::fabric::parse_segment_header(bad);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == (i == 4 ? errc::format_unsupported : errc::journal_corrupt));
  }
  CHECK(sl::fabric::parse_segment_header(sl::fabric::byte_span(bytes).first(23)).error().code ==
        errc::decode_truncated);
  const auto newer = sl::fabric::encode_segment_header(
      sl::fabric::segment_header{.major = 1, .minor = 3, .segment = 1, .first_seq = 1});
  CHECK(sl::fabric::parse_segment_header(newer).value_or(sl::fabric::segment_header{}).minor == 3);
}

TEST_CASE("FAB-INV-JRN-01: segment file names are seg-NNNNNNNN.slg") {
  CHECK(sl::fabric::segment_file_name(1) == "seg-00000001.slg");
  CHECK(sl::fabric::segment_file_name(12345678) == "seg-12345678.slg");
  CHECK(sl::fabric::parse_segment_file_name("seg-00000001.slg") == std::optional<std::uint32_t>(1));
  CHECK(sl::fabric::parse_segment_file_name("seg-00000420.slg") ==
        std::optional<std::uint32_t>(420));
  for (const std::string_view bad : {"seg-00000000.slg", "seg-0000001.slg", "seg-0000000a.slg",
                                     "seg-00000001.slx", "seg-000000001.slg", "FORMAT"}) {
    CHECK_FALSE(sl::fabric::parse_segment_file_name(bad).has_value());
  }
}

TEST_CASE("FAB-INV-JRN-01: scan_segment over a vfs file reports what frame_reader reports") {
  std::vector<std::uint8_t> log(24, 0);
  append(log, frame(frame_type::object, "a"));
  append(log, frame(frame_type::intent_begin, "bb"));
  append(log, frame(static_cast<frame_type>(200), "skipped"));
  append(log, frame(frame_type::commit, "ccc"));
  const std::vector<std::uint8_t> torn = frame(frame_type::object, "torn");
  log.insert(log.end(), torn.begin(), torn.begin() + 11);

  sl::fabric::vfs_memory fs;
  {
    auto f = fs.open("seg", sl::fabric::open_create);
    REQUIRE(f.has_value());
    REQUIRE(f->append(log).has_value());
  }
  auto f = fs.open("seg", sl::fabric::open_read);
  REQUIRE(f.has_value());
  std::vector<std::string> scanned;
  const auto stop =
      sl::fabric::scan_segment(*f, 24, [&scanned](const frame_view &v) -> sl::result<void> {
        scanned.push_back(std::to_string(v.offset) + ":" +
                          std::to_string(static_cast<int>(v.type)) + ":" + payload_text(v));
        return {};
      });
  REQUIRE(stop.has_value());

  frame_reader r(sl::fabric::byte_span(log).subspan(24), 24);
  std::vector<std::string> read;
  while (const std::optional<frame_view> v = r.next()) {
    read.push_back(std::to_string(v->offset) + ":" + std::to_string(static_cast<int>(v->type)) +
                   ":" + payload_text(*v));
  }
  CHECK(scanned == read);
  CHECK(scanned == std::vector<std::string>{"24:1:a", "34:3:bb", "61:2:ccc"});
  CHECK(stop->fault == frame_fault::truncated_payload);
  CHECK(r.stop().fault == frame_fault::truncated_payload);
  CHECK(stop->offset == r.stop().offset);
  CHECK(stop->end == r.stop().end);
  CHECK(stop->skipped == 1);
  CHECK(r.stop().skipped == 1);
}
