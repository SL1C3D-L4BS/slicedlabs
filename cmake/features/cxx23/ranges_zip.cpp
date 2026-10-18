// std::views::zip.
#include <array>
#include <ranges>
#include <version>

#if !defined(__cpp_lib_ranges_zip) || __cpp_lib_ranges_zip < 202110L
#error "__cpp_lib_ranges_zip is missing or too old"
#endif

constexpr int dot() {
  std::array a{1, 2, 3};
  std::array b{4, 5, 6};
  int sum = 0;
  for (auto [x, y] : std::views::zip(a, b)) {
    sum += x * y;
  }
  return sum;
}

static_assert(dot() == 32);

int main() { return 0; }
