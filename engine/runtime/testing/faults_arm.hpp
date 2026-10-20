#pragma once

// Test-only arming of failpoints RFP0-RFP10 (WO-076, SL-ARCH-003 ARCH3-R020 and `failpoints`,
// ADR-0022). Lives in sl_runtime_testing, so code that does not link the test library cannot arm.
// Owner-thread (`thread_safety`, testing::*): arm on the test thread before the workload starts;
// visits from any number of threads are counted. Include it by path:
// "../../../engine/runtime/testing/faults_arm.hpp".

#include "sl/core/error.hpp"
#include "sl/runtime/faults.hpp"

#include <cstdint>

namespace sl::runtime::testing {

/// Arms `point` to fire at the at_visit-th visit after this call: once, or at that visit and every
/// later one when `sticky`. `param` reaches the site in fault_action::param. Re-arming replaces
/// the previous arming. invalid_argument for at_visit 0 or a value outside rfp0..rfp10;
/// unsupported when the failpoints are compiled out (SL_RUNTIME_FAULTS=0, Release).
[[nodiscard]] result<void> arm(failpoint point, std::uint64_t at_visit, bool sticky = false,
                               std::uint32_t param = 0);

/// Disarms `point`; its visit count keeps running.
void disarm(failpoint point) noexcept;

/// Disarms every failpoint.
void disarm_all() noexcept;

/// Arms one failpoint for a scope and disarms it on destruction.
class scoped_arm {
public:
  scoped_arm(failpoint point, std::uint64_t at_visit, bool sticky = false, std::uint32_t param = 0);
  scoped_arm(const scoped_arm &) = delete;
  scoped_arm(scoped_arm &&) = delete;
  scoped_arm &operator=(const scoped_arm &) = delete;
  scoped_arm &operator=(scoped_arm &&) = delete;
  ~scoped_arm();

  /// The result of the arm() call the constructor made.
  [[nodiscard]] const result<void> &status() const noexcept { return status_; }

private:
  failpoint point_;
  result<void> status_;
};

} // namespace sl::runtime::testing
