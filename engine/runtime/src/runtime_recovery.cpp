// Snapshots and recovery (WO-096, SL-ARCH-003 ARCH3-R029, R030, R032, R143, ADR-0028): the
// end_frame slot snapshot (every snapshot_every ticks, the newest two kept), the recovery window
// (max_recoveries faults per recovery_window_ticks, in the root block), the restore of the newest
// snapshot at or before the fault tick and the discard of undelivered background results.

#include "sl/runtime/runtime.hpp"

#include "sl/core/error.hpp"
#include "sl/core/log.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/snapshot.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace sl::runtime {

result<runtime::recovery_point> runtime::take_snapshot(std::uint64_t frame, time::tick tick) {
  const std::vector<const det::snapshot_contributor *> contributors = const_contributors();
  const snapshot_meta meta{.frame = frame,
                           .tick = tick,
                           .schedule_digest = schedule_digest_,
                           .runtime_abi_digest = runtime_abi_digest()};
  auto taken = sl::runtime::take(*world_, components_, types_, contributors, meta);
  if (!taken) {
    return std::unexpected(taken.error());
  }
  return recovery_point{.snap = std::move(*taken),
                        .runs = systems_->last_runs().entries(),
                        .runs_floor = systems_->last_runs().floor()};
}

void runtime::on_snapshot(const boundary_context &bc) noexcept {
  const std::uint32_t every = config_.snapshot_every;
  if (every == 0 || frame_faulted_.load() || state_.load() != rt_state::running) {
    return;
  }
  // The first snapshot is the baseline at the end of the first unfaulted frame, after the host
  // filled the world; later ones follow the cadence (ARCH3-R029).
  if (!snapshots_.empty() && bc.tick / every <= last_snapshot_tick_ / every) {
    return;
  }
  auto taken = take_snapshot(bc.frame_index, bc.tick);
  if (!taken) {
    const sl::log::correlation_scope correlation(scope_, bc.tick);
    sl::log::emit(sl::log::level::warn, "runtime.snapshot", "snapshot failed",
                  {{"code", sl::to_string(taken.error().code)}, {"tick", bc.tick}});
    return;
  }
  snapshots_.push_back(std::move(*taken));
  if (snapshots_.size() > 2) {
    snapshots_.erase(snapshots_.begin());
  }
  last_snapshot_tick_ = bc.tick;
}

bool runtime::over_recovery_limit(time::tick tick) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): the root block ring
  fault_ticks_[fault_count_ % fault_ring_] = tick;
  ++fault_count_;
  const std::size_t n = std::min(fault_count_, fault_ring_);
  std::size_t in_window = 0;
  for (std::size_t i = 0; i < n; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): i < fault_ring_
    const time::tick t = fault_ticks_[i];
    if (t <= tick && tick - t <= config_.recovery_window_ticks) {
      ++in_window;
    }
  }
  return in_window > config_.max_recoveries;
}

result<time::tick> runtime::restore_newest(time::tick at_or_before) noexcept {
  for (const recovery_point &p : std::views::reverse(snapshots_)) {
    if (p.snap.tick() <= at_or_before) {
      if (auto restored = restore_snapshot(p); !restored) {
        return std::unexpected(restored.error());
      }
      return p.snap.tick();
    }
  }
  return make_error(errc::recovery_failed, "runtime: no snapshot at or before the fault tick");
}

result<void> runtime::restore_snapshot(const recovery_point &p) noexcept {
  const std::vector<det::snapshot_contributor *> contributors = contributors_;
  if (auto restored = sl::runtime::restore(*world_, components_, types_, contributors, p.snap,
                                           runtime_abi_digest());
      !restored) {
    return restored;
  }
  // ARCH3-R029, R145: logical time and every system's last run go back to the snapshot's.
  systems_->last_runs().restore(p.runs, p.runs_floor);
  frames_->rewind_logical(p.snap.tick());
  // ARCH3-R030: the results of background jobs from before the restore never reach the world.
  discard_background();
  return {};
}

void runtime::discard_background() noexcept {
  jobs_->drain_results(
      [this](const jobs::background_result & /*result*/) noexcept { discarded_.fetch_add(1); });
}

} // namespace sl::runtime
