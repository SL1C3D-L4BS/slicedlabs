// std::byteswap.
#include <bit>
#include <cstdint>
#include <version>

#if !defined(__cpp_lib_byteswap) || __cpp_lib_byteswap < 202110L
#error "__cpp_lib_byteswap is missing or too old"
#endif

static_assert(std::byteswap(std::uint16_t{0x1234}) == 0x3412);
static_assert(std::byteswap(std::uint32_t{0x01020304}) == 0x04030201U);

int main() { return 0; }
