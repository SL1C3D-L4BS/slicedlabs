// frame_stats (WO-089, SL-ARCH-003 ARCH3-R100, R051, `limits` warmup_frames): per-phase job
// counts, critical paths and wall times, the clocks' drop and non-monotonic totals, and the
// frame's upstream allocation delta: blocks the frame arena took from its upstream plus every
// growth of the scheduler's own containers during the frame. Wall time is recorded, never
// asserted; after warm-up a frame with upstream allocations logs one runtime.frame warning.

#include "sl/core/log.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/time.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace sl::runtime {

std::uint64_t frame_scheduler::upstream_count() const noexcept {
  return arena_->upstream_allocations() + internal_allocs_;
}

void frame_scheduler::record_phase(frame_stats &stats, phase ph, nanos start) const noexcept {
  const auto p = static_cast<std::size_t>(ph);
  frame_stats::per_phase &out = std::span(stats.phases)[p];
  const phase_graph &g = graphs_[p];
  out.jobs += static_cast<std::uint32_t>(g.tasks.size());
  out.critical_path = g.critical_path;
  const nanos end = platform_->clock.now();
  if (end > start) {
    out.wall += end - start;
  }
}

void frame_scheduler::close_stats(frame_stats &stats, std::uint64_t upstream_before) noexcept {
  stats.frame_index = frame_index_;
  stats.dropped_ticks = stepper_.dropped_ticks();
  stats.dropped_ns = frame_clock_.dropped_ns();
  stats.non_monotonic = frame_clock_.non_monotonic();
  const std::uint64_t upstream_after = upstream_count();
  stats.upstream_allocs = upstream_after - upstream_before;
  last_upstream_ = stats.upstream_allocs;
  if (frame_index_ > warmup_frames_ && stats.upstream_allocs != 0) {
    sl::log::emit(sl::log::level::warn, "runtime.frame", "upstream allocation in a steady frame",
                  {{"frame", frame_index_}, {"upstream_allocs", stats.upstream_allocs}});
  }
}

} // namespace sl::runtime
