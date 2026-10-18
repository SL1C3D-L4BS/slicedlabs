#include "sl/fabric/clock.hpp"

#include <chrono>

namespace sl::fabric {

std::int64_t system_clock_source::now_us() const noexcept {
  const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::microseconds>(since_epoch).count();
}

} // namespace sl::fabric
