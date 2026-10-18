#include "sl/fabric/digest.hpp"

namespace sl::fabric {

std::string digest::to_hex() const {
  static constexpr std::string_view hex = "0123456789abcdef";
  std::string out;
  out.reserve(2 * bytes.size());
  for (const std::uint8_t b : bytes) {
    out.push_back(hex[b >> 4U]);
    out.push_back(hex[b & 0x0FU]);
  }
  return out;
}

digest hash_object(const frame_header &header, byte_span payload) noexcept {
  static constexpr std::array<std::uint8_t, 10> domain = {'s', 'l', 'f', '-', 'o',
                                                          'b', 'j', '/', '1', 0x00};
  const std::array<std::uint8_t, 4> fields = {header.major, header.minor,
                                              static_cast<std::uint8_t>(header.kind), header.flags};
  std::array<std::uint8_t, max_uvarint_size> length{};
  const std::size_t length_size = encode_uvarint(payload.size(), length);

  sha256 h;
  h.update(byte_span{domain});
  h.update(byte_span{fields});
  h.update(byte_span{length}.first(length_size));
  h.update(payload);
  return digest{.algo = hash_algo::sha256, .bytes = h.finish()};
}

} // namespace sl::fabric
