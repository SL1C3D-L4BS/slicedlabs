// std::views::enumerate.
#include <array>
#include <ranges>
#include <version>

#if !defined(__cpp_lib_ranges_enumerate) || __cpp_lib_ranges_enumerate < 202302L
#error "__cpp_lib_ranges_enumerate is missing or too old"
#endif

constexpr int weighted() {
  std::array a{10, 20, 30};
  int sum = 0;
  for (auto [i, v] : std::views::enumerate(a)) {
    sum += static_cast<int>(i) * v;
  }
  return sum;
}

static_assert(weighted() == 80);

int main() { return 0; }
