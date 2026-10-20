// The Linux headless platform (WO-078, SL-ARCH-003 ARCH3-R036, ADR-0006): no display, window or
// input device; files over the POSIX vfs under root_dir.

#include "sl/core/error.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/headless_platform.hpp"
#include "sl/runtime/platform.hpp"

#include <memory>
#include <string>
#include <utility>

namespace sl::runtime {

headless_platform::headless_platform(const platform_options &options, file_access files) noexcept
    : threads_(options.instance_id), files_(std::move(files)),
      platform_{.clock = clock_,
                .wall = wall_,
                .threads = threads_,
                .input = input_,
                .files = files_,
                .win = win_,
                .sink = sink_,
                .instance_id = options.instance_id} {}

result<std::unique_ptr<headless_platform>>
make_headless_platform(const platform_options &options) noexcept {
  auto files = file_access::make_posix(options.root_dir);
  if (!files) {
    return make_error(errc::platform_unavailable, "make_headless_platform: root_dir " +
                                                      options.root_dir + ": " +
                                                      files.error().message);
  }
  return std::make_unique<headless_platform>(options, std::move(*files));
}

} // namespace sl::runtime
