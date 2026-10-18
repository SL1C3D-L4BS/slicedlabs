// std::print and std::println (links against the library implementation).
#include <print>
#include <version>

#if !defined(__cpp_lib_print) || __cpp_lib_print < 202207L
#error "__cpp_lib_print is missing or too old"
#endif

void sl_probe_print() {
  std::print("{} {}\n", 1, "two");
  std::println("{}", 3.5);
}

int main() { return 0; }
