#pragma once

// The runtime: lifecycle and recovery (WO-096, SL-ARCH-003 ARCH3-R022 to R034, ADR-0028; episode
// 081). A runtime composes the platform, the memory root (tracking resources and diagnostics), the
// job system, the frame scheduler, the system scheduler and exactly one world, and walks the
// `rt_states` table: Created -start-> Starting -start_ok-> Ready -run_frame-> Running, suspend and
// resume, fault and recovery, stop and stopped. Every transition emits one sl::log event (system
// runtime.lifecycle, fields from, to, tick, instance) under the correlation scope runtime/<id>.
// start is all-or-nothing: platform check, memory root (the runtime tags, the tracking resources
// and the root block), tag freeze, registry freezes, job system, frame scheduler, world, system
// scheduler and phase graphs, the tick-0 snapshot; a failing step unwinds the completed ones in
// reverse and leaves state Created. The host pumps frames (run_frame, run_frames); the runtime
// never loops or sleeps. The end_frame slots the runtime wires: fault_check (a fatal job, a failed
// system or out_of_memory in playback moves Running to Faulted; the frame's commands are cleared,
// frame_faulted() tells the link to skip the observation flush; one sl-runtime-fault/1 report),
// snapshot (every snapshot_every ticks, the newest two kept), swap, lifecycle_requests (stop, then
// suspend) and diagnostics (diagnostics::on_frame_end). In SL_RUNTIME_FAULTS builds every boundary
// slot and every phase end visits RFP9 (suspend.request), which issues request_suspend. Sources:
// src/runtime_core.cpp (table, create, accessors, digest), src/runtime_lifecycle.cpp (start,
// frames, suspend, shutdown), src/runtime_fault.cpp (fault capture, sl-runtime-fault/1),
// src/runtime_recovery.cpp (snapshots, restore), src/runtime_swap.cpp (request_swap) and
// src/runtime_terminate.cpp (install_terminate_report). Layer L9.
//
// Thread safety (`thread_safety`): the owner thread is the thread that called create; start,
// run_frame, run_frames, resume and shutdown return wrong_thread elsewhere, and the registry and
// registration calls (tags, components, types, events, add_system, add_contributor) are
// owner-thread before start. request_suspend, request_stop and request_swap are any-thread and
// take effect at the next end_frame; state, last_fault, memory_report, swap_status, swap_code,
// frame_faulted and discarded_results are any-thread. world, frames, jobs, systems and
// root_memory are valid from a successful start until shutdown (jobs and root_memory until
// destruction), owner-thread as their own classes say. Two runtimes share no mutable state.

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/ecs/command_buffer.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/events.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/memdiag.hpp"
#include "sl/runtime/memory.hpp"
#include "sl/runtime/platform.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/replay.hpp"
#include "sl/runtime/snapshot.hpp"
#include "sl/runtime/time.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace sl::runtime {

// --- the rt_states table (ARCH3-R022) ---

enum class rt_state : std::uint8_t {
  created,
  starting,
  ready,
  running,
  suspended,
  stopping,
  stopped,
  faulted,
  recovering
};

/// The events of `rt_states`, in table order.
enum class rt_event : std::uint8_t {
  start,
  start_ok,
  start_fail,
  run_frame,
  suspend,
  resume,
  stop,
  fault,
  recover,
  recover_ok,
  recover_fail,
  stopped
};

inline constexpr std::size_t rt_state_count = 9;
inline constexpr std::size_t rt_event_count = 12;

/// The table spelling: "Created", "Starting", ...
[[nodiscard]] std::string_view to_string(rt_state s) noexcept;
/// The table spelling: "start", "start_ok", ...
[[nodiscard]] std::string_view to_string(rt_event e) noexcept;
/// The state `rt_states` moves `from` to on `e`; invalid_state for every other pair.
[[nodiscard]] result<rt_state> next_state(rt_state from, rt_event e) noexcept;

// --- configuration ---

enum class recovery_policy : std::uint8_t { stop, restore_snapshot };
enum class queue_kind : std::uint8_t { locked, work_stealing };
enum class swap_class : std::uint8_t {
  data,
  gameplay_function,
  component_layout,
  allocator,
  scheduler,
  runtime_abi
};

/// The runtime's configuration; the defaults are the `limits` table.
struct runtime_config {
  /// The correlation scope is runtime/<instance_id> (at least 1).
  std::uint32_t instance_id = 1;
  /// Worker threads; nullopt means hardware_concurrency - 1 - reserved_threads, at least 1.
  std::optional<std::uint32_t> workers;
  std::uint32_t reserved_threads = 0;
  std::uint32_t job_capacity = jobs::job_capacity_default;
  /// The per-worker queue: the Chase-Lev deque with the stealing policy (WO-086's TSan run is
  /// green), or the locked deque with the reference policy.
  queue_kind queue = queue_kind::work_stealing;
  time::tick_rate rate{.num = 60, .den = 1};
  nanos max_frame_delta{std::chrono::milliseconds(250)};
  std::uint32_t max_catchup = time::max_catchup_default;
  std::uint32_t max_entities = ecs::max_entities_default;
  recovery_policy recovery = recovery_policy::stop;
  std::uint32_t max_recoveries = 3;
  std::uint32_t recovery_window_ticks = 600;
  /// Ticks between two automatic snapshots; 0 takes none (and restore_snapshot has nothing).
  std::uint32_t snapshot_every = 0;
  std::uint32_t watchdog_ms = jobs::watchdog_ms_default;
  det::determinism_mode mode = det::determinism_mode::deterministic;
  std::uint32_t report_interval_frames = mem::report_interval_frames;
  std::uint64_t seed = 0;
  /// Live tracked bytes at shutdown make shutdown return leak_detected (tests and the gate).
  bool leak_check = false;
  enum class process_mode : std::uint8_t { in_process };
  process_mode process = process_mode::in_process;
};

// --- faults (ARCH3-R028) ---

/// One fault, as sl-runtime-fault/1 (schemas/runtime/fault.schema.json).
struct fault_report {
  std::uint32_t instance = 1;
  std::uint64_t frame = 0;
  time::tick tick = 0;
  std::string kind; // job_fatal, system_fail, playback_oom
  errc code = errc::runtime_failure;
  std::string system; // the failed system ("" for a job or playback fault)
  std::string job;    // the failed job ("<system>#<system_index>" for a system)
  std::string action; // stop, restore
  std::optional<time::tick> restored_tick;

  friend bool operator==(const fault_report &, const fault_report &) = default;
};

/// sl-runtime-fault/1: keys in schema order, two-space indentation, one trailing newline.
[[nodiscard]] std::string to_json(const fault_report &report);

// --- swaps (ARCH3-R031) ---

class runtime;

struct swap_request {
  swap_class cls = swap_class::data;
  /// Runs on the owner thread in the swap slot of end_frame.
  std::function<result<void>(runtime &)> apply;
  /// Runs at every end_frame after apply until health_ticks ticks passed; false rolls back.
  std::function<bool(runtime &, time::tick)> health_check;
  std::uint32_t health_ticks = 60;
};

enum class swap_state : std::uint8_t { none, pending, committed, rolled_back, restart_required };

// --- the runtime ---

class runtime {
  struct passkey {};

public:
  /// A runtime in state Created on `plat` (which must outlive it): invalid_argument for an
  /// instance_id of 0, a tick rate with a zero term, a max_frame_delta at or below 0, more than
  /// jobs::max_workers workers, a job_capacity outside 1..job_capacity_max, a max_entities of 0;
  /// out_of_memory when the runtime cannot be allocated. The calling thread becomes the owner.
  [[nodiscard]] static result<std::unique_ptr<runtime>>
  create(platform &plat, const runtime_config &config) noexcept;

  runtime(passkey /*key*/, platform &plat, const runtime_config &config);
  runtime(const runtime &) = delete;
  runtime(runtime &&) = delete;
  runtime &operator=(const runtime &) = delete;
  runtime &operator=(runtime &&) = delete;
  /// Tears down whatever is still alive (no transition is logged).
  ~runtime();

  // --- registration (owner thread, before start) ---

  [[nodiscard]] mem::tag_registry &tags() noexcept { return tags_; }
  [[nodiscard]] ecs::component_registry &components() noexcept { return components_; }
  [[nodiscard]] type_registry &types() noexcept { return types_; }
  [[nodiscard]] event_registry &events() noexcept { return events_; }
  /// Stages a system for start, which adds it to the system scheduler in staging order (the
  /// scheduler's add errors fail start). wrong_thread off the owner thread; invalid_state after
  /// start; invalid_argument for an empty name or body.
  [[nodiscard]] result<void> add_system(ecs::system_desc desc);
  /// Adds a snapshot contributor (owner thread, before start; it outlives the runtime).
  void add_contributor(det::snapshot_contributor &contributor);

  // --- lifecycle (owner thread) ---

  /// Created -> Starting -> Ready, or Starting -> Created with the failing step's error.
  [[nodiscard]] result<void> start() noexcept;
  /// One frame (Ready -> Running on the first). invalid_state outside Ready and Running;
  /// runtime_faulted when a fault stopped the runtime under policy stop; recovery_failed when the
  /// window's max_recoveries were exceeded or the restore was refused; otherwise the frame's
  /// stats (a recovered frame included).
  [[nodiscard]] result<frame_stats> run_frame() noexcept;
  /// Up to `n` frames; returns early (ok) once the runtime left Running through a request, and
  /// with the error of a failing frame.
  [[nodiscard]] result<void> run_frames(std::uint32_t n) noexcept;
  /// Any thread: Running -> Suspended at the next end_frame (clocks frozen).
  void request_suspend() noexcept;
  /// Suspended -> Running; the first delta after it is 0.
  [[nodiscard]] result<void> resume() noexcept;
  /// Any thread: Running -> Stopping at the next end_frame.
  void request_stop() noexcept;
  /// Ready, Running, Suspended or Faulted -> Stopping, then Stopping -> Stopped: cancels pending
  /// jobs (job_cancelled), joins every worker, discards undelivered background results, releases
  /// the world and the frame memory, emits the shutdown runtime.mem event with the leak report.
  /// invalid_state elsewhere; leak_detected (after Stopped) with leak_check and live tracked bytes.
  [[nodiscard]] result<void> shutdown() noexcept;

  // --- swaps (ARCH3-R031) ---

  /// Any thread: queues `request` for the next end_frame (a newer request replaces a pending one).
  void request_swap(swap_request request);
  [[nodiscard]] swap_state swap_status() const noexcept { return swap_state_.load(); }
  /// The code of the latest swap outcome: restart_required, swap_rejected (a failed health
  /// check), the error of apply or of the pre-swap snapshot, ok otherwise.
  [[nodiscard]] errc swap_code() const noexcept { return swap_code_.load(); }

  // --- any-thread reads ---

  [[nodiscard]] rt_state state() const noexcept { return state_.load(); }
  [[nodiscard]] std::optional<fault_report> last_fault() const;
  /// The memory report of the last frame (after shutdown: the shutdown report with its leaks);
  /// empty before start.
  [[nodiscard]] mem::memory_report memory_report() const;
  /// True from the fault_check slot of a faulted frame until the next run_frame: the link skips
  /// the observation flush.
  [[nodiscard]] bool frame_faulted() const noexcept { return frame_faulted_.load(); }
  /// Background results discarded by restores and shutdown.
  [[nodiscard]] std::uint64_t discarded_results() const noexcept { return discarded_.load(); }
  /// Recoveries that restored a snapshot.
  [[nodiscard]] std::uint64_t recoveries() const noexcept { return recoveries_.load(); }

  /// SHA-256 over chunk_bytes, the entity bit layout, the component layout digest, the errc and
  /// failpoint table versions and the ordered_key layout version (ARCH3-R032).
  [[nodiscard]] sl::fabric::digest runtime_abi_digest() const noexcept;
  [[nodiscard]] const runtime_config &config() const noexcept { return config_; }
  [[nodiscard]] std::string_view scope() const noexcept { return scope_; }
  /// The ticks of the snapshots kept for recovery, oldest first (owner thread).
  [[nodiscard]] std::vector<time::tick> snapshot_ticks() const;

  // --- the composed parts (after start; SL_ASSERT otherwise) ---

  [[nodiscard]] ecs::world &world() noexcept;
  [[nodiscard]] frame_scheduler &frames() noexcept;
  [[nodiscard]] jobs::job_system &jobs() noexcept;
  [[nodiscard]] ecs::system_scheduler &systems() noexcept;
  /// The runtime.root tracking resource (host allocations under the runtime tags).
  [[nodiscard]] mem::tracking_resource &root_memory() noexcept;

private:
  // runtime_core.cpp
  [[nodiscard]] bool is_owner() const noexcept { return std::this_thread::get_id() == owner_; }
  [[nodiscard]] result<void> transition(rt_event e) noexcept;
  // transition where the caller already knows the table allows `e` (a refusal changes nothing).
  void apply_event(rt_event e) noexcept;
  [[nodiscard]] time::tick current_tick() const noexcept;
  [[nodiscard]] std::vector<const det::snapshot_contributor *> const_contributors() const;

  // runtime_lifecycle.cpp
  [[nodiscard]] result<void> start_steps() noexcept;
  [[nodiscard]] result<void> memory_root() noexcept;
  [[nodiscard]] result<void> start_jobs() noexcept;
  [[nodiscard]] result<void> start_frames() noexcept;
  void wire_hooks();
  void teardown() noexcept;
  void release_frame_state() noexcept;
  void on_lifecycle_requests() noexcept;
  void on_diagnostics(const boundary_context &bc) noexcept;
  void emit_shutdown_report() noexcept;

  // runtime_fault.cpp
  void on_fatal(const jobs::fatal_report &report) noexcept;
  void discard_after_fatal() noexcept;
  void on_phase_end(phase ph) noexcept;
  void on_fault_check(const boundary_context &bc) noexcept;
  [[nodiscard]] std::optional<fault_report> detect_fault(const boundary_context &bc);
  void publish_fault(const fault_report &report);

  // runtime_recovery.cpp
  [[nodiscard]] result<snapshot> take_snapshot(std::uint64_t frame, time::tick tick);
  void on_snapshot(const boundary_context &bc) noexcept;
  [[nodiscard]] bool over_recovery_limit(time::tick tick) noexcept;
  [[nodiscard]] result<time::tick> restore_newest(time::tick at_or_before) noexcept;
  [[nodiscard]] result<void> restore_snapshot(const snapshot &s) noexcept;
  void discard_background() noexcept;

  // runtime_swap.cpp
  void on_swap(const boundary_context &bc) noexcept;
  void finish_swap(swap_state outcome, errc code) noexcept;

  platform *plat_;
  runtime_config config_;
  std::string scope_; // runtime/<instance_id>
  std::thread::id owner_;
  std::atomic<rt_state> state_{rt_state::created};

  // Registries (create).
  mem::tag_registry tags_;
  type_registry types_;
  ecs::component_registry components_;
  event_registry events_;
  std::vector<ecs::system_desc> staged_systems_;
  std::vector<det::snapshot_contributor *> contributors_;

  // The started parts, in start order (teardown releases them in reverse).
  std::unique_ptr<mem::tracking_resource> root_;
  std::unique_ptr<mem::tracking_resource> world_memory_;
  std::unique_ptr<mem::tracking_resource> frame_memory_;
  std::unique_ptr<mem::diagnostics> diag_;
  time::tick *fault_ticks_ = nullptr; // the root block: the recovery window ring
  std::size_t fault_ring_ = 0;        // its capacity (max_recoveries + 1)
  std::size_t fault_count_ = 0;       // faults recorded in the ring
  std::unique_ptr<jobs::job_system> jobs_;
  std::unique_ptr<mem::frame_arena> arena_;
  std::unique_ptr<frame_scheduler> frames_;
  std::unique_ptr<ecs::world> world_;
  std::unique_ptr<ecs::command_queue> queue_;
  std::unique_ptr<ecs::system_scheduler> systems_;
  sl::fabric::digest schedule_digest_;

  // memory_report: diag_ is published under the mutex once start succeeded.
  mutable std::mutex diag_mutex_;
  bool diag_published_ = false;
  std::optional<mem::memory_report> shutdown_report_;
  std::atomic<std::uint64_t> last_frame_{0};
  time::tick last_tick_ = 0; // owner thread: the tick once frames_ is gone

  // Fault capture.
  std::mutex fatal_mutex_;
  std::vector<jobs::fatal_report> fatal_; // names are static literals
  std::uint64_t failures_seen_ = 0;
  std::vector<errc> errors_seen_;         // last_error per system at the previous fault_check
  std::vector<std::string> system_names_; // by system id (registration order)
  bool playback_oom_ = false;
  std::atomic<bool> frame_faulted_{false};
  errc frame_error_ = errc::ok;
  mutable std::mutex fault_mutex_;
  std::optional<fault_report> last_fault_;

  // Requests (any thread).
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> suspend_requested_{false};

  // Snapshots and recovery (owner thread).
  std::vector<snapshot> snapshots_; // the newest two, oldest first
  time::tick last_snapshot_tick_ = 0;
  std::atomic<std::uint64_t> discarded_{0};
  std::atomic<std::uint64_t> recoveries_{0};

  // Swaps.
  std::mutex swap_mutex_;
  std::optional<swap_request> swap_pending_; // guarded by swap_mutex_
  std::optional<swap_request> swap_active_;  // owner thread: in its health window
  std::optional<snapshot> pre_swap_;
  time::tick swap_tick_ = 0;
  std::atomic<swap_state> swap_state_{swap_state::none};
  std::atomic<errc> swap_code_{errc::ok};
};

/// Hosts only (the library never calls it): std::set_terminate with a handler that writes one
/// fatal sl::log event (system runtime.terminate) and the assert report
/// (sl::default_assert_handler), then aborts.
void install_terminate_report() noexcept;

} // namespace sl::runtime
