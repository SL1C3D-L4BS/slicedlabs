#pragma once

// testing::stress harness (WO-098, SL-ARCH-003 ARCH3-R162 to R165, `stress_matrix`, `limits`
// stress_file_cap and SL_RUNTIME_STRESS_SCALE_default; episode 099). Used only by the stress_*
// integration tests (tests/integration/runtime/stress_*_test.cpp):
//   stress_scale()  SL_RUNTIME_STRESS_SCALE (default 1.0; ctest sets 0.1 under the sanitizers),
//                   clamped to [0.01, 10]; scaled(n) is n times it, at least 1;
//   stress_seed(f)  SL_RUNTIME_SOAK_SEED when set (runtime-soak), else the cell's fixed seed f;
//   SL_STRESS_SEED  a seed_guard plus a doctest INFO, so a failing check prints
//                   "seed <s>; rerun: SL_RUNTIME_SOAK_SEED=<s> ctest --preset debug -R <test>";
//   cap_timer       the file cap (20 s Debug at scale 1; scaled runs under the sanitizers keep the
//                   20 s cap) checked with SL_STRESS_CAP;
//   run_watched     runs a body on a thread from the platform thread_factory and aborts the test
//                   process after 30 s without completion (doctest prints the INFO context, so
//                   the seed line, on the abort). Its spawn visits RFP1 (thread.spawn).
// Stress tests assert invariants, never timings: the cap and the watchdog are the only limits
// (ARCH3-R165). Owner-thread (`thread_safety`, testing::*). Include it by path:
// "../../../engine/runtime/testing/stress_harness.hpp".

#include "sl/core/error.hpp"
#include "sl/runtime/headless_platform.hpp"
#include "sl/runtime/platform.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace sl::runtime::testing {

/// `limits` stress_file_cap: seconds per stress file, Debug at scale 1.
inline constexpr double stress_file_cap_s = 20.0;
/// The watchdog of run_watched.
inline constexpr std::chrono::milliseconds stress_watchdog_limit{30'000};

/// SL_RUNTIME_STRESS_SCALE, default 1.0, clamped to [0.01, 10]; unparsable text is 1.0.
[[nodiscard]] double stress_scale() noexcept;
/// n * stress_scale(), at least 1.
[[nodiscard]] std::uint64_t scaled(std::uint64_t n) noexcept;
/// SL_RUNTIME_SOAK_SEED when it is set to a decimal number, else `fixed`.
[[nodiscard]] std::uint64_t stress_seed(std::uint64_t fixed) noexcept;
/// True when SL_RUNTIME_SOAK_SEED is set to a decimal number (a soak run).
[[nodiscard]] bool soak_seed_set() noexcept;

/// The seed of one stress run and its rerun line. Use SL_STRESS_SEED, which also puts the line
/// into doctest's INFO context so it prints with every failed check of the scope.
class seed_guard {
public:
  /// `test` is the ctest name (runtime_stress_<file>_test).
  seed_guard(std::string_view test, std::uint64_t seed);
  [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }
  /// "seed <s>; rerun: SL_RUNTIME_SOAK_SEED=<s> ctest --preset debug -R <test>".
  [[nodiscard]] const std::string &line() const noexcept { return line_; }

private:
  std::uint64_t seed_;
  std::string line_;
};

/// Measures one stress file (or case) against the file cap on the steady clock.
class cap_timer {
public:
  explicit cap_timer(double cap_s = stress_file_cap_s) noexcept;
  [[nodiscard]] double elapsed_s() const noexcept;
  [[nodiscard]] double cap_s() const noexcept { return cap_s_; }
  [[nodiscard]] bool within_cap() const noexcept { return elapsed_s() <= cap_s_; }

private:
  steady_clock_source clock_;
  nanos start_;
  double cap_s_;
};

/// Runs `body` on a thread named slrt<instance>-stress and waits for it. When it has not
/// completed within `limit`, writes "stress watchdog: no completion within <ms> ms" to stderr and
/// aborts. The thread_factory error (RFP1 or the OS) is returned without running the body.
[[nodiscard]] result<void> run_watched(const std::function<void()> &body,
                                       std::chrono::milliseconds limit = stress_watchdog_limit);

} // namespace sl::runtime::testing

/// A seed_guard named `var` for `test` and `seed_value`, with its rerun line in doctest's INFO
/// context for the rest of the scope (a doctest test file only).
#define SL_STRESS_SEED(var, test, seed_value)                                                      \
  const ::sl::runtime::testing::seed_guard var((test), (seed_value));                              \
  INFO((var).line())

/// Checks a cap_timer against its cap, printing the elapsed seconds (a doctest test file only).
#define SL_STRESS_CAP(timer)                                                                       \
  do {                                                                                             \
    INFO("elapsed " << (timer).elapsed_s() << " s, cap " << (timer).cap_s() << " s");              \
    CHECK((timer).within_cap());                                                                   \
  } while (false)
