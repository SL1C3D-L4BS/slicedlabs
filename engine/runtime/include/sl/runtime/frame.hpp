#pragma once

// The frame scheduler (WO-089, SL-ARCH-003 ARCH3-R094 to R100, R051, episode 090, ADR-0024,
// ADR-0026). A frame runs the `phases` table in order: begin_frame (frame index + 1, one
// clock_source sample through the frame clock, the sample and every drained background result
// passed to the record_sink), apply_inputs, 0..max_catchup fixed ticks of pre_tick, tick and
// post_tick (each under a time::tick_scope; the logical tick + 1 after post_tick), observe,
// present (a hook slot without tasks, Volume 07 seam V07-1) and end_frame, the safe boundary: zero
// frame jobs in flight, the job system quiesced, then the `boundary_slots` in order on the owner
// thread (retire_after callbacks in the retire_after slot, frame_arena::flip in arena_flip). Each
// task phase submits its tasks as frame jobs with their intra-phase after-edges, waits (helping),
// drains the main-affinity queue and runs the phase end hooks. Phase graphs are built once in
// build(); add_task afterwards is schedule_frozen. frame_stats records per-phase job counts, the
// critical path in jobs and per-phase wall time (recorded, never asserted). Sources:
// src/frame_scheduler.cpp (make, run_frame, begin_frame, clocks), src/frame_graph.cpp (add_task,
// build, phase runs), src/frame_boundary.cpp (hooks, retire_after, end_frame) and
// src/frame_stats.cpp (stats and upstream counting). Layer L4: includes L0 to L3 runtime headers.
//
// Thread safety (`thread_safety`): every frame_scheduler member is owner-thread, the thread that
// called make (add_task, build, retire_after and run_frame return wrong_thread elsewhere; the void
// calls are a Debug assertion and do nothing); task bodies are worker-only and run as frame jobs.

#include "sl/core/error.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/platform.hpp"
#include "sl/runtime/replay.hpp"
#include "sl/runtime/sync.hpp"
#include "sl/runtime/time.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace sl::runtime {

/// The rows of the `phases` table; the value is the ordered_key phase byte (phases.index).
enum class phase : std::uint8_t {
  begin_frame = 0,
  apply_inputs,
  pre_tick,
  tick,
  post_tick,
  observe,
  present,
  end_frame
};

/// The `boundary_slots` table, in run order.
enum class boundary_slot : std::uint8_t {
  fault_check,
  observation_flush,
  projection_rebase,
  snapshot,
  swap,
  lifecycle_requests,
  retire_after,
  arena_flip,
  diagnostics
};

inline constexpr std::size_t phase_count = 8;
inline constexpr std::size_t boundary_slot_count = 9;
/// `limits` max_systems_per_phase: the tasks one phase may hold.
inline constexpr std::uint32_t max_tasks_per_phase = 256;
/// `limits` warmup_frames: frames before zero-upstream counting.
inline constexpr std::uint32_t warmup_frames_default = 10;

// NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members): a view handed to one task body
/// What a task body sees: its job context, the frame's time value, its task id and the logical tick
/// it runs for (the current tick outside simulate). Worker-only: valid inside the body only.
// cppcheck-suppress-begin uninitMemberVarNoCtor
struct task_context {
  jobs::job_context &job;
  const time::frame_time &time;
  std::uint32_t task = 0;
  std::uint64_t tick = 0;
};
// cppcheck-suppress-end uninitMemberVarNoCtor
// NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)

/// A task body stored inline with the job_fn constraints: a callable F with F& noexcept-invocable
/// on task_context&, at most 48 bytes, aligned at most 16 and nothrow movable. Invoked once per
/// phase run (every frame, every tick), so it is not consumed. Move-only.
class task_fn {
public:
  task_fn() noexcept = default;

  template <class F>
    requires(!std::same_as<std::remove_cvref_t<F>, task_fn>)
  // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions,bugprone-forwarding-reference-overload)
  task_fn(F &&f) noexcept { // cppcheck-suppress noExplicitConstructor ; converts like job_fn
    using T = std::remove_cvref_t<F>;
    static_assert(std::is_nothrow_invocable_v<T &, task_context &>,
                  "a task body must be noexcept-invocable with task_context& (ARCH3-R061)");
    static_assert(sizeof(T) <= jobs::job_inline_payload_bytes,
                  "a task body must fit the 48-byte inline payload (ARCH3-R061)");
    static_assert(alignof(T) <= jobs::job_payload_alignment,
                  "a task body must be aligned at most 16");
    static_assert(std::is_nothrow_move_constructible_v<T>, "a task body must be nothrow movable");
    static_assert(std::is_nothrow_constructible_v<T, F &&>,
                  "a task body must be nothrow constructible from its argument");
    ::new (static_cast<void *>(storage_.data())) T(std::forward<F>(f));
    ops_ = &ops_for<T>;
  }

  task_fn(task_fn &&other) noexcept : ops_(other.ops_) {
    if (ops_ != nullptr) {
      ops_->relocate(storage_.data(), other.storage_.data());
      other.ops_ = nullptr;
    }
  }

  task_fn &operator=(task_fn &&other) noexcept {
    if (this != &other) {
      reset();
      ops_ = other.ops_;
      if (ops_ != nullptr) {
        ops_->relocate(storage_.data(), other.storage_.data());
        other.ops_ = nullptr;
      }
    }
    return *this;
  }

  task_fn(const task_fn &) = delete;
  task_fn &operator=(const task_fn &) = delete;
  ~task_fn() { reset(); }

  [[nodiscard]] explicit operator bool() const noexcept { return ops_ != nullptr; }

  /// Runs the body (a no-op when empty).
  void operator()(task_context &ctx) noexcept {
    if (ops_ != nullptr) {
      ops_->invoke(storage_.data(), ctx);
    }
  }

private:
  struct fn_ops {
    void (*invoke)(void *self, task_context &ctx) noexcept;
    void (*relocate)(void *dst, void *src) noexcept; // move-constructs dst, destroys src
    void (*destroy)(void *self) noexcept;
  };

  // Named functions rather than captureless lambdas (WO-107: gcc -fanalyzer).
  template <class T> static void invoke_as(void *self, task_context &ctx) noexcept {
    (*static_cast<T *>(self))(ctx);
  }
  template <class T> static void relocate_as(void *dst, void *src) noexcept {
    ::new (dst) T(std::move(*static_cast<T *>(src)));
    static_cast<T *>(src)->~T();
  }
  template <class T> static void destroy_as(void *self) noexcept { static_cast<T *>(self)->~T(); }

  template <class T>
  static constexpr fn_ops ops_for{&invoke_as<T>, &relocate_as<T>, &destroy_as<T>};

  void reset() noexcept {
    if (ops_ != nullptr) {
      ops_->destroy(storage_.data());
      ops_ = nullptr;
    }
  }

  using payload = std::array<std::byte, jobs::job_inline_payload_bytes>;
  alignas(jobs::job_payload_alignment) payload storage_{};
  const fn_ops *ops_ = nullptr;
};

/// One task: a debug name (copied), its phase (apply_inputs, pre_tick, tick, post_tick or
/// observe), its body and the ids of earlier tasks of the same phase it runs after (at most 8).
struct task_desc {
  std::string_view name;
  phase ph = phase::tick;
  task_fn fn;
  std::vector<std::uint32_t> after;
};

struct frame_config {
  time::tick_rate rate{60, 1};
  nanos max_frame_delta{time::max_frame_delta_default};
  std::uint32_t max_catchup = time::max_catchup_default;
  std::uint32_t warmup_frames = warmup_frames_default;
};

/// One frame as run_frame saw it (transient-diagnostic). dropped_ticks, dropped_ns and
/// non_monotonic are the clocks' totals since make; upstream_allocs is this frame's delta.
struct frame_stats {
  std::uint64_t frame_index = 0;
  std::uint32_t ticks_run = 0;
  std::uint64_t dropped_ticks = 0;
  nanos dropped_ns{0};
  std::uint64_t non_monotonic = 0;
  struct per_phase {
    /// Jobs submitted in the phase this frame (summed over the ticks for the tick sub-phases).
    std::uint32_t jobs = 0;
    /// The longest after-chain of the phase graph in jobs; 0 when the phase did not run.
    std::uint32_t critical_path = 0;
    /// Wall time of the phase this frame, hooks included (recorded, never asserted).
    nanos wall{0};
  };
  std::array<per_phase, phase_count> phases{};
  std::uint64_t upstream_allocs = 0;
};

/// What a boundary hook sees.
struct boundary_context {
  boundary_slot slot = boundary_slot::fault_check;
  std::uint64_t frame_index = 0;
  time::tick tick = 0;
};

class frame_scheduler {
public:
  using boundary_hook = std::function<void(const boundary_context &)>;
  using phase_end_hook = std::function<void(phase, time::tick)>;

  /// Validates `config` (the stepper's invalid_tick_rate; invalid_argument for a max_frame_delta
  /// at or below 0 or a frame arena whose workers() differs from the job system's worker count)
  /// and builds the scheduler. The calling thread becomes the owner thread; it must be the job
  /// system's owner. Every reference must outlive the scheduler.
  [[nodiscard]] static result<std::unique_ptr<frame_scheduler>>
  make(jobs::job_system &jobs, platform &plat, mem::frame_arena &arena,
       frame_config config = {}) noexcept;

  frame_scheduler(const frame_scheduler &) = delete;
  frame_scheduler(frame_scheduler &&) = delete;
  frame_scheduler &operator=(const frame_scheduler &) = delete;
  frame_scheduler &operator=(frame_scheduler &&) = delete;
  ~frame_scheduler() = default;

  /// Adds a task and returns its id (from 0, in add order). schedule_frozen after build;
  /// invalid_argument for a phase without tasks (begin_frame, present, end_frame), an empty body,
  /// more than 8 after ids, an unknown after id or a cross-phase edge; out_of_range beyond
  /// max_tasks_per_phase in one phase.
  [[nodiscard]] result<std::uint32_t> add_task(task_desc desc) noexcept;
  /// Freezes the schedule and builds every phase graph once; a second call returns ok.
  [[nodiscard]] result<void> build() noexcept;
  /// Runs `hook` in `slot` of every end_frame, after the slot's own action, in registration
  /// order. Not during run_frame (Debug assertion; ignored).
  void add_boundary_hook(boundary_slot slot, boundary_hook hook);
  /// Runs `hook` at every phase sync boundary from begin_frame to present (each tick sub-phase
  /// once per tick, with that tick), in registration order. Not during run_frame (as above).
  void add_phase_end_hook(phase_end_hook hook);
  /// Runs `fn` on the owner thread in the retire_after slot of the end_frame `frames` frames after
  /// the current frame index; invalid_argument for frames 0 or an empty fn.
  [[nodiscard]] result<void> retire_after(std::uint32_t frames, std::function<void()> fn);
  /// Runs one frame. wrong_thread off the owner thread; invalid_state before build or inside a
  /// hook of a frame that is running.
  [[nodiscard]] result<frame_stats> run_frame() noexcept;
  /// Suspend and resume the frame clock (time::frame_clock freeze and thaw).
  void freeze_clocks() noexcept;
  void thaw_clocks() noexcept;
  /// Snapshot restore only (ARCH3-R029): sets the logical tick to `t` (owner thread, in an
  /// end_frame slot or between frames); the frame clock and the stepper keep their state.
  void rewind_logical(time::tick t) noexcept;
  /// Marks the running frame faulted, or unmarks it once a restore replaced its state (ARCH3-R028):
  /// while marked, attach's hooks skip deliver and, at arena_flip, keep both event lists and drop
  /// the undelivered writes. Owner thread, inside run_frame; every frame starts unmarked.
  void set_frame_faulted(bool faulted) noexcept;
  [[nodiscard]] bool frame_faulted() const noexcept { return frame_faulted_; }

  /// The frame's time value: its now is the running tick inside simulate.
  [[nodiscard]] const time::frame_time &current() const noexcept { return current_; }
  /// The background results begin_frame drained this frame, as external inputs in delivery order.
  [[nodiscard]] std::span<const det::external_input> frame_inputs() const noexcept {
    return inputs_;
  }
  /// The upstream allocations of the last frame (frame_stats::upstream_allocs).
  [[nodiscard]] std::uint64_t upstream_allocs_last_frame() const noexcept { return last_upstream_; }

private:
  // The body of one frame job: runs task `task` of `self`.
  struct frame_job {
    frame_scheduler *self = nullptr;
    std::uint32_t task = 0;
    void operator()(jobs::job_context &ctx) const noexcept;
  };

  // The drain_results callback: turns a background result into an external input.
  struct result_sink {
    frame_scheduler *self = nullptr;
    void operator()(const jobs::background_result &r) const;
  };

  struct task_record {
    std::string name;
    phase ph = phase::tick;
    task_fn fn;
    std::vector<std::uint32_t> after;
    std::uint32_t local = 0; // the position in its phase graph (build)
  };

  struct phase_graph {
    std::vector<std::uint32_t> tasks; // task ids in add order (a topological order)
    std::uint32_t critical_path = 0;
  };

  struct retire_entry {
    std::uint64_t due = 0;
    std::function<void()> fn;
  };

  frame_scheduler(jobs::job_system &jobs, platform &plat, mem::frame_arena &arena,
                  const frame_config &config, const time::fixed_stepper &stepper);

  [[nodiscard]] bool is_owner() const noexcept { return std::this_thread::get_id() == owner_; }

  // frame_scheduler.cpp
  void begin_frame(frame_stats &stats) noexcept;
  void accept_result(const jobs::background_result &r);

  // frame_graph.cpp
  void run_phase(phase ph, frame_stats &stats) noexcept;
  void finish_phase(phase ph, nanos start, frame_stats &stats) noexcept;
  void run_task(std::uint32_t task, jobs::job_context &ctx) noexcept;

  // frame_boundary.cpp
  void end_frame(frame_stats &stats) noexcept;
  void run_slot(boundary_slot slot) noexcept;
  void run_retired() noexcept;

  // frame_stats.cpp
  [[nodiscard]] std::uint64_t upstream_count() const noexcept;
  void record_phase(frame_stats &stats, phase ph, nanos start) const noexcept;
  void close_stats(frame_stats &stats, std::uint64_t upstream_before) noexcept;

  jobs::job_system *jobs_ = nullptr;
  platform *platform_ = nullptr;
  mem::frame_arena *arena_ = nullptr;
  std::thread::id owner_;
  std::uint32_t warmup_frames_ = warmup_frames_default;
  std::string scope_; // runtime/<instance>: the tick_scope correlation scope

  time::frame_clock frame_clock_;
  time::fixed_stepper stepper_;
  time::logical_clock logical_;
  time::frame_time current_;
  std::uint64_t frame_index_ = 0;

  bool built_ = false;
  bool in_frame_ = false;
  bool frame_faulted_ = false;
  std::vector<task_record> tasks_;
  std::vector<phase_graph> graphs_;       // phase_count graphs (build)
  std::vector<jobs::job_handle> handles_; // the handles of the running phase
  job_counter frame_jobs_;                // frame jobs in flight

  std::vector<std::vector<boundary_hook>> boundary_hooks_; // boundary_slot_count lists
  std::vector<phase_end_hook> phase_end_hooks_;
  std::vector<retire_entry> retire_;

  std::function<void(const jobs::background_result &)> deliver_;
  std::vector<det::external_input> inputs_;

  std::uint64_t internal_allocs_ = 0; // growth of the scheduler's own containers during frames
  std::uint64_t last_upstream_ = 0;
};

} // namespace sl::runtime
