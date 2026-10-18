// P1169 static operator().
#ifndef __cpp_static_call_operator
#error "__cpp_static_call_operator is not defined"
#endif

struct Doubler {
  static constexpr int operator()(int x) { return x * 2; }
};

static_assert(Doubler{}(21) == 42);
static_assert(Doubler::operator()(2) == 4);

int main() { return 0; }
