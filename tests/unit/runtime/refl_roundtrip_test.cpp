// RT-INV-REFL-03 (SL-ARCH-003 ARCH3-R134, ADR-0026): encode then decode round trips every
// reference type bit-exactly, floats canonical (-0.0 as +0.0, one quiet NaN); the byte layout is
// little-endian in declaration order; decode refuses a wrong length or a bool byte above 1 and then
// leaves the object unchanged.

#include "sl/core/error.hpp"
#include "sl/fabric/id.hpp"
#include "sl/runtime/reflect.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace {

namespace rt = sl::runtime;
using rt::field_flags;

struct ref_scalars {
  bool b = false;
  std::int8_t i8 = 0;
  std::int16_t i16 = 0;
  std::int32_t i32 = 0;
  std::int64_t i64 = 0;
  std::uint8_t u8 = 0;
  std::uint16_t u16 = 0;
  std::uint32_t u32 = 0;
  std::uint64_t u64 = 0;
  float f32 = 0.0F;
  double f64 = 0.0;
};

struct ref_handles {
  rt::entity_bits e;
  sl::fabric::record_id id;
  std::array<std::byte, 5> blob{};
  float nan = 0.0F;
  double neg = 0.0;
};

constexpr field_flags all = field_flags::snapshot | field_flags::hashed | field_flags::transient;

rt::type_info scalars_info() {
  rt::type_builder<ref_scalars> b("test.refl.scalars");
  SL_RT_FIELD(b, ref_scalars, b, field_flags::snapshot);
  SL_RT_FIELD(b, ref_scalars, i8, field_flags::snapshot);
  SL_RT_FIELD(b, ref_scalars, i16, field_flags::hashed);
  SL_RT_FIELD(b, ref_scalars, i32, field_flags::snapshot);
  SL_RT_FIELD(b, ref_scalars, i64, field_flags::snapshot);
  SL_RT_FIELD(b, ref_scalars, u8, field_flags::transient);
  SL_RT_FIELD(b, ref_scalars, u16, field_flags::snapshot);
  SL_RT_FIELD(b, ref_scalars, u32, field_flags::snapshot);
  SL_RT_FIELD(b, ref_scalars, u64, field_flags::snapshot);
  SL_RT_FIELD(b, ref_scalars, f32, field_flags::snapshot);
  SL_RT_FIELD(b, ref_scalars, f64, field_flags::snapshot);
  return b.build();
}

rt::type_info handles_info() {
  rt::type_builder<ref_handles> b("test.refl.handles");
  SL_RT_FIELD(b, ref_handles, e, field_flags::snapshot);
  SL_RT_FIELD(b, ref_handles, id, field_flags::snapshot);
  SL_RT_FIELD(b, ref_handles, blob, field_flags::snapshot);
  SL_RT_FIELD(b, ref_handles, nan, field_flags::snapshot);
  SL_RT_FIELD(b, ref_handles, neg, field_flags::snapshot);
  return b.build();
}

ref_scalars scalars_sample() {
  ref_scalars v;
  v.b = true;
  v.i8 = std::numeric_limits<std::int8_t>::min();
  v.i16 = -2;
  v.i32 = std::numeric_limits<std::int32_t>::min();
  v.i64 = -5;
  v.u8 = 255;
  v.u16 = 0x1234;
  v.u32 = 0xDEAD'BEEFU;
  v.u64 = std::numeric_limits<std::uint64_t>::max();
  v.f32 = std::bit_cast<float>(std::uint32_t{0xFFC0'0001U}); // a negative NaN with a payload
  v.f64 = -0.0;
  return v;
}

ref_handles handles_sample() {
  ref_handles v;
  v.e = rt::entity_bits{.index = 0x0102'0304U, .generation = 0xA0B0'C0D0U};
  v.id = sl::fabric::well_known(sl::fabric::id_class::entity, 7);
  v.blob = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}};
  v.nan = std::bit_cast<float>(std::uint32_t{0x7F80'0001U});
  v.neg = 3.25;
  return v;
}

std::uint32_t bits(float f) { return std::bit_cast<std::uint32_t>(f); }
std::uint64_t bits(double d) { return std::bit_cast<std::uint64_t>(d); }

template <class T> T garbage() {
  T v;
  std::ranges::fill(std::as_writable_bytes(std::span<T, 1>(&v, 1)), std::byte{0xA5});
  return v;
}

} // namespace

TEST_CASE("RT-INV-REFL-03: scalars round trip bit-exactly with canonical floats") {
  const rt::type_info t = scalars_info();
  const ref_scalars in = scalars_sample();
  std::vector<std::byte> bytes;
  rt::encode(t, &in, all, bytes);
  CHECK(bytes.size() == rt::encoded_size(t, all));
  auto out = garbage<ref_scalars>();
  REQUIRE(rt::decode(t, bytes, all, &out).has_value());
  CHECK(out.b == in.b);
  CHECK(out.i8 == in.i8);
  CHECK(out.i16 == in.i16);
  CHECK(out.i32 == in.i32);
  CHECK(out.i64 == in.i64);
  CHECK(out.u8 == in.u8);
  CHECK(out.u16 == in.u16);
  CHECK(out.u32 == in.u32);
  CHECK(out.u64 == in.u64);
  CHECK(bits(out.f32) == 0x7FC0'0000U);
  CHECK(bits(out.f64) == 0U);
  std::vector<std::byte> again;
  rt::encode(t, &out, all, again);
  CHECK(again == bytes);
}

TEST_CASE("RT-INV-REFL-03: entities, record ids and fixed bytes round trip") {
  const rt::type_info t = handles_info();
  const ref_handles in = handles_sample();
  std::vector<std::byte> bytes;
  rt::encode(t, &in, field_flags::snapshot, bytes);
  REQUIRE(bytes.size() == 8 + 16 + 5 + 4 + 8);
  // entity: index then generation, each little-endian.
  CHECK(bytes[0] == std::byte{0x04});
  CHECK(bytes[3] == std::byte{0x01});
  CHECK(bytes[4] == std::byte{0xD0});
  CHECK(bytes[7] == std::byte{0xA0});
  CHECK(std::memcmp(bytes.data() + 8, in.id.bytes().data(), 16) == 0);
  auto out = garbage<ref_handles>();
  REQUIRE(rt::decode(t, bytes, field_flags::snapshot, &out).has_value());
  CHECK(out.e == in.e);
  CHECK(out.id == in.id);
  CHECK(out.blob == in.blob);
  CHECK(bits(out.nan) == 0x7FC0'0000U);
  CHECK(bits(out.neg) == bits(in.neg));
}

TEST_CASE("RT-INV-REFL-03: the byte layout is little-endian in declaration order") {
  const rt::type_info t = scalars_info();
  ref_scalars v;
  v.b = true;
  v.i8 = -1;
  v.i32 = 0x0102'0304;
  v.u16 = 0xBEEF;
  v.f32 = 1.0F;
  std::vector<std::byte> bytes;
  rt::encode(t, &v, field_flags::snapshot, bytes);
  // b, i8, i32, i64, u16, u32, u64, f32, f64 (i16 is hashed only, u8 transient only).
  const std::vector<std::uint8_t> want{0x01, 0xFF, 0x04, 0x03, 0x02, 0x01, 0, 0, 0, 0, 0, 0, 0, 0,
                                       0xEF, 0xBE, 0,    0,    0,    0,    0, 0, 0, 0, 0, 0, 0, 0,
                                       0,    0,    0x80, 0x3F, 0,    0,    0, 0, 0, 0, 0, 0};
  REQUIRE(bytes.size() == want.size());
  for (std::size_t i = 0; i < want.size(); ++i) {
    CAPTURE(i);
    CHECK(std::to_integer<std::uint8_t>(bytes[i]) == want[i]);
  }
}

TEST_CASE("RT-INV-REFL-03: decode refuses a wrong length or bool byte and changes nothing") {
  const rt::type_info t = scalars_info();
  const ref_scalars in = scalars_sample();
  std::vector<std::byte> bytes;
  rt::encode(t, &in, field_flags::snapshot, bytes);
  auto out = garbage<ref_scalars>();
  const ref_scalars before = out;
  const std::span<const std::byte> all_bytes(bytes);
  const auto short_read =
      rt::decode(t, all_bytes.first(bytes.size() - 1), field_flags::snapshot, &out);
  REQUIRE_FALSE(short_read.has_value());
  CHECK(short_read.error().code == sl::errc::invalid_argument);
  bytes[0] = std::byte{2};
  const auto bad_bool = rt::decode(t, bytes, field_flags::snapshot, &out);
  REQUIRE_FALSE(bad_bool.has_value());
  CHECK(bad_bool.error().code == sl::errc::invalid_argument);
  CHECK(std::ranges::equal(std::as_bytes(std::span<const ref_scalars, 1>(&out, 1)),
                           std::as_bytes(std::span<const ref_scalars, 1>(&before, 1))));
  const auto null_obj = rt::decode(t, bytes, field_flags::snapshot, nullptr);
  REQUIRE_FALSE(null_obj.has_value());
  CHECK(null_obj.error().code == sl::errc::invalid_argument);
}
