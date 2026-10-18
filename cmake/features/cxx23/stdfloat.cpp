// <stdfloat> fixed-width floating-point types (optional: libstdc++ and clang differ).
#include <stdfloat>
#include <version>

#if !defined(__cpp_lib_stdfloat) || __cpp_lib_stdfloat < 202207L
#error "__cpp_lib_stdfloat is missing or too old"
#endif

#ifndef __STDCPP_FLOAT32_T__
#error "std::float32_t is not provided"
#endif

static_assert(sizeof(std::float32_t) == 4);

int main() { return 0; }
