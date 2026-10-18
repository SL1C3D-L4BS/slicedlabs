// std::generator (coroutines).
#include <generator>
#include <version>

#if !defined(__cpp_lib_generator) || __cpp_lib_generator < 202207L
#error "__cpp_lib_generator is missing or too old"
#endif

std::generator<int> sl_probe_count(int n) {
  for (int i = 0; i < n; ++i) {
    co_yield i;
  }
}

int sl_probe_generator() {
  int sum = 0;
  for (int v : sl_probe_count(4)) {
    sum += v;
  }
  return sum;
}

int main() { return 0; }
