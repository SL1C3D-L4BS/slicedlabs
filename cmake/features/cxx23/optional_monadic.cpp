// std::optional and_then / transform / or_else.
#include <optional>
#include <version>

#if !defined(__cpp_lib_optional) || __cpp_lib_optional < 202110L
#error "__cpp_lib_optional (monadic operations) is missing or too old"
#endif

constexpr std::optional<int> safe_half(int x) {
  if (x % 2 != 0) {
    return std::nullopt;
  }
  return x / 2;
}

static_assert(
    std::optional<int>{8}.and_then(safe_half).transform([](int v) { return v + 1; }).value() == 5);
static_assert(!std::optional<int>{3}.and_then(safe_half).has_value());
static_assert(std::optional<int>{}.or_else([] { return std::optional<int>{9}; }).value() == 9);

int main() { return 0; }
