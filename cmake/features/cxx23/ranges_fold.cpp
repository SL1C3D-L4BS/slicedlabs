// std::ranges::fold_left.
#include <algorithm>
#include <array>
#include <functional>
#include <version>

#if !defined(__cpp_lib_ranges_fold) || __cpp_lib_ranges_fold < 202207L
#error "__cpp_lib_ranges_fold is missing or too old"
#endif

constexpr int total() {
  std::array a{1, 2, 3, 4};
  return std::ranges::fold_left(a, 0, std::plus<>{});
}

static_assert(total() == 10);

int main() { return 0; }
