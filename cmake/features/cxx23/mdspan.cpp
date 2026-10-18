// std::mdspan with the multidimensional subscript.
#include <mdspan>
#include <version>

#if !defined(__cpp_lib_mdspan) || __cpp_lib_mdspan < 202207L
#error "__cpp_lib_mdspan is missing or too old"
#endif

constexpr int corner() {
  int data[6] = {0, 1, 2, 3, 4, 5};
  std::mdspan m(data, 2, 3);
  return m[1, 2];
}

static_assert(corner() == 5);

int main() { return 0; }
