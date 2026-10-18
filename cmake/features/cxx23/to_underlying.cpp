// std::to_underlying.
#include <cstdint>
#include <utility>
#include <version>

#if !defined(__cpp_lib_to_underlying) || __cpp_lib_to_underlying < 202102L
#error "__cpp_lib_to_underlying is missing or too old"
#endif

enum class Color : std::uint8_t { Red = 3, Blue = 7 };

static_assert(std::to_underlying(Color::Blue) == 7);

int main() { return 0; }
