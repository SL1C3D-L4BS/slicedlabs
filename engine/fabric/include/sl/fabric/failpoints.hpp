#pragma once

// Commit failpoints FP0-FP6 (WO-050, SL-ARCH-002 ARCH2-R19 and `failpoints`, ADR-0015).
//
// A failpoint is a named place in the commit protocol where a test can make the commit stop as if
// the process had crashed there. Production code never arms one: arming goes through
// testing::tx_failpoints, which is declared here and defined only in the test library
// sl_fabric_testing (engine/fabric/testing/tx_failpoints.cpp), exactly like testing::backdoor
// (store.hpp). Unarmed, a visit only counts.
//
// Where each one sits and what a crash there leaves (SL-ARCH-002 `failpoints`):
//   FP0  before validate               not committed
//   FP1  after validate                not committed
//   FP2  after materialise             not committed; objects in memory only
//   FP3  mid commit frame write        not committed; torn tail truncated on recovery
//   FP4  after write, before fsync     either, decided by frame intactness
//   FP5  after fsync, before ack       committed; retry returns the original receipt
//   FP6  after ack, before events      committed; recovery emits tx recovered
// The transaction manager visits FP0, FP1, FP2, FP5 and FP6. FP3 and FP4 lie inside
// store_backend::commit (the log backend's append and sync, WO-053): tests reach them through the
// store's own fault seams (memory_backend_faults, the faulty vfs), so tx_failpoints refuses to
// arm them on the manager. A tripped FP0-FP2 fails the commit with the armed error and writes
// nothing; a tripped FP5 or FP6 is after the commit point, so the commit is `commit_indeterminate`
// and the manager refuses later transactions with `recovery_required` until it is reopened
// (ARCH2-R38).

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "sl/core/error.hpp"

namespace sl::fabric {

/// The seven failpoints of SL-ARCH-002 `failpoints`; the number is the FP index.
enum class failpoint : std::uint8_t {
  fp0 = 0,
  fp1 = 1,
  fp2 = 2,
  fp3 = 3,
  fp4 = 4,
  fp5 = 5,
  fp6 = 6
};

inline constexpr std::size_t failpoint_count = 7;

/// One row of SL-ARCH-002 `failpoints`.
struct failpoint_info {
  failpoint fp = failpoint::fp0;
  std::string_view name;    // "FP0"
  std::string_view at;      // "before validate"
  std::string_view outcome; // "not committed"
  bool committed = false;   // true when a crash there leaves the commit durable (FP5, FP6)
  bool manager_visits = false;
};

/// The table, in FP order; the text equals SL-ARCH-002 `failpoints`.
[[nodiscard]] std::span<const failpoint_info> failpoint_table() noexcept;
/// "FP0" .. "FP6"; "FP?" otherwise.
[[nodiscard]] std::string_view to_string(failpoint fp) noexcept;

class failpoint_set;

namespace testing {
/// Test-only arming of failpoints (ARCH2-R19). Declared here and defined only in sl_fabric_testing
/// (engine/fabric/testing/tx_failpoints.cpp): code that does not link the test library cannot arm.
class tx_failpoints {
public:
  tx_failpoints() = delete;

  /// Arms `fp` once: the next visit fails with `code` and disarms it. `invalid_argument` for FP3
  /// and FP4 (inside the store, see the file comment) or for `code` ok.
  [[nodiscard]] static result<void> arm(failpoint_set &set, failpoint fp,
                                        errc code = errc::store_io) noexcept;
  static void disarm(failpoint_set &set, failpoint fp) noexcept;
  static void disarm_all(failpoint_set &set) noexcept;
};
} // namespace testing

/// The failpoints of one transaction manager. Thread-safe: visits and arming are atomic.
class failpoint_set {
public:
  failpoint_set() noexcept = default;
  failpoint_set(const failpoint_set &) = delete;
  failpoint_set(failpoint_set &&) = delete;
  failpoint_set &operator=(const failpoint_set &) = delete;
  failpoint_set &operator=(failpoint_set &&) = delete;
  ~failpoint_set() = default;

  /// Called by the transaction manager at `fp`: counts the visit and, when `fp` is armed, disarms
  /// it and returns the armed error (the message names the failpoint).
  [[nodiscard]] result<void> visit(failpoint fp) noexcept;
  /// The number of visits of `fp` so far.
  [[nodiscard]] std::uint64_t visits(failpoint fp) const noexcept;
  /// The number of visits that tripped an armed `fp`.
  [[nodiscard]] std::uint64_t trips(failpoint fp) const noexcept;
  [[nodiscard]] bool armed(failpoint fp) const noexcept;

private:
  friend class testing::tx_failpoints;

  std::array<std::atomic<std::uint32_t>, failpoint_count> armed_{}; // errc value, 0 = disarmed
  std::array<std::atomic<std::uint64_t>, failpoint_count> visits_{};
  std::array<std::atomic<std::uint64_t>, failpoint_count> trips_{};
};

} // namespace sl::fabric
