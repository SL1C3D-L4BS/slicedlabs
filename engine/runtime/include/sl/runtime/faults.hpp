#pragma once

// Failpoints RFP0-RFP10 (WO-076, SL-ARCH-003 ARCH3-R008/R020 and `failpoints`, ADR-0022, ADR-0015
// pattern). A failpoint is a visit point inside the runtime: the site calls visit(point) and, when
// the returned action fires, takes the outcome of the `failpoints` table. Compiled when
// SL_RUNTIME_FAULTS=1 (Debug and sanitizer builds); in Release visit() is a constexpr no-op that
// never fires. Arming is test-only: sl::runtime::testing (engine/runtime/testing/faults_arm.hpp,
// in sl_runtime_testing). The registry behind visit() lives in src/faults.cpp, the one mutable
// global of the runtime (ARCH3-R008). Layer L0. visit, visits and failpoint_table are any-thread
// and lock-free.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#ifndef SL_RUNTIME_FAULTS
#define SL_RUNTIME_FAULTS                                                                          \
  0 // the sl_runtime target always defines it; 0 keeps a stray TU compiled out
#endif

namespace sl::runtime {

/// The visit points of SL-ARCH-003 `failpoints`; the number is the RFP index.
enum class failpoint : std::uint8_t {
  rfp0,  // alloc.fail
  rfp1,  // thread.spawn
  rfp2,  // job.error
  rfp3,  // job.fatal
  rfp4,  // worker.stall
  rfp5,  // system.fail
  rfp6,  // clock.jump
  rfp7,  // event.overflow
  rfp8,  // snapshot.io
  rfp9,  // suspend.request
  rfp10, // entity.exhaust
};

inline constexpr std::size_t failpoint_count = 11;

/// The `table_version` of SL-ARCH-003 `failpoints` (an input of runtime_abi_digest, ARCH3-R032).
inline constexpr std::uint32_t failpoint_table_version = 1;

/// One row of SL-ARCH-003 `failpoints`: "RFP0", "alloc.fail", the site text.
struct failpoint_info {
  std::string_view id;
  std::string_view name;
  std::string_view site;
};

/// The table, in RFP order; the text equals SL-ARCH-003 `failpoints`.
[[nodiscard]] std::span<const failpoint_info> failpoint_table() noexcept;

/// True when the failpoints are compiled in (SL_RUNTIME_FAULTS=1).
inline constexpr bool faults_compiled = SL_RUNTIME_FAULTS != 0;

/// What a visit tells its site: fire the table outcome or proceed; `param` is the armed parameter
/// (the stall in microseconds for RFP4, the jump for RFP6, ...), 0 when not armed.
struct fault_action {
  bool fire = false;
  std::uint32_t param = 0;
};

#if SL_RUNTIME_FAULTS
/// Counts one visit of `point` and returns whether it fires: an armed point fires exactly at the
/// at_visit-th visit after arming, and at every later visit when armed sticky; an unarmed point
/// returns {false, 0}.
[[nodiscard]] fault_action visit(failpoint point) noexcept;
/// The number of visits of `point` since process start.
[[nodiscard]] std::uint64_t visits(failpoint point) noexcept;
#else
[[nodiscard]] constexpr fault_action visit(failpoint /*point*/) noexcept { return {}; }
[[nodiscard]] constexpr std::uint64_t visits(failpoint /*point*/) noexcept { return 0; }
#endif

namespace testing {
class fault_arming;
} // namespace testing

namespace detail {
/// The arming side of the registry in src/faults.cpp. Private: only testing::fault_arming (defined
/// in sl_runtime_testing, engine/runtime/testing/faults_arm.cpp) reaches it, so code that does not
/// link the test library cannot arm (ARCH3-R020).
class fault_registry_hooks {
  friend class testing::fault_arming;
  // Arms `point` to fire at the at_visit-th visit after now (at_visit >= 1).
  static void arm(failpoint point, std::uint64_t at_visit, bool sticky,
                  std::uint32_t param) noexcept;
  static void disarm(failpoint point) noexcept;
};
} // namespace detail

} // namespace sl::runtime
