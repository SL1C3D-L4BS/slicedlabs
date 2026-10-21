#pragma once

// The job system (WO-082, SL-ARCH-003 ARCH3-R030, R061 to R071, ADR-0024; episode 086). A job is
// a job_desc: a noexcept callable stored inline in 48 bytes, a priority (frame or background), an
// affinity (any or main) and a static debug name. Jobs live in a bounded pool of 128-byte records
// aligned to 64, addressed by generation-checked job_handle values; dependencies are earlier jobs,
// job_counter or cpu_fence values (at most 8 per submit), so a cycle cannot be expressed. Waits
// help (a waiting worker or the owner thread runs other ready jobs; the owner thread also runs
// main-affinity jobs); a submit at capacity helps until a record frees. job_context::fail marks
// the job failed and cancels its transitive dependents; fail_fatal also calls on_fatal. A job runs
// under its submitter's sl::log correlation. Background results reach the owner thread only
// through drain_results, in submission order. In SL_RUNTIME_FAULTS builds a watchdog reports
// deadlock_suspected (src/job_system_watchdog.cpp). Sources: src/job_system.cpp (submit, run,
// wait, stop), src/job_system_records.cpp (record pool, dependents), src/job_system_watchdog.cpp.
// Layer L3: includes L0 to L2 runtime headers and work_queue.hpp.
//
// Thread safety (`thread_safety`): start, stop, drain_results, run_main_affinity and quiesce are
// owner-thread (the thread that called start; stop returns wrong_thread elsewhere, the void calls
// are a Debug assertion); submit, try_submit, wait, status, stats, worker_count and
// current_job_priority are any-thread; job_context members are worker-only (inside a job body).

#include "sl/core/error.hpp"
#include "sl/core/log.hpp"
#include "sl/runtime/platform.hpp"
#include "sl/runtime/sync.hpp"
#include "sl/runtime/work_queue.hpp"

#include <array>
#include <atomic>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace sl::runtime::jobs {

// SL-ARCH-003 `limits` (ARCH3-R174).
inline constexpr std::uint32_t job_capacity_default = 65536;
inline constexpr std::size_t job_record_bytes = 128;
inline constexpr std::size_t job_record_alignment = 64;
inline constexpr std::size_t job_inline_payload_bytes = 48;
inline constexpr std::size_t job_payload_alignment = 16;
inline constexpr std::size_t job_max_dependencies = 8;
inline constexpr std::uint32_t job_nested_wait_depth = 64;
inline constexpr std::uint32_t max_workers = 64;
inline constexpr std::uint32_t watchdog_ms_default = 5000;
inline constexpr std::uint32_t aging_limit = 64;
/// The watchdog's check period.
inline constexpr std::uint32_t watchdog_period_ms = 100;
/// The largest job_config::capacity start accepts.
inline constexpr std::uint32_t job_capacity_max = 1U << 24U;
/// The longest RFP4 stall in microseconds (`failpoints`: 0-2 ms).
inline constexpr std::uint32_t worker_stall_max_us = 2000;
/// job_context::worker() of a job run inline on a thread that is neither a worker nor the owner.
inline constexpr std::uint32_t external_thread = 0xFFFFFFFFU;

enum class priority : std::uint8_t { frame, background };
enum class affinity : std::uint8_t { any, main };
enum class job_status : std::uint8_t { pending, running, done, failed, cancelled };

/// A job: index into the record pool and the record's generation (from 1). The null handle {0, 0}
/// names no live job: its status is stale_job and waiting on it returns at once.
struct job_handle {
  std::uint32_t index = 0;
  std::uint32_t generation = 0;

  friend bool operator==(const job_handle &, const job_handle &) noexcept = default;
};

class job_system;

/// What a job body sees. Worker-only: valid only inside the body it was passed to.
class job_context {
public:
  job_context(const job_context &) = delete;
  job_context(job_context &&) = delete;
  job_context &operator=(const job_context &) = delete;
  job_context &operator=(job_context &&) = delete;
  ~job_context() = default;

  /// The worker index k (slrt<instance>-w<k>), worker_count() on the owner thread, or
  /// external_thread.
  [[nodiscard]] std::uint32_t worker() const noexcept { return worker_; }
  [[nodiscard]] bool background() const noexcept { return prio_ == priority::background; }
  /// Marks the job failed with `code` (the first call wins; errc::ok counts as job_failed); the
  /// body keeps running, its dependents are cancelled when it returns.
  void fail(errc code) noexcept;
  /// fail(code), and job_config::on_fatal receives a fatal_report after the body returns.
  void fail_fatal(errc code) noexcept;
  /// Appends one result for drain_results (background jobs only; invalid_state otherwise).
  [[nodiscard]] result<void> post_result(std::uint32_t kind,
                                         std::span<const std::byte> bytes) noexcept;

private:
  friend class job_system;
  job_context(job_system &system, std::uint32_t worker, priority prio, std::uint64_t seq) noexcept
      : system_(&system), worker_(worker), prio_(prio), seq_(seq) {}

  job_system *system_;
  std::uint32_t worker_;
  priority prio_;
  std::uint64_t seq_;
  errc code_ = errc::ok;
  bool fatal_ = false;
};

/// A type-erased job body stored inline: a callable F with F& invocable as noexcept on
/// job_context&, at most 48 bytes, aligned at most 16 and nothrow movable. Move-only.
class job_fn {
public:
  job_fn() noexcept = default;

  template <class F>
    requires(!std::same_as<std::remove_cvref_t<F>, job_fn>)
  // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions,bugprone-forwarding-reference-overload)
  job_fn(F &&f) noexcept {
    using T = std::remove_cvref_t<F>;
    static_assert(std::is_nothrow_invocable_v<T &, job_context &>,
                  "a job body must be noexcept-invocable with job_context& (ARCH3-R061)");
    static_assert(sizeof(T) <= job_inline_payload_bytes,
                  "a job body must fit the 48-byte inline payload (ARCH3-R061)");
    static_assert(alignof(T) <= job_payload_alignment, "a job body must be aligned at most 16");
    static_assert(std::is_nothrow_move_constructible_v<T>, "a job body must be nothrow movable");
    static_assert(std::is_nothrow_constructible_v<T, F &&>,
                  "a job body must be nothrow constructible from its argument");
    ::new (static_cast<void *>(storage_.data())) T(std::forward<F>(f));
    ops_ = &ops_for<T>;
  }

  job_fn(job_fn &&other) noexcept : ops_(other.ops_) {
    if (ops_ != nullptr) {
      ops_->relocate(storage_.data(), other.storage_.data());
      other.ops_ = nullptr;
    }
  }

  job_fn &operator=(job_fn &&other) noexcept {
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

  job_fn(const job_fn &) = delete;
  job_fn &operator=(const job_fn &) = delete;
  ~job_fn() { reset(); }

  [[nodiscard]] explicit operator bool() const noexcept { return ops_ != nullptr; }

  /// Runs the body (a no-op when empty).
  void operator()(job_context &ctx) noexcept {
    if (ops_ != nullptr) {
      ops_->invoke(storage_.data(), ctx);
    }
  }

private:
  friend class job_system;

  struct fn_ops {
    void (*invoke)(void *self, job_context &ctx) noexcept;
    void (*relocate)(void *dst, void *src) noexcept; // move-constructs dst, destroys src
    void (*destroy)(void *self) noexcept;
  };

  // Named functions rather than captureless lambdas: gcc -fanalyzer models a lambda's function
  // pointer conversion as a call of operator() on a null closure (WO-107).
  template <class T> static void invoke_as(void *self, job_context &ctx) noexcept {
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

  alignas(job_payload_alignment) std::array<std::byte, job_inline_payload_bytes> storage_{};
  const fn_ops *ops_ = nullptr;
};

/// One job to submit. `name` must have static storage duration (a literal).
struct job_desc {
  job_fn fn;
  priority prio = priority::frame;
  affinity aff = affinity::any;
  std::string_view name;
};

/// A predecessor of a submit: an earlier job, a job_counter (satisfied at zero) or a cpu_fence
/// (satisfied when released). Counters and fences are borrowed and must outlive the dependent.
struct dependency {
  enum class kind : std::uint8_t { job, counter, fence };

  [[nodiscard]] static dependency on(job_handle handle) noexcept {
    return dependency{kind::job, handle, nullptr, nullptr};
  }
  [[nodiscard]] static dependency on(const job_counter &counter) noexcept {
    return dependency{kind::counter, {}, &counter, nullptr};
  }
  [[nodiscard]] static dependency on(const cpu_fence &fence) noexcept {
    return dependency{kind::fence, {}, nullptr, &fence};
  }

  kind what = kind::job;
  job_handle job;
  const job_counter *counter = nullptr;
  const cpu_fence *fence = nullptr;
};

/// What on_fatal receives after a body called fail_fatal (or RFP3 fired).
struct fatal_report {
  errc code = errc::job_failed;
  job_handle job;
  std::string_view name;
  std::uint32_t worker = 0;
};

/// One background result as drain_results delivers it: the submit sequence number of the job
/// that posted it, the post's index within that job, the kind and the bytes.
struct background_result {
  std::uint64_t job_seq = 0;
  std::uint32_t post_index = 0;
  std::uint32_t kind = 0;
  std::vector<std::byte> bytes;
};

/// `limits` workers_default: hardware_concurrency - 1 - reserved_threads, at least 1.
[[nodiscard]] std::uint32_t default_workers(std::uint32_t reserved_threads = 0) noexcept;

struct job_config {
  /// Worker threads (at most max_workers); 0 runs every job on the owner thread.
  std::uint32_t workers = default_workers();
  /// Job records (1 to job_capacity_max).
  std::uint32_t capacity = job_capacity_default;
  /// No completion for this long with jobs pending reports deadlock_suspected (SL_RUNTIME_FAULTS
  /// builds; 0 disables the watchdog).
  std::uint32_t watchdog_ms = watchdog_ms_default;
  /// Named in the watchdog report; thread names take the platform's instance id.
  std::uint32_t instance_id = 1;
  /// Builds each worker's local queue (capacity records).
  queue_factory make_queue = make_locked_queue;
  /// Null selects the reference policy.
  std::unique_ptr<scheduler_policy> policy;
  /// Called on the worker after a body called fail_fatal.
  std::function<void(const fatal_report &)> on_fatal;
  std::uint64_t seed = 0;
};

/// Counters since start (any-thread; each value is read on its own).
struct job_stats {
  std::uint64_t submitted = 0;
  std::uint64_t completed = 0; // done, failed or cancelled
  std::uint64_t failed = 0;
  std::uint64_t cancelled = 0;
  std::uint64_t fatal = 0;
  std::uint64_t inline_runs = 0;    // jobs without predecessors run on the caller at capacity
  std::uint64_t capacity_waits = 0; // submits that found no free record
  std::uint64_t rejected = 0;       // submit() calls refused (stopped or invalid)
  std::uint64_t pending = 0;        // submitted and not yet completed
};

class job_system {
public:
  /// Validates `config` (workers <= 64, capacity 1 to job_capacity_max: invalid_argument), builds
  /// the queues (the factory's error is returned), spawns the workers and, in SL_RUNTIME_FAULTS
  /// builds, the watchdog through platform.threads (thread_spawn_failed unwinds every thread
  /// already started). The calling thread becomes the owner thread.
  [[nodiscard]] static result<std::unique_ptr<job_system>> start(platform &plat,
                                                                 job_config config) noexcept;

  job_system(const job_system &) = delete;
  job_system(job_system &&) = delete;
  job_system &operator=(const job_system &) = delete;
  job_system &operator=(job_system &&) = delete;
  /// Stops (as stop()) when stop() was not called.
  ~job_system();

  /// try_submit, except at capacity: helps until a record frees, or runs a job without
  /// predecessors inline when no ready job exists (returning the null handle). A refused submit
  /// (invalid_argument, scheduler_stopped) returns the null handle and counts in stats().rejected.
  job_handle submit(job_desc desc, std::span<const dependency> deps = {},
                    job_counter *done = nullptr) noexcept;
  /// Submits `desc` after `deps` (at most 8: invalid_argument); `done` (when not null) is added
  /// to now and subtracted when the job completes. job_capacity when no record is free,
  /// scheduler_stopped after stop().
  [[nodiscard]] result<job_handle> try_submit(job_desc desc, std::span<const dependency> deps = {},
                                              job_counter *done = nullptr) noexcept;

  /// Returns once the job completed, helping meanwhile: ok when done (or the handle is stale),
  /// job_failed (the message names the job's code) or job_cancelled.
  [[nodiscard]] result<void> wait(job_handle handle) noexcept;
  /// Returns once `counter` is zero, helping meanwhile.
  [[nodiscard]] result<void> wait(const job_counter &counter) noexcept;
  /// The job's status; stale_job when the handle's generation is not the record's.
  [[nodiscard]] result<job_status> status(job_handle handle) const noexcept;
  [[nodiscard]] job_stats stats() const noexcept;

  /// Owner thread: runs every main-affinity job that is ready now.
  void run_main_affinity() noexcept;
  /// Owner thread: delivers the results of completed background jobs in submission order; the
  /// results of a job wait until every background job submitted before it completed.
  void drain_results(const std::function<void(const background_result &)> &deliver);
  /// Owner thread: helps until no job is pending, then quiesces every queue.
  void quiesce() noexcept;
  /// Owner thread (wrong_thread elsewhere): refuses new submits, cancels every job that has not
  /// started, waits for running jobs and joins the threads. A second call returns ok.
  [[nodiscard]] result<void> stop() noexcept;
  [[nodiscard]] std::uint32_t worker_count() const noexcept { return workers_; }
  /// The priority of the job running on the calling thread, nullopt outside a job.
  [[nodiscard]] std::optional<priority> current_job_priority() const noexcept;

private:
  friend class job_context;

  static constexpr std::uint32_t no_link = 0xFFFFFFFFU;
  static constexpr std::uint8_t no_priority = 0xFFU;
  static constexpr std::size_t fixed_dependents = 4;
  static constexpr std::size_t stripe_count = 64;

  // One job record: 128 bytes aligned to 64 (`limits` job_record_bytes).
  struct alignas(job_record_alignment) record {
    alignas(job_payload_alignment) std::array<std::byte, job_inline_payload_bytes> payload{};
    const job_fn::fn_ops *ops = nullptr;
    std::atomic<const char *> name_ptr{nullptr}; // read by the watchdog: generation-checked
    const char *scope_ptr = nullptr;             // the submitter's correlation
    std::uint64_t corr_id = 0;
    std::uint64_t corr_tick = 0;
    std::uint32_t index = 0;
    std::atomic<std::uint32_t> generation{0}; // 0 until first use, then from 1
    std::atomic<std::uint32_t> remaining{0};  // unfinished predecessors + the submit guard
    std::atomic<std::uint16_t> name_len{0};
    std::uint16_t scope_len = 0;
    std::atomic<job_status> status{job_status::done};
    priority prio = priority::frame;
    affinity aff = affinity::any;
    std::uint8_t dependents = 0;      // used entries of fixed (under the stripe lock)
    std::uint32_t overflow = no_link; // head of the overflow chain (under the stripe lock)
    std::array<std::uint32_t, fixed_dependents> fixed{};
  };
  static_assert(sizeof(record) == job_record_bytes);
  static_assert(alignof(record) == job_record_alignment);

  // Per-record data off the hot line: the done counter, the submit sequence, the outcome and the
  // overflow links (one per dependency slot of this job: a pool of capacity x 8 links).
  struct record_cold {
    job_counter *done = nullptr;
    std::uint64_t seq = 0;
    std::atomic<std::uint32_t> error{0};
    std::atomic<bool> cancel{false};
    std::array<std::uint32_t, job_max_dependencies> next{};
  };

  // A thread that may run jobs: worker k at index k, the owner thread at index workers_.
  struct alignas(job_record_alignment) thread_slot {
    std::atomic<std::thread::id> id{};
    std::unique_ptr<work_queue> local; // null for the owner
    std::atomic<std::int64_t> depth{0};
    std::atomic<std::uint64_t> current{0}; // packed handle of the running job, 0 none
    std::atomic<std::uint64_t> waiting{0}; // packed handle of the wait target, 0 none
    std::uint8_t prio = no_priority;       // the slot's own thread only
    std::uint64_t rounds = 0;
    std::uint64_t local_pops = 0;
    std::uint32_t since_background = 0;
  };

  // A job held until its counter and fence predecessors are satisfied.
  struct gate {
    std::uint32_t index = 0;
    std::uint32_t count = 0;
    std::array<dependency, job_max_dependencies> deps{};
  };

  // The results of one background job, in submission order.
  struct background_entry {
    std::uint64_t seq = 0;
    bool done = false;
    std::vector<background_result> results;
  };

  job_system(platform &plat, job_config &config) noexcept;

  // job_system.cpp
  [[nodiscard]] result<job_handle> submit_impl(job_desc &desc, std::span<const dependency> deps,
                                               job_counter *done, bool may_help) noexcept;
  void run_inline(job_desc &desc, job_counter *done, thread_slot *slot) noexcept;
  [[nodiscard]] std::optional<std::uint32_t> take(thread_slot &slot) noexcept;
  [[nodiscard]] bool poll_gates(thread_slot *slot) noexcept;
  void run_job(std::uint32_t index, thread_slot &slot) noexcept;
  void finish(std::uint32_t index, job_status final_status, errc code, thread_slot *slot) noexcept;
  void make_ready(std::uint32_t index, thread_slot *slot) noexcept;
  void release_one(std::uint32_t index, bool cancel, thread_slot *slot) noexcept;
  template <class Done> void help_until(Done done, const job_counter *counter) noexcept;
  void worker_main(std::uint32_t k) noexcept;
  void shutdown() noexcept;
  void post(std::uint64_t seq, std::uint32_t kind, std::span<const std::byte> bytes) noexcept;
  void background_done(std::uint64_t seq) noexcept;
  [[nodiscard]] std::uint64_t register_background() noexcept;
  void report_fatal(const job_context &ctx, job_handle handle, std::string_view name) noexcept;
  [[nodiscard]] thread_slot *find_slot() noexcept;
  [[nodiscard]] std::uint32_t slot_index(const thread_slot *slot) const noexcept;
  [[nodiscard]] bool is_owner() const noexcept;

  // job_system_records.cpp
  [[nodiscard]] result<void> init_records() noexcept;
  [[nodiscard]] std::optional<std::uint32_t> allocate_record() noexcept;
  void free_record(std::uint32_t index) noexcept;
  enum class attach_result : std::uint8_t { attached, satisfied, cancel };
  [[nodiscard]] attach_result attach(job_handle pred, std::uint32_t dependent,
                                     std::uint32_t slot) noexcept;
  void release_dependents(std::uint32_t index, job_status final_status, thread_slot *slot) noexcept;
  [[nodiscard]] std::string_view record_name(std::uint64_t packed) const noexcept;
  [[nodiscard]] std::mutex &stripe(std::uint32_t index) noexcept {
    return stripes_[index % stripe_count];
  }
  [[nodiscard]] static std::uint64_t pack(job_handle h) noexcept {
    return (std::uint64_t{h.generation} << 32U) | h.index;
  }

  // job_system_watchdog.cpp
  [[nodiscard]] result<void> start_watchdog() noexcept;
  void stop_watchdog() noexcept;
  void watchdog_main() noexcept;
  void watchdog_report(std::uint64_t silent_ms) noexcept;

  platform *platform_;
  std::uint32_t workers_;
  std::uint32_t capacity_;
  std::uint32_t watchdog_ms_;
  std::uint32_t instance_id_;
  queue_factory make_queue_;
  std::unique_ptr<scheduler_policy> policy_;
  std::function<void(const fatal_report &)> on_fatal_;
  std::uint64_t seed_;
  std::thread::id owner_;

  std::unique_ptr<record[]> records_;
  std::unique_ptr<record_cold[]> cold_;
  std::mutex free_mutex_;
  std::unique_ptr<std::uint32_t[]> free_ring_;
  std::size_t free_head_ = 0;
  std::size_t free_tail_ = 0;
  std::array<std::mutex, stripe_count> stripes_;

  std::vector<thread_slot> slots_;
  std::unique_ptr<locked_deque> frame_queue_;
  std::unique_ptr<locked_deque> background_queue_;
  std::unique_ptr<locked_deque> main_queue_;
  event_count events_;

  std::mutex gate_mutex_;
  std::vector<gate> gates_;
  std::atomic<std::uint32_t> gated_{0};

  std::mutex results_mutex_;
  std::deque<background_entry> background_;

  std::atomic<bool> stopping_{false};
  std::atomic<bool> exiting_{false};
  bool stopped_ = false;
  std::atomic<std::uint64_t> seq_{0};
  std::atomic<std::uint64_t> pending_{0};
  std::atomic<std::uint64_t> completions_{0};
  std::atomic<std::uint64_t> submitted_{0};
  std::atomic<std::uint64_t> failed_{0};
  std::atomic<std::uint64_t> cancelled_{0};
  std::atomic<std::uint64_t> fatal_{0};
  std::atomic<std::uint64_t> inline_runs_{0};
  std::atomic<std::uint64_t> capacity_waits_{0};
  std::atomic<std::uint64_t> rejected_{0};

  thread_group threads_;
  std::mutex watchdog_mutex_;
  std::condition_variable watchdog_cv_;
  bool watchdog_exit_ = false;
  thread_handle watchdog_;
};

} // namespace sl::runtime::jobs
