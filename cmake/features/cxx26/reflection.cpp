// P2996 static reflection (gcc 16 needs -freflection, which this probe does not pass; clang 23 has
// none).
#include <meta>

constexpr auto sl_type = ^^int;
static_assert(sl_type == ^^int);

int main() { return 0; }
