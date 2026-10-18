// std::move_only_function.
#include <functional>
#include <memory>
#include <version>

#if !defined(__cpp_lib_move_only_function) || __cpp_lib_move_only_function < 202110L
#error "__cpp_lib_move_only_function is missing or too old"
#endif

int sl_probe_move_only_function() {
  auto p = std::make_unique<int>(7);
  std::move_only_function<int()> f = [q = std::move(p)] { return *q; };
  return f();
}

int main() { return 0; }
