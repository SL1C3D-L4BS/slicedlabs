// sl-link: stdc++exp
// std::stacktrace (C++23 library feature; optional here because libstdc++ needs -lstdc++exp).
#include <stacktrace>
#include <version>

#if !defined(__cpp_lib_stacktrace) || __cpp_lib_stacktrace < 202011L
#error "__cpp_lib_stacktrace is missing or too old"
#endif

unsigned long sl_probe_stacktrace() { return std::stacktrace::current().size(); }

int main() { return 0; }
