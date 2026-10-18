// FAB-INV-SER-07 (SL-ARCH-001 ARCH1-R27): NaN and infinities are refused on encode and on decode;
// -0.0 is encoded as +0.0 and its bit pattern is refused on decode; float text is the shortest
// round-trip spelling of slb_text.py.

#include "sl/fabric/codec.hpp"

#include <doctest/doctest.h>

#include <bit>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;
using sl::errc;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

std::vector<std::uint8_t> f32_wire(std::uint32_t bits) {
  std::vector<std::uint8_t> out = {0x05};
  for (unsigned i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>(bits >> (8U * i)));
  }
  return out;
}

std::vector<std::uint8_t> f64_wire(std::uint64_t bits) {
  std::vector<std::uint8_t> out = {0x06};
  for (unsigned i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::uint8_t>(bits >> (8U * i)));
  }
  return out;
}

std::string text64(double x) {
  schema_registry reg;
  return must(to_text(must(value::f64(x)), type_id_of("f64"), reg));
}

std::string text32(float x) {
  schema_registry reg;
  return must(to_text(must(value::f32(x)), type_id_of("f32"), reg));
}

} // namespace

TEST_CASE("FAB-INV-SER-07: NaN and infinities are refused") {
  CHECK_FALSE(value::f32(std::numeric_limits<float>::quiet_NaN()).has_value());
  CHECK_FALSE(value::f32(std::numeric_limits<float>::infinity()).has_value());
  CHECK_FALSE(value::f64(-std::numeric_limits<double>::infinity()).has_value());
  CHECK_FALSE(value::f64(std::numeric_limits<double>::signaling_NaN()).has_value());
  for (const std::uint32_t bits : {0x7F800000U, 0xFF800000U, 0x7FC00000U, 0x7F800001U}) {
    CAPTURE(bits);
    const auto r = decode(f32_wire(bits));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == errc::decode_bad_float);
    CHECK(r.error().message.starts_with("R-SER-NAN at offset 0:"));
  }
  for (const std::uint64_t bits : {0x7FF0000000000000ULL, 0xFFF0000000000000ULL,
                                   0x7FF8000000000000ULL, 0xFFFFFFFFFFFFFFFFULL}) {
    CAPTURE(bits);
    const auto r = decode(f64_wire(bits));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == errc::decode_bad_float);
  }
}

TEST_CASE("FAB-INV-SER-07: -0.0 is encoded as +0.0 and its pattern is refused on decode") {
  CHECK(must(encode(must(value::f64(-0.0)))) == f64_wire(0));
  CHECK(must(encode(must(value::f32(-0.0F)))) == f32_wire(0));
  const auto neg32 = decode(f32_wire(0x80000000U));
  REQUIRE_FALSE(neg32.has_value());
  CHECK(neg32.error().code == errc::decode_noncanonical);
  CHECK(neg32.error().message.starts_with("R-SER-NEGZERO at offset 0:"));
  const auto neg64 = decode(f64_wire(0x8000000000000000ULL));
  REQUIRE_FALSE(neg64.has_value());
  CHECK(neg64.error().message.starts_with("R-SER-NEGZERO at offset 0:"));
  // Inside a container the offset is the float's tag byte.
  std::vector<std::uint8_t> list = {0x0B, 0x02, 0x01};
  const auto neg = f64_wire(0x8000000000000000ULL);
  list.insert(list.end(), neg.begin(), neg.end());
  CHECK(decode(list).error().message.starts_with("R-SER-NEGZERO at offset 3:"));
  // Subnormals and the extremes are finite and decode.
  for (const std::uint64_t bits :
       {1ULL, 0x000FFFFFFFFFFFFFULL, 0x7FEFFFFFFFFFFFFFULL, 0x8000000000000001ULL}) {
    CAPTURE(bits);
    const value v = must(decode(f64_wire(bits)));
    CHECK(std::bit_cast<std::uint64_t>(v.as_f64()) == bits);
  }
}

TEST_CASE("FAB-INV-SER-07: float text is the shortest round-trip spelling") {
  CHECK(text64(0.0) == "0.0");
  CHECK(text64(-0.0) == "0.0");
  CHECK(text64(1.5) == "1.5");
  CHECK(text64(100.0) == "100.0");
  CHECK(text64(-2.0) == "-2.0");
  CHECK(text64(0.1) == "0.1");
  CHECK(text64(0.0001) == "0.0001");
  CHECK(text64(0.00001) == "1e-05");
  CHECK(text64(1e15) == "1000000000000000.0");
  CHECK(text64(1e16) == "1e+16");
  CHECK(text64(123456789012345680.0) == "1.2345678901234568e+17");
  CHECK(text64(5e-324) == "5e-324");
  CHECK(text64(std::numeric_limits<double>::max()) == "1.7976931348623157e+308");
  CHECK(text64(0.30000000000000004) == "0.30000000000000004");
  CHECK(text32(0.1F) == "0.1");
  CHECK(text32(1.5F) == "1.5");
  CHECK(text32(std::numeric_limits<float>::max()) == "3.4028235e+38");
  CHECK(text32(std::numeric_limits<float>::denorm_min()) == "1e-45");
  CHECK(text32(16777216.0F) == "16777216.0");
  // Every spelling parses back to the same bits.
  schema_registry reg;
  for (const double x : {0.1, 1e-7, 2.5e-308, 6.02214076e23, 1.0 / 3.0}) {
    const std::string t = text64(x);
    CAPTURE(t);
    CHECK(std::bit_cast<std::uint64_t>(must(from_text(t, type_id_of("f64"), reg)).as_f64()) ==
          std::bit_cast<std::uint64_t>(x));
  }
  // Out of range for f32 is a type error, not a silent infinity.
  CHECK(from_text("1e+39", type_id_of("f32"), reg).error().code == errc::type_mismatch);
}
