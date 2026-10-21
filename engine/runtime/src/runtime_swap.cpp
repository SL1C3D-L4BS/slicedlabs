// Swaps (WO-096, SL-ARCH-003 ARCH3-R031, ADR-0028): request_swap queues a request for the
// end_frame slot swap. Classes component_layout, allocator, scheduler and runtime_abi end at once
// as restart_required; data and gameplay_function take a pre-swap snapshot, apply, then run the
// health check at every end_frame until health_ticks ticks passed (committed) or it returns false
// (the pre-swap snapshot is restored: rolled_back, swap_rejected).

#include "sl/runtime/runtime.hpp"

#include "sl/core/error.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/snapshot.hpp"

#include <mutex>
#include <optional>
#include <utility>

namespace sl::runtime {

namespace {

[[nodiscard]] bool restart_class(swap_class cls) noexcept {
  return cls == swap_class::component_layout || cls == swap_class::allocator ||
         cls == swap_class::scheduler || cls == swap_class::runtime_abi;
}

} // namespace

void runtime::request_swap(swap_request request) {
  const std::scoped_lock lock(swap_mutex_);
  swap_pending_ = std::move(request);
  swap_state_.store(swap_state::pending);
  swap_code_.store(errc::ok);
}

void runtime::finish_swap(swap_state outcome, errc code) noexcept {
  swap_active_.reset();
  pre_swap_.reset();
  swap_code_.store(code);
  swap_state_.store(outcome);
}

void runtime::on_swap(const boundary_context &bc) noexcept {
  if (frame_faulted_.load()) {
    // The recovery replaced the state the active swap was checked against.
    if (swap_active_) {
      finish_swap(swap_state::rolled_back, errc::swap_rejected);
    }
    return;
  }
  if (state_.load() != rt_state::running) {
    return;
  }
  if (swap_active_ && pre_swap_) {
    const bool healthy = !swap_active_->health_check || swap_active_->health_check(*this, bc.tick);
    if (!healthy) {
      const result<void> restored = restore_snapshot(*pre_swap_);
      finish_swap(swap_state::rolled_back, restored ? errc::swap_rejected : restored.error().code);
      return;
    }
    if (bc.tick - swap_tick_ >= swap_active_->health_ticks) {
      finish_swap(swap_state::committed, errc::ok);
    }
    return;
  }
  std::optional<swap_request> request;
  {
    const std::scoped_lock lock(swap_mutex_);
    request.swap(swap_pending_);
  }
  if (!request) {
    return;
  }
  if (restart_class(request->cls)) {
    finish_swap(swap_state::restart_required, errc::restart_required);
    return;
  }
  auto pre = take_snapshot(bc.frame_index, bc.tick);
  if (!pre) {
    finish_swap(swap_state::rolled_back, pre.error().code);
    return;
  }
  if (request->apply) {
    if (auto applied = request->apply(*this); !applied) {
      const result<void> restored = restore_snapshot(*pre);
      finish_swap(swap_state::rolled_back, restored ? applied.error().code : restored.error().code);
      return;
    }
  }
  if (request->health_ticks == 0 || !request->health_check) {
    finish_swap(swap_state::committed, errc::ok);
    return;
  }
  pre_swap_.emplace(std::move(*pre));
  swap_active_ = std::move(request);
  swap_tick_ = bc.tick;
}

} // namespace sl::runtime
