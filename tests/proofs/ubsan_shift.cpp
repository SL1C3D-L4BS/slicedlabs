// Planted defect for `just sanitize-proof` (WO-025): a shift exponent as wide as the type. Built
// only with -DSL_PROOFS=ON; under the ubsan preset (-fno-sanitize-recover=all) it must report
// "shift exponent" and exit non-zero.
#include <cstdio>
#include <cstdlib>

int main(int argc, char ** /*argv*/) {
  const int exponent = 31 + argc; // 32 for a plain run: undefined for a 32-bit int
  const int value = 1 << exponent;
  std::printf("shifted %d\n", value);
  return EXIT_SUCCESS;
}
