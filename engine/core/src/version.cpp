#include "sl/core/version.hpp"

#include "sl/build_info.hpp"
#include "sl/core/log.hpp"

namespace sl::core {

BuildInfo build_info() noexcept {
  return BuildInfo{
      .compiler = generated::kCompiler,
      .compiler_version = generated::kCompilerVersion,
      .cxx_standard = generated::kCxxStandard,
      .build_type = generated::kBuildType,
      .sanitizer = generated::kSanitizer,
      .preset = generated::kPreset,
      .linker = generated::kLinker,
      .revision = generated::kRevision,
  };
}

void log_build_info(sl::log::logger &target) noexcept {
  const BuildInfo info = build_info();
  sl::log::emit(target, sl::log::level::info, "core", "build info",
                {{"compiler", info.compiler},
                 {"compiler_version", info.compiler_version},
                 {"cxx_standard", info.cxx_standard},
                 {"build_type", info.build_type},
                 {"sanitizer", info.sanitizer},
                 {"preset", info.preset},
                 {"linker", info.linker},
                 {"revision", info.revision}});
}

void log_build_info() noexcept { log_build_info(sl::log::default_logger()); }

} // namespace sl::core
