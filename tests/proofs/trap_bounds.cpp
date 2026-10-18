// Hardened build only (SL_HARDEN=ON): an out-of-bounds array access must trap (SIGILL or SIGTRAP),
// an in-bounds one must not. The ctest wrapper ExpectSignal.cmake runs each mode; a hardening flag
// that was silently dropped makes an `-out` mode run to completion and the test goes red instead of
// the build staying green.
//
//   array-in / array-out     a local array through a volatile index
//   member-in / member-out   an array member of a heap struct, with a neighbour member behind it
//   (an overrun would
//                            land in `after` without a trap)
// Both rely on -fsanitize=bounds -fsanitize-trap=bounds. A trailing one-element array (the "struct
// hack") is left out: gcc does not instrument it even with -fstrict-flex-arrays=3, so the two
// compilers would disagree.
#include <cstddef>
#include <cstdlib>
#include <new>
#include <string_view>

namespace {
struct Holder {
  int slots[4];
  int after;
};

int through_array(std::size_t index) {
  int numbers[4] = {1, 2, 3, 4};
  const volatile std::size_t k = index;
  return numbers[k];
}

int through_member(std::size_t index) {
  auto *const holder = new Holder{{1, 2, 3, 4}, 99};
  const volatile std::size_t k = index;
  const int value = holder->slots[k];
  delete holder;
  return value;
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    return EXIT_FAILURE;
  }
  const std::string_view mode{argv[1]};
  if (mode == "array-in") {
    return through_array(3) == 4 ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  if (mode == "array-out") {
    return through_array(4);
  }
  if (mode == "member-in") {
    return through_member(3) == 4 ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  if (mode == "member-out") {
    return through_member(4);
  }
  return EXIT_FAILURE;
}
