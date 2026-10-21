// The field walk (WO-087, SL-ARCH-003 ARCH3-R134, `determinism`, `limits` canonical_nan_f32/f64,
// ADR-0026): encode, decode and hash_into visit the fields whose flags intersect the selection in
// declaration order and touch only each field's own bytes (memcpy of exactly its size), so padding
// is never read. Integers are written little-endian byte by byte, floats by canonical bit pattern
// (-0.0 as +0.0, every NaN as the one quiet NaN), bool as 0 or 1, entity as index then generation.

#include "sl/runtime/reflect.hpp"

#include "sl/core/assert.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <span>
#include <vector>

namespace sl::runtime {

namespace {

constexpr std::uint32_t canonical_nan_f32 = 0x7FC0'0000U;
constexpr std::uint64_t canonical_nan_f64 = 0x7FF8'0000'0000'0000ULL;

constexpr std::uint32_t canonical32(std::uint32_t bits) noexcept {
  if ((bits & 0x7F80'0000U) == 0x7F80'0000U && (bits & 0x007F'FFFFU) != 0) {
    return canonical_nan_f32;
  }
  return bits == 0x8000'0000U ? 0 : bits;
}

constexpr std::uint64_t canonical64(std::uint64_t bits) noexcept {
  if ((bits & 0x7FF0'0000'0000'0000ULL) == 0x7FF0'0000'0000'0000ULL &&
      (bits & 0x000F'FFFF'FFFF'FFFFULL) != 0) {
    return canonical_nan_f64;
  }
  return bits == 0x8000'0000'0000'0000ULL ? 0 : bits;
}

static_assert(canonical32(0xFFC0'0001U) == canonical_nan_f32);
static_assert(canonical64(0x8000'0000'0000'0000ULL) == 0);

bool selected(const field_info &f, field_flags select) noexcept { return any(f.flags & select); }

const std::byte *at(const void *obj, std::uint32_t offset) noexcept {
  return static_cast<const std::byte *>(obj) + offset;
}

std::byte *at_mut(void *obj, std::uint32_t offset) noexcept {
  return static_cast<std::byte *>(obj) + offset;
}

/// The field's bytes as an unsigned integer of its width (1, 2, 4 or 8).
std::uint64_t load_bits(const std::byte *p, std::uint32_t width) noexcept {
  switch (width) {
  case 1: {
    std::uint8_t v = 0;
    std::memcpy(&v, p, 1);
    return v;
  }
  case 2: {
    std::uint16_t v = 0;
    std::memcpy(&v, p, 2);
    return v;
  }
  case 4: {
    std::uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
  }
  default: {
    std::uint64_t v = 0;
    std::memcpy(&v, p, 8);
    return v;
  }
  }
}

void store_bits(std::byte *p, std::uint64_t bits, std::uint32_t width) noexcept {
  switch (width) {
  case 1: {
    const auto v = static_cast<std::uint8_t>(bits);
    std::memcpy(p, &v, 1);
    return;
  }
  case 2: {
    const auto v = static_cast<std::uint16_t>(bits);
    std::memcpy(p, &v, 2);
    return;
  }
  case 4: {
    const auto v = static_cast<std::uint32_t>(bits);
    std::memcpy(p, &v, 4);
    return;
  }
  default:
    std::memcpy(p, &bits, 8);
    return;
  }
}

void put_le(std::vector<std::byte> &out, std::uint64_t v, std::uint32_t width) noexcept {
  for (std::uint32_t i = 0; i < width; ++i) {
    out.push_back(static_cast<std::byte>((v >> (8U * i)) & 0xFFU));
  }
}

std::uint64_t get_le(std::span<const std::byte> in, std::uint32_t width) noexcept {
  std::uint64_t v = 0;
  for (std::uint32_t i = 0; i < width; ++i) {
    v |= std::to_integer<std::uint64_t>(in[i]) << (8U * i);
  }
  return v;
}

/// The canonical encoded bits of a scalar field of width 1 to 8 (not record_id or bytes_fixed).
std::uint64_t scalar_bits(const field_info &f, const void *obj) noexcept {
  const std::byte *p = at(obj, f.offset);
  switch (f.kind) {
  case field_kind::boolean:
    return load_bits(p, 1) != 0 ? 1 : 0;
  case field_kind::f32:
    return canonical32(static_cast<std::uint32_t>(load_bits(p, 4)));
  case field_kind::f64:
    return canonical64(load_bits(p, 8));
  default:
    return load_bits(p, f.size);
  }
}

bool is_raw(field_kind k) noexcept {
  return k == field_kind::record_id || k == field_kind::bytes_fixed;
}

} // namespace

std::size_t encoded_size(const type_info &t, field_flags select) noexcept {
  return std::accumulate(t.fields.begin(), t.fields.end(), std::size_t{0},
                         [select](std::size_t n, const field_info &f) noexcept {
                           return selected(f, select) ? n + f.size : n;
                         });
}

void encode(const type_info &t, const void *obj, field_flags select,
            std::vector<std::byte> &out) noexcept {
  SL_ASSERT(obj != nullptr, "reflect encode: null object");
  out.reserve(out.size() + encoded_size(t, select));
  for (const field_info &f : t.fields) {
    if (!selected(f, select)) {
      continue;
    }
    if (is_raw(f.kind)) {
      const std::span<const std::byte> raw(at(obj, f.offset), f.size);
      out.insert(out.end(), raw.begin(), raw.end());
    } else if (f.kind == field_kind::entity) {
      put_le(out, load_bits(at(obj, f.offset), 4), 4);
      put_le(out, load_bits(at(obj, f.offset + 4), 4), 4);
    } else {
      put_le(out, scalar_bits(f, obj), f.size);
    }
  }
}

result<void> decode(const type_info &t, std::span<const std::byte> in, field_flags select,
                    void *obj) noexcept {
  if (obj == nullptr) {
    return make_error(errc::invalid_argument, "reflect decode: null object");
  }
  if (in.size() != encoded_size(t, select)) {
    return make_error(errc::invalid_argument, "reflect decode: the byte count does not match");
  }
  std::size_t pos = 0;
  for (const field_info &f : t.fields) {
    if (!selected(f, select)) {
      continue;
    }
    if (f.kind == field_kind::boolean && std::to_integer<unsigned>(in[pos]) > 1U) {
      return make_error(errc::invalid_argument, "reflect decode: a bool byte is not 0 or 1");
    }
    pos += f.size;
  }
  pos = 0;
  for (const field_info &f : t.fields) {
    if (!selected(f, select)) {
      continue;
    }
    const std::span<const std::byte> src = in.subspan(pos, f.size);
    if (is_raw(f.kind)) {
      std::memcpy(at_mut(obj, f.offset), src.data(), f.size);
    } else if (f.kind == field_kind::entity) {
      store_bits(at_mut(obj, f.offset), get_le(src, 4), 4);
      store_bits(at_mut(obj, f.offset + 4), get_le(src.subspan(4), 4), 4);
    } else if (f.kind == field_kind::boolean) {
      const bool b = get_le(src, 1) != 0;
      std::memcpy(at_mut(obj, f.offset), &b, 1);
    } else {
      store_bits(at_mut(obj, f.offset), get_le(src, f.size), f.size);
    }
    pos += f.size;
  }
  return {};
}

void hash_into(const type_info &t, const void *obj, field_flags select,
               det::state_hasher &h) noexcept {
  SL_ASSERT(obj != nullptr, "reflect hash_into: null object");
  for (const field_info &f : t.fields) {
    if (!selected(f, select)) {
      continue;
    }
    switch (f.kind) {
    case field_kind::boolean:
      h.boolean(scalar_bits(f, obj) != 0);
      break;
    case field_kind::i8:
      h.i8(static_cast<std::int8_t>(scalar_bits(f, obj)));
      break;
    case field_kind::i16:
      h.i16(static_cast<std::int16_t>(scalar_bits(f, obj)));
      break;
    case field_kind::i32:
      h.i32(static_cast<std::int32_t>(scalar_bits(f, obj)));
      break;
    case field_kind::i64:
      h.i64(static_cast<std::int64_t>(scalar_bits(f, obj)));
      break;
    case field_kind::u8:
      h.u8(static_cast<std::uint8_t>(scalar_bits(f, obj)));
      break;
    case field_kind::u16:
      h.u16(static_cast<std::uint16_t>(scalar_bits(f, obj)));
      break;
    case field_kind::u32:
      h.u32(static_cast<std::uint32_t>(scalar_bits(f, obj)));
      break;
    case field_kind::u64:
      h.u64(scalar_bits(f, obj));
      break;
    case field_kind::f32: {
      float v = 0.0F;
      const auto bits = static_cast<std::uint32_t>(scalar_bits(f, obj));
      std::memcpy(&v, &bits, sizeof v);
      h.f32(v);
      break;
    }
    case field_kind::f64: {
      double v = 0.0;
      const std::uint64_t bits = scalar_bits(f, obj);
      std::memcpy(&v, &bits, sizeof v);
      h.f64(v);
      break;
    }
    case field_kind::entity:
      h.u32(static_cast<std::uint32_t>(load_bits(at(obj, f.offset), 4)));
      h.u32(static_cast<std::uint32_t>(load_bits(at(obj, f.offset + 4), 4)));
      break;
    case field_kind::record_id:
    case field_kind::bytes_fixed:
      h.bytes(std::span<const std::byte>(at(obj, f.offset), f.size));
      break;
    }
  }
}

} // namespace sl::runtime
