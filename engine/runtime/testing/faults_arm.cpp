// testing::arm, disarm, disarm_all and scoped_arm (WO-076, SL-ARCH-003 ARCH3-R020): the only way
// to arm a failpoint. testing::fault_arming is the friend of detail::fault_registry_hooks
// (sl/runtime/faults.hpp) and is defined only here, in sl_runtime_testing.

#include "faults_arm.hpp"

#include "sl/core/error.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/faults.hpp"

#include <cstddef>
#include <cstdint>

namespace sl::runtime::testing {

class fault_arming {
public:
  static void arm(failpoint point, std::uint64_t at_visit, bool sticky,
                  std::uint32_t param) noexcept {
    detail::fault_registry_hooks::arm(point, at_visit, sticky, param);
  }
  static void disarm(failpoint point) noexcept { detail::fault_registry_hooks::disarm(point); }
};

result<void> arm(failpoint point, std::uint64_t at_visit, bool sticky, std::uint32_t param) {
  if (!faults_compiled) {
    return make_error(errc::unsupported, "failpoints are compiled out (SL_RUNTIME_FAULTS=0)");
  }
  if (static_cast<std::size_t>(point) >= failpoint_count) {
    return make_error(errc::invalid_argument, "no such failpoint");
  }
  if (at_visit == 0) {
    return make_error(errc::invalid_argument, "at_visit counts visits from 1");
  }
  fault_arming::arm(point, at_visit, sticky, param);
  return {};
}

void disarm(failpoint point) noexcept { fault_arming::disarm(point); }

void disarm_all() noexcept {
  for (std::size_t i = 0; i < failpoint_count; ++i) {
    fault_arming::disarm(static_cast<failpoint>(i));
  }
}

scoped_arm::scoped_arm(failpoint point, std::uint64_t at_visit, bool sticky, std::uint32_t param)
    : point_(point), status_(arm(point, at_visit, sticky, param)) {}

scoped_arm::~scoped_arm() {
  if (status_) {
    disarm(point_);
  }
}

} // namespace sl::runtime::testing
