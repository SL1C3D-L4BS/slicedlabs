// std::simd (<simd>).
#include <simd>
#include <version>

#if !defined(__cpp_lib_simd) || __cpp_lib_simd < 202411L
#error "__cpp_lib_simd is missing or too old"
#endif

int main() { return 0; }
