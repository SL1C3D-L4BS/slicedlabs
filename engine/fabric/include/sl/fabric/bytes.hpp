#pragma once

// Byte buffers for the Fabric encodings (WO-037, SL-ARCH-001 ARCH1-R26 and ARCH1-R30, ADR-0012).
//
// byte_writer appends little-endian integers, raw bytes and unsigned LEB128 varints (shortest form
// only). byte_reader reads them back with every read bounded by the remaining input: a read past
// the end is `decode_truncated` (R-SER-TRUNC), an overlong or over-64-bit varint is
// `decode_noncanonical` (R-SER-VARINT); a failed read leaves the position unchanged. Messages name
// the rule id and the byte offset.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "sl/core/error.hpp"

namespace sl::fabric {

/// A read-only view of bytes.
using byte_span = std::span<const std::uint8_t>;

/// The longest LEB128 encoding of a 64-bit value.
inline constexpr std::size_t max_uvarint_size = 10;

/// The number of bytes of the shortest LEB128 encoding of `v` (1 to 10).
[[nodiscard]] constexpr std::size_t uvarint_size(std::uint64_t v) noexcept {
  std::size_t n = 1;
  while (v >= 0x80U) {
    v >>= 7U;
    ++n;
  }
  return n;
}

/// Writes the shortest LEB128 encoding of `v` to the front of `out`; returns its length.
constexpr std::size_t encode_uvarint(std::uint64_t v,
                                     std::span<std::uint8_t, max_uvarint_size> out) noexcept {
  std::size_t n = 0;
  while (v >= 0x80U) {
    out[n++] = static_cast<std::uint8_t>((v & 0x7FU) | 0x80U);
    v >>= 7U;
  }
  out[n++] = static_cast<std::uint8_t>(v);
  return n;
}

/// An append-only byte buffer.
class byte_writer {
public:
  byte_writer() = default;

  void u8(std::uint8_t v) { buf_.push_back(v); }
  void u32le(std::uint32_t v);
  void u64le(std::uint64_t v);
  void uvarint(std::uint64_t v);
  void bytes(byte_span b);

  [[nodiscard]] std::size_t size() const noexcept { return buf_.size(); }
  [[nodiscard]] byte_span view() const noexcept { return buf_; }
  [[nodiscard]] const std::vector<std::uint8_t> &data() const noexcept { return buf_; }
  /// Moves the buffer out and leaves the writer empty.
  [[nodiscard]] std::vector<std::uint8_t> take() noexcept;
  void clear() noexcept { buf_.clear(); }

private:
  std::vector<std::uint8_t> buf_;
};

/// A bounded cursor over bytes it does not own.
class byte_reader {
public:
  explicit byte_reader(byte_span input) noexcept : in_(input) {}

  [[nodiscard]] result<std::uint8_t> u8();
  [[nodiscard]] result<std::uint32_t> u32le();
  [[nodiscard]] result<std::uint64_t> u64le();
  /// Strict unsigned LEB128: shortest form, at most 64 bits.
  [[nodiscard]] result<std::uint64_t> uvarint();
  /// The next `n` bytes as a view into the input.
  [[nodiscard]] result<byte_span> bytes(std::size_t n);
  /// `decode_noncanonical` (R-SER-TRAILING) unless every byte has been read.
  [[nodiscard]] result<void> expect_end() const;

  [[nodiscard]] std::size_t offset() const noexcept { return pos_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return in_.size() - pos_; }
  [[nodiscard]] bool at_end() const noexcept { return pos_ == in_.size(); }

private:
  byte_span in_;
  std::size_t pos_ = 0;
};

} // namespace sl::fabric
