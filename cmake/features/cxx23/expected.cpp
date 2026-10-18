// std::expected.
#include <expected>
#include <version>

#if !defined(__cpp_lib_expected) || __cpp_lib_expected < 202202L
#error "__cpp_lib_expected is missing or too old"
#endif

constexpr std::expected<int, int> half(int x) {
  if (x % 2 != 0) {
    return std::unexpected(x);
  }
  return x / 2;
}

static_assert(half(4).value() == 2);
static_assert(!half(3).has_value());
static_assert(half(4).transform([](int v) { return v + 1; }).value() == 3);

int main() { return 0; }
