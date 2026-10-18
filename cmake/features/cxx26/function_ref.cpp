// std::function_ref.
#include <functional>
#include <version>

#if !defined(__cpp_lib_function_ref) || __cpp_lib_function_ref < 202306L
#error "__cpp_lib_function_ref is missing or too old"
#endif

int sl_twice(int x) { return x * 2; }

int sl_probe_function_ref() {
  std::function_ref<int(int)> f = sl_twice;
  return f(4);
}

int main() { return 0; }
