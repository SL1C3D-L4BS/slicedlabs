// P2662 pack indexing.
template <class... Ts> using First = Ts...[0];

template <auto... Vs> constexpr auto last() { return Vs...[sizeof...(Vs) - 1]; }

static_assert(sizeof(First<char, int>) == 1);
static_assert(last<1, 2, 3>() == 3);

int main() { return 0; }
