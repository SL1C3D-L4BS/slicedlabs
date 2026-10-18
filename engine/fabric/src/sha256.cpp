// SHA-256 written from FIPS 180-4 (sections 4.1.2, 4.2.2, 5.1.1, 5.3.3 and 6.2); no third-party
// code (ADR-0011, SL-GOV-008 build versus adopt).

#include "sl/fabric/digest.hpp"

#include <algorithm>
#include <bit>

namespace sl::fabric {

namespace {

// First 32 bits of the fractional parts of the cube roots of the first 64 primes (FIPS 180-4
// 4.2.2).
constexpr std::array<std::uint32_t, 64> round_constants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U};

// First 32 bits of the fractional parts of the square roots of the first 8 primes (5.3.3).
constexpr std::array<std::uint32_t, 8> initial_state = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U,
                                                        0xa54ff53aU, 0x510e527fU, 0x9b05688cU,
                                                        0x1f83d9abU, 0x5be0cd19U};

constexpr std::uint32_t load_be32(const std::uint8_t *p) noexcept {
  return (static_cast<std::uint32_t>(p[0]) << 24U) | (static_cast<std::uint32_t>(p[1]) << 16U) |
         (static_cast<std::uint32_t>(p[2]) << 8U) | static_cast<std::uint32_t>(p[3]);
}

} // namespace

void sha256::reset() noexcept {
  state_ = initial_state;
  buffer_.fill(0);
  buffered_ = 0;
  total_ = 0;
}

// The round loops index fixed-size arrays with loop counters bounded by their sizes; Debug builds
// check every subscript (_GLIBCXX_ASSERTIONS).
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index): loop-bounded indices, see above
void sha256::compress(const std::uint8_t *block) noexcept {
  std::array<std::uint32_t, 64> w{};
  for (std::size_t t = 0; t < 16; ++t) {
    w[t] = load_be32(block + (4 * t));
  }
  for (std::size_t t = 16; t < 64; ++t) {
    const std::uint32_t s0 = std::rotr(w[t - 15], 7) ^ std::rotr(w[t - 15], 18) ^ (w[t - 15] >> 3U);
    const std::uint32_t s1 = std::rotr(w[t - 2], 17) ^ std::rotr(w[t - 2], 19) ^ (w[t - 2] >> 10U);
    w[t] = w[t - 16] + s0 + w[t - 7] + s1;
  }
  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];
  for (std::size_t t = 0; t < 64; ++t) {
    const std::uint32_t big_s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ (~e & g);
    const std::uint32_t t1 = h + big_s1 + ch + round_constants[t] + w[t];
    const std::uint32_t big_s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t t2 = big_s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void sha256::update(byte_span data) noexcept {
  total_ += data.size();
  if (buffered_ > 0) {
    const std::size_t take = std::min(block_size - buffered_, data.size());
    std::copy_n(data.begin(), take, buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_));
    buffered_ += take;
    data = data.subspan(take);
    if (buffered_ < block_size) {
      return;
    }
    compress(buffer_.data());
    buffered_ = 0;
  }
  while (data.size() >= block_size) {
    compress(data.data());
    data = data.subspan(block_size);
  }
  std::ranges::copy(data, buffer_.begin());
  buffered_ = data.size();
}

void sha256::update(std::string_view text) noexcept {
  // A byte view of the characters: char and std::uint8_t share size and alignment.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): char to byte view, no copy
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(text.data());
  update(byte_span{bytes, text.size()});
}

sha256::value_type sha256::finish() const noexcept {
  sha256 copy = *this;
  const std::uint64_t bit_length = total_ * 8U;
  // Padding (5.1.1): 0x80, zeros up to 56 mod 64, then the 64-bit big-endian bit length.
  std::array<std::uint8_t, block_size + 8> pad{};
  pad[0] = 0x80U;
  const std::size_t zeros = (buffered_ < 56 ? 56 - buffered_ : (block_size + 56) - buffered_);
  std::array<std::uint8_t, 8> length{};
  for (std::size_t i = 0; i < 8; ++i) {
    length[i] = static_cast<std::uint8_t>(bit_length >> (8U * (7 - i)));
  }
  copy.update(byte_span{pad}.first(zeros));
  copy.update(byte_span{length});
  value_type out{};
  for (std::size_t i = 0; i < 8; ++i) {
    out[4 * i] = static_cast<std::uint8_t>(copy.state_[i] >> 24U);
    out[(4 * i) + 1] = static_cast<std::uint8_t>(copy.state_[i] >> 16U);
    out[(4 * i) + 2] = static_cast<std::uint8_t>(copy.state_[i] >> 8U);
    out[(4 * i) + 3] = static_cast<std::uint8_t>(copy.state_[i]);
  }
  return out;
}
// NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)

sha256::value_type sha256::hash(byte_span data) noexcept {
  sha256 h;
  h.update(data);
  return h.finish();
}

sha256::value_type sha256::hash(std::string_view text) noexcept {
  sha256 h;
  h.update(text);
  return h.finish();
}

} // namespace sl::fabric
