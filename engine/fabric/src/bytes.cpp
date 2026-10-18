#include "sl/fabric/bytes.hpp"

#include <format>
#include <utility>

namespace sl::fabric {

namespace {

std::unexpected<error> truncated(std::size_t offset, std::size_t want, std::size_t have) {
  return make_error(
      errc::decode_truncated,
      std::format("R-SER-TRUNC at offset {}: need {} bytes, {} left", offset, want, have));
}

} // namespace

void byte_writer::u32le(std::uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) {
    buf_.push_back(static_cast<std::uint8_t>(v >> (8U * i)));
  }
}

void byte_writer::u64le(std::uint64_t v) {
  for (unsigned i = 0; i < 8; ++i) {
    buf_.push_back(static_cast<std::uint8_t>(v >> (8U * i)));
  }
}

void byte_writer::uvarint(std::uint64_t v) {
  std::array<std::uint8_t, max_uvarint_size> tmp{};
  const std::size_t n = encode_uvarint(v, tmp);
  bytes(byte_span{tmp}.first(n));
}

void byte_writer::bytes(byte_span b) { buf_.insert(buf_.end(), b.begin(), b.end()); }

std::vector<std::uint8_t> byte_writer::take() noexcept {
  std::vector<std::uint8_t> out = std::move(buf_);
  buf_.clear();
  return out;
}

result<std::uint8_t> byte_reader::u8() {
  if (remaining() < 1) {
    return truncated(pos_, 1, remaining());
  }
  return in_[pos_++];
}

result<std::uint32_t> byte_reader::u32le() {
  if (remaining() < 4) {
    return truncated(pos_, 4, remaining());
  }
  std::uint32_t v = 0;
  for (unsigned i = 0; i < 4; ++i) {
    v |= static_cast<std::uint32_t>(in_[pos_ + i]) << (8U * i);
  }
  pos_ += 4;
  return v;
}

result<std::uint64_t> byte_reader::u64le() {
  if (remaining() < 8) {
    return truncated(pos_, 8, remaining());
  }
  std::uint64_t v = 0;
  for (unsigned i = 0; i < 8; ++i) {
    v |= static_cast<std::uint64_t>(in_[pos_ + i]) << (8U * i);
  }
  pos_ += 8;
  return v;
}

result<std::uint64_t> byte_reader::uvarint() {
  std::uint64_t v = 0;
  std::size_t i = 0;
  while (true) {
    if (pos_ + i >= in_.size()) {
      return truncated(pos_ + i, 1, 0);
    }
    const std::uint8_t b = in_[pos_ + i];
    // The tenth byte carries bit 63 only, and nothing may follow it.
    if (i == max_uvarint_size - 1 && (b & 0xFEU) != 0) {
      return make_error(errc::decode_noncanonical,
                        std::format("R-SER-VARINT at offset {}: varint exceeds 64 bits", pos_));
    }
    v |= static_cast<std::uint64_t>(b & 0x7FU) << (7U * i);
    if ((b & 0x80U) == 0) {
      if (b == 0 && i > 0) {
        return make_error(errc::decode_noncanonical,
                          std::format("R-SER-VARINT at offset {}: overlong varint", pos_));
      }
      pos_ += i + 1;
      return v;
    }
    ++i;
  }
}

result<byte_span> byte_reader::bytes(std::size_t n) {
  if (remaining() < n) {
    return truncated(pos_, n, remaining());
  }
  const byte_span out = in_.subspan(pos_, n);
  pos_ += n;
  return out;
}

result<void> byte_reader::expect_end() const {
  if (!at_end()) {
    return make_error(
        errc::decode_noncanonical,
        std::format("R-SER-TRAILING at offset {}: {} trailing bytes", pos_, remaining()));
  }
  return {};
}

} // namespace sl::fabric
