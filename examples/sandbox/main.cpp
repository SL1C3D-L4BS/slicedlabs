#include <cstddef>
#include <cstdio>
#include <exception>
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

// The banner on stdout, then the same build information as one structured info event on the
// default logger (stderr). std::println throws std::system_error when stdout cannot be written.
int run(std::span<char *const> args) {
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
  // The banner is complete on stdout before the event reaches stderr.
  std::fflush(stdout);
  sl::core::log_build_info();
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  try {
    return run(std::span<char *const>{argv, static_cast<std::size_t>(argc)});
  } catch (const std::exception &e) {
    std::fputs("sl_sandbox: ", stderr);
    std::fputs(e.what(), stderr);
    std::fputs("\n", stderr);
  } catch (...) {
    std::fputs("sl_sandbox: unknown error\n", stderr);
  }
  return 1;
}
