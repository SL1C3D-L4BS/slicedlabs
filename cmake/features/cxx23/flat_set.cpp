// std::flat_set.
#include <flat_set>
#include <version>

#if !defined(__cpp_lib_flat_set) || __cpp_lib_flat_set < 202207L
#error "__cpp_lib_flat_set is missing or too old"
#endif

int sl_probe_flat_set() {
  std::flat_set<int> s{3, 1, 2};
  return *s.begin();
}

int main() { return 0; }
