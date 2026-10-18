// FAB-INV-BY-02 (SL-ARCH-001 ARCH1-R30): byte_reader never reads outside its input; a read past
// the end is decode_truncated (R-SER-TRUNC) and leaves the position unchanged.

#include "sl/fabric/bytes.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

using sl::fabric::byte_reader;
using sl::fabric::byte_span;
using sl::fabric::byte_writer;

std::vector<std::uint8_t> sample() {
  byte_writer w;
  w.u8(0xAB);
  w.u32le(0x01020304U);
  w.u64le(0x1122334455667788ULL);
  w.uvarint(300);
  const std::vector<std::uint8_t> raw = {'h', 'i', '!'};
  w.uvarint(raw.size());
  w.bytes(raw);
  return w.take();
}

// Reads the sample layout; returns the first failure's code, or ok.
sl::errc read_all(byte_reader &r) {
  if (auto v = r.u8(); !v || *v != 0xAB) {
    return v ? sl::errc::internal : v.error().code;
  }
  if (auto v = r.u32le(); !v || *v != 0x01020304U) {
    return v ? sl::errc::internal : v.error().code;
  }
  if (auto v = r.u64le(); !v || *v != 0x1122334455667788ULL) {
    return v ? sl::errc::internal : v.error().code;
  }
  if (auto v = r.uvarint(); !v || *v != 300) {
    return v ? sl::errc::internal : v.error().code;
  }
  const auto n = r.uvarint();
  if (!n) {
    return n.error().code;
  }
  const auto b = r.bytes(static_cast<std::size_t>(*n));
  if (!b) {
    return b.error().code;
  }
  if (auto e = r.expect_end(); !e) {
    return e.error().code;
  }
  return sl::errc::ok;
}

} // namespace

TEST_CASE("FAB-INV-BY-02: writer layout is little-endian and the reader reads it back") {
  const std::vector<std::uint8_t> s = sample();
  REQUIRE(s.size() == 1 + 4 + 8 + 2 + 1 + 3);
  CHECK(s[1] == 0x04);
  CHECK(s[4] == 0x01);
  CHECK(s[5] == 0x88);
  CHECK(s[12] == 0x11);
  byte_reader r{s};
  CHECK(read_all(r) == sl::errc::ok);
  CHECK(r.at_end());
  CHECK(r.remaining() == 0);
}

TEST_CASE("FAB-INV-BY-02: every truncation of the sample is decode_truncated") {
  const std::vector<std::uint8_t> s = sample();
  for (std::size_t n = 0; n < s.size(); ++n) {
    CAPTURE(n);
    // A heap copy of exactly n bytes, so a sanitizer sees any read past the end.
    const std::vector<std::uint8_t> cut(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
    byte_reader r{cut};
    CHECK(read_all(r) == sl::errc::decode_truncated);
    CHECK(r.offset() <= n);
  }
}

TEST_CASE("FAB-INV-BY-02: failed reads leave the position and report the offset") {
  const std::vector<std::uint8_t> s = {0x01, 0x02, 0x03};
  byte_reader r{s};
  REQUIRE(r.u8().has_value());
  const auto big = r.u32le();
  REQUIRE_FALSE(big.has_value());
  CHECK(big.error().code == sl::errc::decode_truncated);
  CHECK(big.error().message.starts_with("R-SER-TRUNC at offset 1: need 4 bytes, 2 left"));
  CHECK(r.offset() == 1);
  CHECK_FALSE(r.u64le().has_value());
  CHECK(r.offset() == 1);
  const auto huge = r.bytes(std::numeric_limits<std::size_t>::max());
  REQUIRE_FALSE(huge.has_value());
  CHECK(huge.error().code == sl::errc::decode_truncated);
  CHECK(r.offset() == 1);
  const auto two = r.bytes(2);
  REQUIRE(two.has_value());
  CHECK(two->size() == 2);
  CHECK(two->data() == s.data() + 1); // a view into the input, not a copy
  CHECK(r.at_end());
  const auto past = r.u8();
  REQUIRE_FALSE(past.has_value());
  CHECK(past.error().code == sl::errc::decode_truncated);
  CHECK(r.bytes(0).has_value()); // an empty read at the end is in bounds
}

TEST_CASE("FAB-INV-BY-02: a varint cut short is decode_truncated, trailing bytes are refused") {
  const std::vector<std::uint8_t> cut = {0x80, 0x80};
  byte_reader r{cut};
  const auto v = r.uvarint();
  REQUIRE_FALSE(v.has_value());
  CHECK(v.error().code == sl::errc::decode_truncated);
  CHECK(v.error().message.starts_with("R-SER-TRUNC at offset 2"));
  CHECK(r.offset() == 0);

  const std::vector<std::uint8_t> extra = {0x01, 0x00};
  byte_reader t{extra};
  REQUIRE(t.uvarint().has_value());
  const auto end = t.expect_end();
  REQUIRE_FALSE(end.has_value());
  CHECK(end.error().code == sl::errc::decode_noncanonical);
  CHECK(end.error().message.starts_with("R-SER-TRAILING at offset 1"));

  byte_reader empty{byte_span{}};
  CHECK(empty.at_end());
  CHECK(empty.expect_end().has_value());
  CHECK(empty.uvarint().error().code == sl::errc::decode_truncated);
}

TEST_CASE("FAB-INV-BY-02: take() empties the writer") {
  byte_writer w;
  w.u8(1);
  w.uvarint(128);
  CHECK(w.size() == 3);
  CHECK(w.view().size() == 3);
  const std::vector<std::uint8_t> out = w.take();
  CHECK(out == std::vector<std::uint8_t>{0x01, 0x80, 0x01});
  CHECK(w.size() == 0);
  w.u8(2);
  CHECK(w.data() == std::vector<std::uint8_t>{0x02});
  w.clear();
  CHECK(w.size() == 0);
}
