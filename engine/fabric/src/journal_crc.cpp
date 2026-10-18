// CRC-32C (Castagnoli) for the commit log (WO-053, SL-ARCH-002 `journal` crc, ARCH2-R29):
// reflected polynomial 0x82F63B78, init 0xFFFFFFFF, final xor 0xFFFFFFFF, one 256-entry table
// built at compile time.

#include <array>
#include <cstddef>
#include <cstdint>

#include "sl/fabric/bytes.hpp"
#include "sl/fabric/journal.hpp"

namespace sl::fabric {

namespace {

constexpr std::uint32_t castagnoli_reflected = 0x82F63B78U;

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index): indices are masked to 0..255
constexpr std::array<std::uint32_t, 256> make_table() noexcept {
  std::array<std::uint32_t, 256> t{};
  for (std::uint32_t i = 0; i < 256U; ++i) {
    std::uint32_t c = i;
    for (int bit = 0; bit < 8; ++bit) {
      c = (c & 1U) != 0U ? (c >> 1U) ^ castagnoli_reflected : c >> 1U;
    }
    t[i] = c;
  }
  return t;
}

constexpr std::array<std::uint32_t, 256> crc_table = make_table();

static_assert(crc_table[1] == 0xF26B8303U, "CRC-32C table entry 1");
static_assert(crc_table[128] == 0x82F63B78U, "CRC-32C table entry 128");

} // namespace

std::uint32_t crc32c_extend(std::uint32_t crc, byte_span data) noexcept {
  std::uint32_t c = ~crc;
  for (const std::uint8_t b : data) {
    c = crc_table[(c ^ b) & 0xFFU] ^ (c >> 8U);
  }
  return ~c;
}
// NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)

} // namespace sl::fabric
