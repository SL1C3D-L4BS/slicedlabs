#include <cstddef>
#include <print>
#include <span>
#include <string_view>

#include "sl/core/version.hpp"

namespace {

// The fields the build must always provide; --self-check fails when one is empty.
bool build_info_complete(const sl::core::BuildInfo &info) {
  return !info.compiler.empty() && !info.compiler_version.empty() && !info.cxx_standard.empty() &&
         !info.build_type.empty() && !info.sanitizer.empty() && !info.preset.empty() &&
         !info.linker.empty() && !info.revision.empty();
}

} // namespace

int main(int argc, char **argv) {
  const std::span<char *const> args{argv, static_cast<std::size_t>(argc)};
  const sl::core::BuildInfo info = sl::core::build_info();

  if (args.size() > 1) {
    if (args.size() == 2 && std::string_view{args[1]} == "--self-check") {
      return build_info_complete(info) ? 0 : 1;
    }
    std::println(stderr, "usage: sl_sandbox [--self-check]");
    return 2;
  }

  std::println("sl_sandbox");
  std::println("  compiler   {} {}", info.compiler, info.compiler_version);
  std::println("  standard   C++{}", info.cxx_standard);
  std::println("  build type {}", info.build_type);
  std::println("  sanitizer  {}", info.sanitizer);
  std::println("  preset     {}", info.preset);
  std::println("  linker     {}", info.linker);
  std::println("  revision   {}", info.revision);
  return 0;
}
