// P0847 explicit object parameter ("deducing this").
#ifndef __cpp_explicit_this_parameter
#error "__cpp_explicit_this_parameter is not defined"
#endif

struct Counter {
  int value = 3;
  constexpr int get(this const Counter &self) { return self.value; }
  template <class Self> constexpr auto &&field(this Self &&self) { return (self.value); }
};

static_assert(Counter{}.get() == 3);
static_assert(Counter{}.field() == 3);

int main() { return 0; }
