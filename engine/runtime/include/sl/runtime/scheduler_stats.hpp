#pragma once

// The stealing scheduler policy and its counters (WO-086, SL-ARCH-003 ARCH3-R074, R075, R076,
// ADR-0024; episode 088). make_steal_policy builds the scheduler_policy of episode 088: victims
// from a per-thread xorshift64 seeded with splitmix64(seed ^ index), one draw per attempt (the job
// system makes at most 2 x workers attempts per round), the injection queue polled first once
// every injection_poll_every local pops, background work promoted once it waited aging_limit
// rounds, and spin_limit empty rounds before a park on the job system's event_count. Counters
// (steals, failed steals, parks, wakes, injection polls, aging promotions) are relaxed atomics,
// one 64-byte line per thread, summed on read. jobs_run is not reported by the policy (the
// scheduler_policy seam has no run hook; it reads 0). Layer L3: includes work_queue.hpp only.
// Source: src/policy_steal.cpp, src/policy_steal_stats.cpp.
//
// Thread safety (`thread_safety`): collect and scheduler_stats are any-thread (relaxed sums,
// diagnostic, never asserted as times). The policy's slots are workers 0..workers-1 and the job
// system's owner thread at index `workers`; pass workers + 1 to collect to include the owner.

#include "sl/core/error.hpp"
#include "sl/runtime/work_queue.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace sl::runtime::jobs {

/// The knobs of the stealing policy; the defaults are the `limits` table (ARCH3-R174).
struct steal_options {
  std::uint64_t seed = 0;
  std::uint32_t injection_poll_every = 61;
  std::uint32_t spin_limit = 64;
  std::uint32_t aging_limit = 64;
};

/// The stealing policy; invalid_argument when injection_poll_every or aging_limit is 0,
/// out_of_memory when the allocation fails.
[[nodiscard]] result<std::unique_ptr<scheduler_policy>>
make_steal_policy(const steal_options &options) noexcept;

/// A snapshot of a policy's counters: one entry per slot read.
struct scheduler_stats {
  std::vector<worker_counters> workers;

  /// The field-wise sum of every entry.
  [[nodiscard]] worker_counters total() const noexcept;
};

/// Reads the counters of slots [0, workers) of `policy` (any thread; each counter is read on its
/// own, relaxed). `policy` is the one handed to job_config::policy (keep a pointer before the
/// move); slots the policy does not have read zero.
[[nodiscard]] scheduler_stats collect(const scheduler_policy &policy, std::uint32_t workers);

} // namespace sl::runtime::jobs
