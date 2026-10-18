// std::string::resize_and_overwrite.
#include <cstddef>
#include <string>
#include <version>

#if !defined(__cpp_lib_string_resize_and_overwrite) ||                                             \
    __cpp_lib_string_resize_and_overwrite < 202110L
#error "__cpp_lib_string_resize_and_overwrite is missing or too old"
#endif

std::string sl_probe_resize_and_overwrite() {
  std::string s;
  s.resize_and_overwrite(4, [](char *buf, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
      buf[i] = 'a';
    }
    return n;
  });
  return s;
}

int main() { return 0; }
