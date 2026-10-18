// std::flat_map.
#include <flat_map>
#include <version>

#if !defined(__cpp_lib_flat_map) || __cpp_lib_flat_map < 202207L
#error "__cpp_lib_flat_map is missing or too old"
#endif

int sl_probe_flat_map() {
  std::flat_map<int, int> m;
  m[2] = 20;
  m[1] = 10;
  return m.begin()->second;
}

int main() { return 0; }
