// The OS clocks of the headless platform (WO-078, SL-ARCH-003 ARCH3-R035, R079). One of the
// src/platform_*.cpp files allowed to call a clock's now() (runtime-lint R-RT-PLATFORM).

#include "sl/runtime/headless_platform.hpp"
#include "sl/runtime/platform.hpp"

#include <chrono>

namespace sl::runtime {

nanos steady_clock_source::now() const noexcept {
  return std::chrono::duration_cast<nanos>(std::chrono::steady_clock::now().time_since_epoch());
}

std::chrono::system_clock::time_point system_wall_clock::now() const noexcept {
  return std::chrono::system_clock::now();
}

} // namespace sl::runtime
