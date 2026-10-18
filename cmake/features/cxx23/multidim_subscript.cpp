// P2128 multidimensional subscript operator.
#ifndef __cpp_multidimensional_subscript
#error "__cpp_multidimensional_subscript is not defined"
#endif

struct Grid {
  int cell[4] = {};
  constexpr int &operator[](int row, int col) { return cell[row * 2 + col]; }
};

constexpr bool roundtrip() {
  Grid g;
  g[1, 1] = 5;
  return g[1, 1] == 5 && g[0, 0] == 0;
}

static_assert(roundtrip());

int main() { return 0; }
