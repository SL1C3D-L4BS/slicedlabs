// The frame loop (WO-089, SL-ARCH-003 ARCH3-R094, R098, R079, R081, R083, R039): make, run_frame,
// begin_frame and the clocks. Phase runs live in frame_graph.cpp, end_frame in frame_boundary.cpp,
// the stats in frame_stats.cpp.

#include "sl/core/error.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/platform.hpp"
#include "sl/runtime/replay.hpp"
#include "sl/runtime/time.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <utility>

namespace sl::runtime {

result<std::unique_ptr<frame_scheduler>> frame_scheduler::make(jobs::job_system &jobs,
                                                               platform &plat,
                                                               mem::frame_arena &arena,
                                                               frame_config config) noexcept {
  auto stepper = time::fixed_stepper::make(config.rate, config.max_catchup);
  if (!stepper) {
    return std::unexpected(stepper.error());
  }
  if (config.max_frame_delta <= nanos{0}) {
    return make_error(errc::invalid_argument, "frame_scheduler: max_frame_delta must be above 0");
  }
  if (arena.workers() != jobs.worker_count()) {
    return make_error(errc::invalid_argument,
                      "frame_scheduler: the frame arena's workers differ from the job system's");
  }
  std::unique_ptr<frame_scheduler> made(new (std::nothrow)
                                            frame_scheduler(jobs, plat, arena, config, *stepper));
  if (!made) {
    return make_error(errc::out_of_memory, "frame_scheduler::make: allocation failed");
  }
  return made;
}

frame_scheduler::frame_scheduler(jobs::job_system &jobs, platform &plat, mem::frame_arena &arena,
                                 const frame_config &config, const time::fixed_stepper &stepper)
    : jobs_(&jobs), platform_(&plat), arena_(&arena), owner_(std::this_thread::get_id()),
      warmup_frames_(config.warmup_frames), scope_("runtime/" + std::to_string(plat.instance_id)),
      frame_clock_(config.max_frame_delta), stepper_(stepper), graphs_(phase_count),
      boundary_hooks_(boundary_slot_count), deliver_(result_sink{.self = this}) {}

void frame_scheduler::result_sink::operator()(const jobs::background_result &r) const {
  self->accept_result(r);
}

void frame_scheduler::accept_result(const jobs::background_result &r) {
  const std::size_t capacity = inputs_.capacity();
  const time::tick now = logical_.now();
  det::ordered_key key;
  key.tick = now;
  key.phase = static_cast<std::uint8_t>(phase::begin_frame);
  key.local_seq = static_cast<std::uint32_t>(inputs_.size());
  det::external_input in{
      .frame = frame_index_, .tick = now, .kind = r.kind, .bytes = r.bytes, .key = key};
  if (!r.bytes.empty()) {
    ++internal_allocs_; // the copied bytes
  }
  inputs_.push_back(std::move(in));
  if (inputs_.capacity() != capacity) {
    ++internal_allocs_;
  }
  platform_->sink.on_external(frame_index_, now, r.kind, r.bytes);
}

void frame_scheduler::begin_frame(frame_stats &stats) noexcept {
  const nanos start = platform_->clock.now();
  // ARCH3-R098, R079: the frame index grows here, with the one wall sample of the frame.
  const nanos sample =
      record_wall_sample(platform_->clock, platform_->sink, frame_clock_.frame_index() + 1);
  const nanos delta = frame_clock_.sample(sample);
  frame_index_ = frame_clock_.frame_index();
  stats.ticks_run = stepper_.advance(delta);
  current_ = time::frame_time{.frame_index = frame_index_,
                              .now = logical_.now(),
                              .delta = delta,
                              .a = stepper_.remainder()};
  inputs_.clear();
  jobs_->drain_results(deliver_);
  finish_phase(phase::begin_frame, start, stats);
}

result<frame_stats> frame_scheduler::run_frame() noexcept {
  if (!is_owner()) {
    return make_error(errc::wrong_thread, "frame_scheduler::run_frame off the owner thread");
  }
  if (!built_) {
    return make_error(errc::invalid_state, "frame_scheduler::run_frame before build");
  }
  if (in_frame_) {
    return make_error(errc::invalid_state, "frame_scheduler::run_frame inside a running frame");
  }
  in_frame_ = true;
  const std::uint64_t upstream_before = upstream_count();
  frame_stats stats;
  begin_frame(stats);
  run_phase(phase::apply_inputs, stats);
  for (std::uint32_t k = 0; k < stats.ticks_run; ++k) {
    const time::tick now = logical_.now();
    const time::tick_scope scope(now, scope_);
    current_.now = now;
    run_phase(phase::pre_tick, stats);
    run_phase(phase::tick, stats);
    run_phase(phase::post_tick, stats);
    logical_.advance();
  }
  current_.now = logical_.now();
  run_phase(phase::observe, stats);
  run_phase(phase::present, stats); // ARCH3-R099: no tasks, the phase end hooks only
  end_frame(stats);
  close_stats(stats, upstream_before);
  in_frame_ = false;
  return stats;
}

void frame_scheduler::freeze_clocks() noexcept {
  SL_DEBUG_ASSERT(is_owner(), "frame_scheduler::freeze_clocks off the owner thread");
  if (is_owner()) {
    frame_clock_.freeze();
  }
}

void frame_scheduler::thaw_clocks() noexcept {
  SL_DEBUG_ASSERT(is_owner(), "frame_scheduler::thaw_clocks off the owner thread");
  if (is_owner()) {
    frame_clock_.thaw();
  }
}

} // namespace sl::runtime
