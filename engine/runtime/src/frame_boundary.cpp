// The safe boundary (WO-089, SL-ARCH-003 ARCH3-R096, R098, R066, `boundary_slots`): hook
// registration, retire_after and end_frame: zero frame jobs in flight, the main-affinity queue
// drained, the job system quiesced, then every boundary slot in table order on the owner thread
// (retire_after callbacks due this frame in the retire_after slot, the frame arena flip in
// arena_flip), and a last main-affinity drain for jobs the hooks submitted.

#include "sl/core/assert.hpp"
#include "sl/core/error.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/jobs.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <utility>

namespace sl::runtime {

void frame_scheduler::add_boundary_hook(boundary_slot slot, boundary_hook hook) {
  SL_DEBUG_ASSERT(is_owner(), "frame_scheduler::add_boundary_hook off the owner thread");
  SL_DEBUG_ASSERT(!in_frame_, "frame_scheduler::add_boundary_hook during run_frame");
  const auto s = static_cast<std::size_t>(slot);
  if (!is_owner() || in_frame_ || s >= boundary_slot_count || !hook) {
    return;
  }
  boundary_hooks_[s].push_back(std::move(hook));
}

void frame_scheduler::add_phase_end_hook(phase_end_hook hook) {
  SL_DEBUG_ASSERT(is_owner(), "frame_scheduler::add_phase_end_hook off the owner thread");
  SL_DEBUG_ASSERT(!in_frame_, "frame_scheduler::add_phase_end_hook during run_frame");
  if (!is_owner() || in_frame_ || !hook) {
    return;
  }
  phase_end_hooks_.push_back(std::move(hook));
}

result<void> frame_scheduler::retire_after(std::uint32_t frames, std::function<void()> fn) {
  if (!is_owner()) {
    return make_error(errc::wrong_thread, "frame_scheduler::retire_after off the owner thread");
  }
  if (frames == 0) {
    return make_error(errc::invalid_argument, "frame_scheduler::retire_after: frames must be >= 1");
  }
  if (!fn) {
    return make_error(errc::invalid_argument, "frame_scheduler::retire_after: empty callback");
  }
  const std::size_t capacity = retire_.capacity();
  retire_.push_back(retire_entry{.due = frame_index_ + frames, .fn = std::move(fn)});
  if (in_frame_ && retire_.capacity() != capacity) {
    ++internal_allocs_;
  }
  return {};
}

void frame_scheduler::end_frame(frame_stats &stats) noexcept {
  const nanos start = platform_->clock.now();
  SL_ASSERT(frame_jobs_.value() == 0,
            "frame_scheduler: end_frame with frame jobs in flight (ARCH3-R096)");
  jobs_->run_main_affinity();
  jobs_->quiesce();
  for (std::size_t s = 0; s < boundary_slot_count; ++s) {
    run_slot(static_cast<boundary_slot>(s));
  }
  jobs_->run_main_affinity();
  record_phase(stats, phase::end_frame, start);
}

void frame_scheduler::run_slot(boundary_slot slot) noexcept {
  if (slot == boundary_slot::retire_after) {
    run_retired();
  } else if (slot == boundary_slot::arena_flip) {
    arena_->flip();
  }
  const boundary_context ctx{.slot = slot, .frame_index = frame_index_, .tick = logical_.now()};
  // A hook may call retire_after but never add a hook (refused during run_frame), so the list
  // does not change while it runs.
  const std::vector<boundary_hook> &hooks = boundary_hooks_[static_cast<std::size_t>(slot)];
  for (const boundary_hook &hook : hooks) {
    hook(ctx);
  }
}

void frame_scheduler::run_retired() noexcept {
  // Due entries run in registration order; a callback may call retire_after (appended past n, and
  // due a later frame), so the loop goes by index and the survivors are compacted afterwards.
  const std::size_t n = retire_.size();
  std::size_t keep = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (retire_[i].due <= frame_index_) {
      const std::function<void()> fn = std::move(retire_[i].fn);
      fn();
    } else {
      if (keep != i) {
        retire_[keep] = std::move(retire_[i]);
      }
      ++keep;
    }
  }
  for (std::size_t j = n; j < retire_.size(); ++j) {
    if (keep != j) {
      retire_[keep] = std::move(retire_[j]);
    }
    ++keep;
  }
  retire_.erase(std::next(retire_.begin(), static_cast<std::ptrdiff_t>(keep)), retire_.end());
}

} // namespace sl::runtime
