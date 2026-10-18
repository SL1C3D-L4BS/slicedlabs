#pragma once

#include <string_view>

namespace sl::log {
class logger;
} // namespace sl::log

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

/// Emits build_info() as one info event (system "core", message "build info", one field per member)
/// to the default logger, or to `target` (sl/core/log.hpp, WO-030).
void log_build_info() noexcept;
void log_build_info(sl::log::logger &target) noexcept;

} // namespace sl::core
