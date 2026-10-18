// std::ranges::to.
#include <ranges>
#include <vector>
#include <version>

#if !defined(__cpp_lib_ranges_to_container) || __cpp_lib_ranges_to_container < 202202L
#error "__cpp_lib_ranges_to_container is missing or too old"
#endif

unsigned long sl_probe_ranges_to() {
  auto v = std::views::iota(0, 4) | std::ranges::to<std::vector>();
  return v.size();
}

int main() { return 0; }
