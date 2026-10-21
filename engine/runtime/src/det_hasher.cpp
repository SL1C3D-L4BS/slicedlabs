// state_hasher (WO-084, SL-ARCH-003 ARCH3-R088, `limits` canonical_nan_f32/f64, ADR-0026): one
// tag byte per value, then its little-endian bytes; floats go through their bit pattern with -0.0
// written as +0.0 and every NaN as the canonical quiet NaN; byte and text strings carry a u64le
// length. Bytes reach sl::fabric::sha256 in 64-byte chunks, so no reinterpretation is needed.

#include "sl/fabric/digest.hpp"
#include "sl/runtime/replay.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace sl::runtime::det {

namespace {

constexpr std::uint32_t canonical_nan_f32 = 0x7FC0'0000U;
constexpr std::uint64_t canonical_nan_f64 = 0x7FF8'0000'0000'0000ULL;

constexpr std::uint32_t canonical(std::uint32_t bits) noexcept {
  if ((bits & 0x7F80'0000U) == 0x7F80'0000U && (bits & 0x007F'FFFFU) != 0) {
    return canonical_nan_f32;
  }
  return bits == 0x8000'0000U ? 0 : bits;
}

constexpr std::uint64_t canonical(std::uint64_t bits) noexcept {
  if ((bits & 0x7FF0'0000'0000'0000ULL) == 0x7FF0'0000'0000'0000ULL &&
      (bits & 0x000F'FFFF'FFFF'FFFFULL) != 0) {
    return canonical_nan_f64;
  }
  return bits == 0x8000'0000'0000'0000ULL ? 0 : bits;
}

static_assert(canonical(0xFFC0'0001U) == canonical_nan_f32);
static_assert(canonical(std::uint64_t{0x8000'0000'0000'0000ULL}) == 0);

} // namespace

void state_hasher::tagged(std::uint8_t tag, std::uint64_t v, std::size_t width) noexcept {
  std::array<std::uint8_t, 9> storage{};
  const std::span<std::uint8_t> out(storage);
  out[0] = tag;
  for (std::size_t i = 0; i < width; ++i) {
    out[1 + i] = static_cast<std::uint8_t>((v >> (8U * i)) & 0xFFU);
  }
  sha_.update(out.first(1 + width));
}

void state_hasher::length_prefixed(std::uint8_t tag, std::span<const std::byte> v) noexcept {
  tagged(tag, static_cast<std::uint64_t>(v.size()), 8);
  std::array<std::uint8_t, fabric::sha256::block_size> storage{};
  const std::span<std::uint8_t> chunk(storage);
  while (!v.empty()) {
    const std::size_t n = std::min(v.size(), chunk.size());
    for (std::size_t i = 0; i < n; ++i) {
      chunk[i] = std::to_integer<std::uint8_t>(v[i]);
    }
    sha_.update(chunk.first(n));
    v = v.subspan(n);
  }
}

void state_hasher::u8(std::uint8_t v) noexcept { tagged(0x01, v, 1); }
void state_hasher::u16(std::uint16_t v) noexcept { tagged(0x02, v, 2); }
void state_hasher::u32(std::uint32_t v) noexcept { tagged(0x04, v, 4); }
void state_hasher::u64(std::uint64_t v) noexcept { tagged(0x08, v, 8); }
void state_hasher::i8(std::int8_t v) noexcept { tagged(0x11, std::bit_cast<std::uint8_t>(v), 1); }
void state_hasher::i16(std::int16_t v) noexcept {
  tagged(0x12, std::bit_cast<std::uint16_t>(v), 2);
}
void state_hasher::i32(std::int32_t v) noexcept {
  tagged(0x14, std::bit_cast<std::uint32_t>(v), 4);
}
void state_hasher::i64(std::int64_t v) noexcept {
  tagged(0x18, std::bit_cast<std::uint64_t>(v), 8);
}
void state_hasher::boolean(bool v) noexcept { tagged(0x40, v ? 1U : 0U, 1); }
void state_hasher::f32(float v) noexcept {
  tagged(0x24, canonical(std::bit_cast<std::uint32_t>(v)), 4);
}
void state_hasher::f64(double v) noexcept {
  tagged(0x28, canonical(std::bit_cast<std::uint64_t>(v)), 8);
}
void state_hasher::bytes(std::span<const std::byte> v) noexcept { length_prefixed(0x30, v); }
void state_hasher::str(std::string_view v) noexcept {
  length_prefixed(0x31, std::as_bytes(std::span(v)));
}

fabric::digest state_hasher::finish() const noexcept {
  return fabric::digest{.algo = fabric::hash_algo::sha256, .bytes = sha_.finish()};
}

} // namespace sl::runtime::det
