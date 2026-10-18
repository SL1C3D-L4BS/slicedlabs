#pragma once

#include <string_view>

namespace sl::core {

/// Build provenance baked in at configure time (cmake/SlBuildInfo.cmake). All views are static
/// storage.
struct BuildInfo {
  std::string_view compiler;
  std::string_view compiler_version;
  std::string_view cxx_standard;
  std::string_view build_type;
  std::string_view sanitizer;
  std::string_view preset;
  std::string_view linker;
  std::string_view revision;
};

[[nodiscard]] BuildInfo build_info() noexcept;

} // namespace sl::core
