// std::views::concat (C++26).
#include <array>
#include <ranges>
#include <version>

#if !defined(__cpp_lib_ranges_concat) || __cpp_lib_ranges_concat < 202403L
#error "__cpp_lib_ranges_concat is missing or too old"
#endif

int main() { return 0; }
