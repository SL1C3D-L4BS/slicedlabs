// std::inplace_vector.
#include <inplace_vector>
#include <version>

#if !defined(__cpp_lib_inplace_vector) || __cpp_lib_inplace_vector < 202406L
#error "__cpp_lib_inplace_vector is missing or too old"
#endif

int sl_probe_inplace_vector() {
  std::inplace_vector<int, 4> v;
  v.push_back(1);
  return static_cast<int>(v.size());
}

int main() { return 0; }
