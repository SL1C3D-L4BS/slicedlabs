// scheduler_stats (WO-086, SL-ARCH-003 ARCH3-R076): a snapshot of a scheduler_policy's per-slot
// counters read through read_counters (relaxed, any thread) and their field-wise sum. Diagnostic
// only: the values are never asserted as times.

#include "sl/runtime/scheduler_stats.hpp"
#include "sl/runtime/work_queue.hpp"

#include <cstdint>
#include <vector>

namespace sl::runtime::jobs {

worker_counters scheduler_stats::total() const noexcept {
  worker_counters sum;
  for (const worker_counters &w : workers) {
    sum.jobs_run += w.jobs_run;
    sum.steals += w.steals;
    sum.failed_steals += w.failed_steals;
    sum.parks += w.parks;
    sum.wakes += w.wakes;
    sum.injection_polls += w.injection_polls;
    sum.aging_promotions += w.aging_promotions;
  }
  return sum;
}

scheduler_stats collect(const scheduler_policy &policy, std::uint32_t workers) {
  scheduler_stats stats;
  stats.workers.resize(workers);
  policy.read_counters(stats.workers);
  return stats;
}

} // namespace sl::runtime::jobs
