#include "sl/core/version.hpp"

#include "sl/build_info.hpp"

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

} // namespace sl::core
