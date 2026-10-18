// FAB-INV-BY-01 (SL-ARCH-001 ARCH1-R26, ADR-0012): LEB128 is written in shortest form only, and
// the reader rejects overlong and over-64-bit varints (R-SER-VARINT, decode_noncanonical).

#include "sl/fabric/bytes.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

using sl::fabric::byte_reader;
using sl::fabric::byte_writer;

std::vector<std::uint8_t> encode(std::uint64_t v) {
  byte_writer w;
  w.uvarint(v);
  return w.take();
}

} // namespace

TEST_CASE("FAB-INV-BY-01: uvarint encodings at the boundaries are the shortest form") {
  using bytes = std::vector<std::uint8_t>;
  constexpr std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
  CHECK(encode(0) == bytes{0x00});
  CHECK(encode(1) == bytes{0x01});
  CHECK(encode(127) == bytes{0x7F});
  CHECK(encode(128) == bytes{0x80, 0x01});
  CHECK(encode(300) == bytes{0xAC, 0x02});
  CHECK(encode(16383) == bytes{0xFF, 0x7F});
  CHECK(encode(16384) == bytes{0x80, 0x80, 0x01});
  CHECK(encode(std::uint64_t{1} << 63U) ==
        bytes{0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01});
  CHECK(encode(max) == bytes{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01});
  CHECK(sl::fabric::uvarint_size(0) == 1);
  CHECK(sl::fabric::uvarint_size(max) == sl::fabric::max_uvarint_size);
}

TEST_CASE("FAB-INV-BY-01: uvarint round trips and its size is uvarint_size") {
  std::vector<std::uint64_t> values;
  for (unsigned shift = 0; shift < 64; ++shift) {
    const std::uint64_t p = std::uint64_t{1} << shift;
    values.push_back(p - 1);
    values.push_back(p);
    values.push_back(p + 1);
  }
  values.push_back(std::numeric_limits<std::uint64_t>::max());
  for (const std::uint64_t v : values) {
    CAPTURE(v);
    const std::vector<std::uint8_t> enc = encode(v);
    CHECK(enc.size() == sl::fabric::uvarint_size(v));
    CHECK((enc.back() != 0x00 || enc.size() == 1)); // shortest: no zero final group
    byte_reader r{enc};
    const auto got = r.uvarint();
    REQUIRE(got.has_value());
    CHECK(*got == v);
    CHECK(r.at_end());
  }
}

TEST_CASE("FAB-INV-BY-01: overlong uvarints are rejected") {
  using bytes = std::vector<std::uint8_t>;
  for (const bytes &enc : {bytes{0x80, 0x00}, bytes{0xFF, 0x00}, bytes{0x81, 0x80, 0x00},
                           bytes{0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00}}) {
    byte_reader r{enc};
    const auto got = r.uvarint();
    REQUIRE_FALSE(got.has_value());
    CHECK(got.error().code == sl::errc::decode_noncanonical);
    CHECK(got.error().message.starts_with("R-SER-VARINT at offset 0"));
    CHECK(r.offset() == 0); // a failed read does not move
  }
}

TEST_CASE("FAB-INV-BY-01: uvarints wider than 64 bits are rejected") {
  using bytes = std::vector<std::uint8_t>;
  // 2^64 (tenth byte 0x02), the tenth byte with a continuation bit, and an eleven-byte value.
  for (const bytes &enc :
       {bytes{0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x02},
        bytes{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F},
        bytes{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x81, 0x00},
        bytes{0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01}}) {
    byte_reader r{enc};
    const auto got = r.uvarint();
    REQUIRE_FALSE(got.has_value());
    CHECK(got.error().code == sl::errc::decode_noncanonical);
    CHECK(got.error().message.find("exceeds 64 bits") != std::string::npos);
    CHECK(r.offset() == 0);
  }
}

TEST_CASE("FAB-INV-BY-01: the offset in a varint error is the varint's start") {
  const std::vector<std::uint8_t> enc = {0x05, 0x80, 0x00};
  byte_reader r{enc};
  CHECK(r.uvarint().value() == 5);
  const auto bad = r.uvarint();
  REQUIRE_FALSE(bad.has_value());
  CHECK(bad.error().message.starts_with("R-SER-VARINT at offset 1"));
  CHECK(r.offset() == 1);
}
