// std::unreachable.
#include <utility>
#include <version>

#if !defined(__cpp_lib_unreachable) || __cpp_lib_unreachable < 202202L
#error "__cpp_lib_unreachable is missing or too old"
#endif

int sl_probe_unreachable(int x) {
  switch (x) {
  case 0:
    return 1;
  case 1:
    return 2;
  default:
    std::unreachable();
  }
}

int main() { return 0; }
