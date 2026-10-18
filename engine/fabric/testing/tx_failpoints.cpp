// testing::tx_failpoints (WO-050, SL-ARCH-002 ARCH2-R19): the test-only arming of the commit
// failpoints. Declared in sl/fabric/failpoints.hpp and defined only here, in sl_fabric_testing,
// so production code (which links sl_fabric alone) cannot arm a failpoint.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "sl/core/error.hpp"
#include "sl/fabric/failpoints.hpp"

namespace sl::fabric::testing {

result<void> tx_failpoints::arm(failpoint_set &set, failpoint fp, errc code) noexcept {
  const auto i = static_cast<std::size_t>(fp);
  if (i >= failpoint_count) {
    return make_error(errc::invalid_argument, "no such failpoint");
  }
  if (!failpoint_table()[i].manager_visits) {
    return make_error(errc::invalid_argument,
                      std::string(to_string(fp)) +
                          " lies inside store_backend::commit; use the store's fault seam");
  }
  if (code == errc::ok) {
    return make_error(errc::invalid_argument, "a failpoint needs an error code");
  }
  std::span{set.armed_}[i].store(static_cast<std::uint32_t>(code), std::memory_order_release);
  return {};
}

void tx_failpoints::disarm(failpoint_set &set, failpoint fp) noexcept {
  const auto i = static_cast<std::size_t>(fp);
  if (i < failpoint_count) {
    std::span{set.armed_}[i].store(0, std::memory_order_release);
  }
}

void tx_failpoints::disarm_all(failpoint_set &set) noexcept {
  for (auto &a : set.armed_) {
    a.store(0, std::memory_order_release);
  }
}

} // namespace sl::fabric::testing
