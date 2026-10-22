// The runtime lifecycle (WO-096, SL-ARCH-003 ARCH3-R023 to R027, R053, ADR-0028, ADR-0023): start
// (all-or-nothing, unwound in reverse), the host-pumped frames, suspend and resume, the end_frame
// slots lifecycle_requests and diagnostics, the RFP9 visits, shutdown and its runtime.mem event.

#include "sl/runtime/runtime.hpp"

#include "sl/core/error.hpp"
#include "sl/core/log.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/ecs/command_buffer.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/faults.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/memdiag.hpp"
#include "sl/runtime/memory.hpp"
#include "sl/runtime/scheduler_stats.hpp"
#include "sl/runtime/snapshot.hpp"
#include "sl/runtime/work_queue.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sl::runtime {

namespace {

/// RFP9 suspend.request (`failpoints`): visited in every boundary slot and at every phase end.
void visit_suspend_point([[maybe_unused]] runtime &rt) noexcept {
#if SL_RUNTIME_FAULTS
  if (visit(failpoint::rfp9).fire) {
    rt.request_suspend();
  }
#endif
}

[[nodiscard]] mem::memory_tag tag_of(const mem::tag_registry &tags, std::string_view name) {
  auto found = tags.find(name);
  return found ? *found : mem::memory_tag{};
}

} // namespace

// --- start (ARCH3-R025) ---

result<void> runtime::start() noexcept {
  if (!is_owner()) {
    return make_error(errc::wrong_thread, "runtime::start off the owner thread");
  }
  if (auto begun = transition(rt_event::start); !begun) {
    return begun;
  }
  auto steps = start_steps();
  if (!steps) {
    teardown();
    apply_event(rt_event::start_fail);
    return steps;
  }
  {
    const std::scoped_lock lock(diag_mutex_);
    diag_published_ = true;
  }
  apply_event(rt_event::start_ok);
  return {};
}

result<void> runtime::start_steps() noexcept {
  // 1. platform check
  if (plat_->instance_id == 0) {
    return make_error(errc::runtime_failure, "runtime::start: the platform has instance id 0");
  }
  // 2. memory root
  if (auto root = memory_root(); !root) {
    return root;
  }
  // 3. tag freeze
  if (auto frozen = tags_.freeze(); !frozen) {
    return frozen;
  }
  // 4. registry freezes
  if (auto frozen = components_.freeze(); !frozen) {
    return frozen;
  }
  if (auto frozen = types_.freeze(); !frozen) {
    return frozen;
  }
  if (auto frozen = events_.freeze(); !frozen) {
    return frozen;
  }
  // 5. workers
  if (auto started = start_jobs(); !started) {
    return started;
  }
  // 6, 7. frame scheduler, world, system scheduler and the phase graphs
  if (auto built = start_frames(); !built) {
    return built;
  }
  // 8. no snapshot yet: the baseline is taken at the end of the first host frame (on_snapshot)
  last_snapshot_tick_ = 0;
  return {};
}

result<void> runtime::memory_root() noexcept {
  for (const std::string_view name : mem::runtime_tag_names) {
    if (!tags_.find(name)) {
      if (auto added = tags_.add(name); !added) {
        return std::unexpected(added.error());
      }
    }
  }
  for (const std::string_view name : mem::runtime_external_tag_names) {
    if (!tags_.find(name)) {
      if (auto added = tags_.reserve_external(name); !added) {
        return std::unexpected(added.error());
      }
    }
  }
  root_ = std::make_unique<mem::tracking_resource>("runtime.root", tag_of(tags_, "runtime.root"),
                                                   &tags_);
  world_memory_ = std::make_unique<mem::tracking_resource>("runtime.world",
                                                           tag_of(tags_, "ecs.entities"), &tags_);
  frame_memory_ = std::make_unique<mem::tracking_resource>("runtime.frame",
                                                           tag_of(tags_, "frame.arena"), &tags_);
  diag_ = std::make_unique<mem::diagnostics>(tags_, config_.instance_id,
                                             config_.report_interval_frames);
  diag_->add(*root_);
  diag_->add(*world_memory_);
  diag_->add(*frame_memory_);
  // The root block: the ring of recent fault ticks behind max_recoveries (ARCH3-R029).
  fault_ring_ = std::size_t{config_.max_recoveries} + 1U;
  auto block = root_->try_allocate(fault_ring_ * sizeof(time::tick), alignof(time::tick),
                                   root_->default_tag());
  if (!block) {
    return std::unexpected(block.error());
  }
  fault_ticks_ = static_cast<time::tick *>(*block);
  std::uninitialized_fill_n(fault_ticks_, fault_ring_, time::tick{0});
  fault_count_ = 0;
  return {};
}

result<void> runtime::start_jobs() noexcept {
  jobs::job_config jc;
  jc.workers = config_.workers.value_or(jobs::default_workers(config_.reserved_threads));
  jc.capacity = config_.job_capacity;
  jc.watchdog_ms = config_.watchdog_ms;
  jc.instance_id = config_.instance_id;
  jc.seed = config_.seed;
  if (config_.queue == queue_kind::work_stealing) {
    jc.make_queue = jobs::make_ws_queue;
    jobs::steal_options options;
    options.seed = config_.seed;
    auto policy = jobs::make_steal_policy(options);
    if (!policy) {
      return std::unexpected(policy.error());
    }
    jc.policy = std::move(*policy);
  } else {
    jc.make_queue = jobs::make_locked_queue;
  }
  jc.on_fatal = [this](const jobs::fatal_report &report) noexcept { on_fatal(report); };
  auto started = jobs::job_system::start(*plat_, std::move(jc));
  if (!started) {
    return std::unexpected(started.error());
  }
  jobs_ = std::move(*started);
  return {};
}

result<void> runtime::start_frames() noexcept {
  arena_ = std::make_unique<mem::frame_arena>(*frame_memory_, jobs_->worker_count(),
                                              mem::frame_arena_block_bytes,
                                              tag_of(tags_, "frame.arena"));
  diag_->add("frame.arena", *arena_);
  frame_config fc;
  fc.rate = config_.rate;
  fc.max_frame_delta = config_.max_frame_delta;
  fc.max_catchup = config_.max_catchup;
  auto made = frame_scheduler::make(*jobs_, *plat_, *arena_, fc);
  if (!made) {
    return std::unexpected(made.error());
  }
  frames_ = std::move(*made);
  // The world comes before the system build, which binds it.
  ecs::world_config wc;
  wc.max_entities = config_.max_entities;
  auto w = ecs::world::make(components_, *world_memory_, wc);
  if (!w) {
    return std::unexpected(w.error());
  }
  world_ = std::move(*w);
  world_->set_job_system(jobs_.get());
  queue_ = std::make_unique<ecs::command_queue>(components_, *arena_);
  systems_ = std::make_unique<ecs::system_scheduler>(*jobs_);
  std::vector<ecs::system_desc> staged = std::move(staged_systems_);
  staged_systems_.clear();
  system_names_.clear();
  for (ecs::system_desc &desc : staged) {
    std::string name(desc.name);
    if (auto added = systems_->add(std::move(desc)); !added) {
      return std::unexpected(added.error());
    }
    system_names_.push_back(std::move(name));
  }
  // Before the system scheduler's own phase hook: a fatal job report clears the phase's commands
  // before they are played back (a failed system's are discarded by the scheduler itself).
  frames_->add_phase_end_hook(
      [this](phase /*ph*/, time::tick /*tick*/) noexcept { discard_after_fatal(); });
  if (auto built = systems_->build(*frames_, *world_, *queue_); !built) {
    return built;
  }
  wire_hooks();
  if (auto built = frames_->build(); !built) {
    return built;
  }
  const std::string schedule = systems_->export_json();
  schedule_digest_ = detail::sha256_of(std::as_bytes(std::span(schedule)));
  failures_seen_ = systems_->failures();
  errors_seen_.assign(system_names_.size(), errc::ok);
  return {};
}

void runtime::wire_hooks() {
  // After the system scheduler's own phase hook (registered by build): last_flush is this phase's.
  frames_->add_phase_end_hook([this](phase ph, time::tick /*tick*/) noexcept {
    on_phase_end(ph);
    visit_suspend_point(*this);
  });
  for (std::size_t i = 0; i < boundary_slot_count; ++i) {
    const auto slot = static_cast<boundary_slot>(i);
    frames_->add_boundary_hook(slot, [this, slot](const boundary_context &bc) noexcept {
      switch (slot) {
      case boundary_slot::fault_check:
        on_fault_check(bc);
        break;
      case boundary_slot::snapshot:
        on_snapshot(bc);
        break;
      case boundary_slot::swap:
        on_swap(bc);
        break;
      case boundary_slot::lifecycle_requests:
        on_lifecycle_requests();
        break;
      case boundary_slot::diagnostics:
        on_diagnostics(bc);
        break;
      default:
        break;
      }
      visit_suspend_point(*this);
    });
  }
}

// --- teardown (reverse start order) ---

void runtime::release_frame_state() noexcept {
  last_tick_ = current_tick();
  systems_.reset();
  queue_.reset();
  world_.reset();
  frames_.reset();
  arena_.reset();
  snapshots_.clear();
  pre_swap_.reset();
  swap_active_.reset();
  if (fault_ticks_ != nullptr) {
    root_->release(fault_ticks_, fault_ring_ * sizeof(time::tick), alignof(time::tick));
    fault_ticks_ = nullptr;
  }
}

void runtime::teardown() noexcept {
  release_frame_state();
  if (jobs_ != nullptr) {
    // cppcheck-suppress unreadVariable ; teardown has no caller to report the stop result to
    [[maybe_unused]] const result<void> stopped = jobs_->stop();
    jobs_.reset();
  }
  {
    const std::scoped_lock lock(diag_mutex_);
    diag_published_ = false;
    diag_.reset();
  }
  frame_memory_.reset();
  world_memory_.reset();
  root_.reset();
}

// --- frames (ARCH3-R024) ---

result<frame_stats> runtime::run_frame() noexcept {
  if (!is_owner()) {
    return make_error(errc::wrong_thread, "runtime::run_frame off the owner thread");
  }
  if (state_.load() == rt_state::ready) {
    if (auto running = transition(rt_event::run_frame); !running) {
      return std::unexpected(running.error());
    }
  } else if (state_.load() != rt_state::running) {
    return make_error(errc::invalid_state, std::string("runtime::run_frame in ") +
                                               std::string(to_string(state_.load())));
  }
  frame_faulted_.store(false);
  frame_error_ = errc::ok;
  playback_oom_ = false;
  auto stats = frames_->run_frame();
  if (!stats) {
    return std::unexpected(stats.error());
  }
  // cppcheck-suppress knownConditionTrueFalse ; the fault_check slot sets it inside run_frame
  if (frame_error_ == errc::runtime_faulted) {
    return make_error(errc::runtime_faulted, "runtime::run_frame: a fault stopped the runtime");
  }
  // cppcheck-suppress knownConditionTrueFalse ; the fault_check slot sets it inside run_frame
  if (frame_error_ != errc::ok) {
    return make_error(frame_error_, "runtime::run_frame: recovery failed");
  }
  return stats;
}

result<void> runtime::run_frames(std::uint32_t n) noexcept {
  for (std::uint32_t i = 0; i < n; ++i) {
    if (auto ran = run_frame(); !ran) {
      return std::unexpected(ran.error());
    }
    if (state_.load() != rt_state::running) {
      return {};
    }
  }
  return {};
}

// --- suspend, resume, stop (ARCH3-R023, R026) ---

void runtime::request_suspend() noexcept { suspend_requested_.store(true); }

void runtime::request_stop() noexcept { stop_requested_.store(true); }

result<void> runtime::resume() noexcept {
  if (!is_owner()) {
    return make_error(errc::wrong_thread, "runtime::resume off the owner thread");
  }
  if (auto resumed = transition(rt_event::resume); !resumed) {
    return resumed;
  }
  frames_->thaw_clocks();
  return {};
}

void runtime::on_lifecycle_requests() noexcept {
  if (state_.load() != rt_state::running) {
    return;
  }
  if (stop_requested_.exchange(false)) {
    apply_event(rt_event::stop);
    return;
  }
  if (suspend_requested_.exchange(false)) {
    apply_event(rt_event::suspend);
    frames_->freeze_clocks();
  }
}

void runtime::on_diagnostics(const boundary_context &bc) noexcept {
  last_frame_.store(bc.frame_index);
  last_tick_ = bc.tick;
  diag_->on_frame_end(bc.frame_index);
}

// --- shutdown (ARCH3-R027) ---

result<void> runtime::shutdown() noexcept {
  if (!is_owner()) {
    return make_error(errc::wrong_thread, "runtime::shutdown off the owner thread");
  }
  const rt_state s = state_.load();
  if (s == rt_state::ready || s == rt_state::running || s == rt_state::suspended ||
      s == rt_state::faulted) {
    if (auto stopping = transition(rt_event::stop); !stopping) {
      return stopping;
    }
  }
  if (state_.load() != rt_state::stopping) {
    return make_error(errc::invalid_state,
                      std::string("runtime::shutdown in ") + std::string(to_string(s)));
  }
  // Cancels every job that has not started (job_cancelled), waits for running ones, joins.
  const result<void> stopped = jobs_->stop();
  discard_background();
  release_frame_state();
  emit_shutdown_report();
  apply_event(rt_event::stopped);
  if (!stopped) {
    return stopped;
  }
  if (config_.leak_check) {
    const mem::memory_report report = memory_report();
    const std::uint64_t live = std::accumulate(
        report.tags.begin(), report.tags.end(), std::uint64_t{0},
        [](std::uint64_t sum, const mem::tag_stats &t) { return sum + t.live_bytes; });
    if (live != 0) {
      return make_error(errc::leak_detected,
                        "runtime::shutdown: " + std::to_string(live) + " tracked bytes still live");
    }
  }
  return {};
}

void runtime::emit_shutdown_report() noexcept {
  mem::memory_report report = diag_->make_report(last_frame_.load());
  report.leaks = diag_->leaks();
  std::uint64_t live_bytes = 0;
  std::uint64_t live_blocks = 0;
  std::uint64_t peak_bytes = 0;
  for (const mem::tag_stats &t : report.tags) {
    live_bytes += t.live_bytes;
    live_blocks += t.live_blocks;
    peak_bytes += t.peak_bytes;
  }
  {
    const sl::log::correlation_scope correlation(scope_, last_tick_);
    sl::log::emit(sl::log::level::info, "runtime.mem", "shutdown memory report",
                  {{"instance", config_.instance_id},
                   {"frame", report.frame},
                   {"live_bytes", live_bytes},
                   {"live_blocks", live_blocks},
                   {"peak_bytes", peak_bytes},
                   {"leaks", report.leaks.size()},
                   {"shutdown", true}});
  }
  sl::log::default_logger().flush();
  const std::scoped_lock lock(diag_mutex_);
  shutdown_report_ = std::move(report);
}

} // namespace sl::runtime
