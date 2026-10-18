// basic_string and basic_string_view contains.
#include <string>
#include <string_view>
#include <version>

#if !defined(__cpp_lib_string_contains) || __cpp_lib_string_contains < 202011L
#error "__cpp_lib_string_contains is missing or too old"
#endif

static_assert(std::string_view("sliced labs").contains("labs"));
static_assert(!std::string_view("sliced").contains('x'));

int main() { return 0; }
