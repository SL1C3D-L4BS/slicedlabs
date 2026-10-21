// RT-INV-REFL-02 (SL-ARCH-003 ARCH3-R134, ADR-0026): objects with equal fields and different
// padding bytes encode and hash identically; -0.0 and +0.0, and every NaN payload, encode and hash
// as one value; the field selection decides what reaches the bytes and the hash.

#include "sl/runtime/reflect.hpp"
#include "sl/runtime/replay.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace det = sl::runtime::det;
using rt::field_flags;

struct ref_padded {
  std::uint8_t a = 0;
  std::uint32_t b = 0;
  std::uint8_t c = 0;
  double d = 0.0;
  std::uint16_t e = 0;
  float f = 0.0F;
  bool g = false;
};

static_assert(sizeof(ref_padded) > 1 + 4 + 1 + 8 + 2 + 4 + 1, "the reference type has padding");

constexpr field_flags all = field_flags::snapshot | field_flags::hashed;

rt::type_info padded_info() {
  rt::type_builder<ref_padded> b("test.refl.padded");
  SL_RT_FIELD(b, ref_padded, a, all);
  SL_RT_FIELD(b, ref_padded, b, all);
  SL_RT_FIELD(b, ref_padded, c, all);
  SL_RT_FIELD(b, ref_padded, d, all);
  SL_RT_FIELD(b, ref_padded, e, field_flags::snapshot);
  SL_RT_FIELD(b, ref_padded, f, all);
  SL_RT_FIELD(b, ref_padded, g, field_flags::transient);
  return b.build();
}

/// Sets every byte no field covers to `fill`.
void fill_padding(const rt::type_info &t, ref_padded &obj, std::byte fill) {
  std::vector<bool> covered(sizeof(ref_padded), false);
  for (const rt::field_info &f : t.fields) {
    for (std::uint32_t i = 0; i < f.size; ++i) {
      covered[f.offset + i] = true;
    }
  }
  const std::span<std::byte> bytes = std::as_writable_bytes(std::span<ref_padded, 1>(&obj, 1));
  for (std::size_t i = 0; i < covered.size(); ++i) {
    if (!covered[i]) {
      bytes[i] = fill;
    }
  }
}

std::vector<std::byte> encoded(const rt::type_info &t, const ref_padded &obj, field_flags select) {
  std::vector<std::byte> out;
  rt::encode(t, &obj, select, out);
  return out;
}

sl::fabric::digest hashed(const rt::type_info &t, const ref_padded &obj, field_flags select) {
  det::state_hasher h;
  rt::hash_into(t, &obj, select, h);
  return h.finish();
}

bool same_raw_bytes(const ref_padded &x, const ref_padded &y) {
  return std::ranges::equal(std::as_bytes(std::span<const ref_padded, 1>(&x, 1)),
                            std::as_bytes(std::span<const ref_padded, 1>(&y, 1)));
}

ref_padded sample() {
  ref_padded v;
  v.a = 0x11;
  v.b = 0x2233'4455U;
  v.c = 0x66;
  v.d = 1.5;
  v.e = 0x7788;
  v.f = -2.25F;
  v.g = true;
  return v;
}

} // namespace

TEST_CASE("RT-INV-REFL-02: different padding bytes encode and hash identically") {
  const rt::type_info t = padded_info();
  ref_padded x = sample();
  ref_padded y = sample();
  fill_padding(t, x, std::byte{0x00});
  fill_padding(t, y, std::byte{0xFF});
  REQUIRE_FALSE(same_raw_bytes(x, y));
  for (const field_flags select : {all, field_flags::snapshot, field_flags::hashed,
                                   field_flags::transient, all | field_flags::transient}) {
    CHECK(encoded(t, x, select) == encoded(t, y, select));
    CHECK(hashed(t, x, select) == hashed(t, y, select));
  }
  CHECK(encoded(t, x, all | field_flags::transient).size() == 1 + 4 + 1 + 8 + 2 + 4 + 1);
}

TEST_CASE("RT-INV-REFL-02: -0.0 and +0.0, and every NaN payload, encode and hash equal") {
  const rt::type_info t = padded_info();
  ref_padded pos = sample();
  ref_padded neg = sample();
  pos.d = 0.0;
  neg.d = -0.0;
  pos.f = 0.0F;
  neg.f = -0.0F;
  CHECK(encoded(t, pos, all) == encoded(t, neg, all));
  CHECK(hashed(t, pos, all) == hashed(t, neg, all));
  ref_padded quiet = sample();
  ref_padded payload = sample();
  quiet.d = std::numeric_limits<double>::quiet_NaN();
  payload.d = std::bit_cast<double>(std::uint64_t{0xFFF0'0000'0000'0001ULL});
  quiet.f = std::numeric_limits<float>::quiet_NaN();
  payload.f = std::bit_cast<float>(std::uint32_t{0x7F80'0001U});
  CHECK(encoded(t, quiet, all) == encoded(t, payload, all));
  CHECK(hashed(t, quiet, all) == hashed(t, payload, all));
  const std::vector<std::byte> bytes = encoded(t, payload, all);
  // d at offset 6 of the encoding (a, b, c), canonical quiet NaN 0x7FF8000000000000 little-endian.
  CHECK(bytes[6 + 6] == std::byte{0xF8});
  CHECK(bytes[6 + 7] == std::byte{0x7F});
}

TEST_CASE("RT-INV-REFL-02: the selection decides which fields reach the hash") {
  const rt::type_info t = padded_info();
  ref_padded x = sample();
  ref_padded y = sample();
  y.g = false; // transient only
  y.e = 1;     // snapshot only
  CHECK(hashed(t, x, field_flags::hashed) == hashed(t, y, field_flags::hashed));
  CHECK(encoded(t, x, field_flags::hashed) == encoded(t, y, field_flags::hashed));
  CHECK(encoded(t, x, field_flags::snapshot) != encoded(t, y, field_flags::snapshot));
  CHECK(hashed(t, x, field_flags::transient) != hashed(t, y, field_flags::transient));
  y = sample();
  y.b = 0;
  CHECK(hashed(t, x, field_flags::hashed) != hashed(t, y, field_flags::hashed));
  CHECK(encoded(t, x, field_flags::none).empty());
}
