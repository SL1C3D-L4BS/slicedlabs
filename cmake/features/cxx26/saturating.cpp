// Saturation arithmetic (P0543): the feature-test macro is the probe. The function names differ
// between libraries (libstdc++ 16 ships saturating_add, the paper says add_sat), so no function is
// called here.
#include <numeric>
#include <version>

#if !defined(__cpp_lib_saturation_arithmetic) || __cpp_lib_saturation_arithmetic < 202311L
#error "__cpp_lib_saturation_arithmetic is missing or too old"
#endif

int main() { return 0; }
