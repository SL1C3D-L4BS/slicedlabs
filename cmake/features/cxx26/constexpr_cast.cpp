// P2738 constexpr cast from void*.
constexpr int sl_probe_cast() {
  int x = 42;
  void *p = &x;
  return *static_cast<int *>(p);
}

static_assert(sl_probe_cast() == 42);

int main() { return 0; }
