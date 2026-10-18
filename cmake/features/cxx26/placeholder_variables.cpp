// P2169 placeholder variables: several `_` in one scope.
constexpr int sl_probe_placeholder() {
  int _ = 1;
  int _ = 2;
  return 0;
}

static_assert(sl_probe_placeholder() == 0);

int main() { return 0; }
