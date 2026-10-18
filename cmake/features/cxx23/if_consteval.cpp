// P1938 if consteval.
#ifndef __cpp_if_consteval
#error "__cpp_if_consteval is not defined"
#endif

constexpr int pick(int x) {
  if consteval {
    return x + 1;
  } else {
    return x;
  }
}

static_assert(pick(1) == 2);

int main() { return pick(1) == 1 ? 0 : 1; }
