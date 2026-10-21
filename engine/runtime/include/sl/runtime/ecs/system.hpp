#pragma once

// System scheduling contracts (WO-094, SL-ARCH-003 ARCH3-R121 to R126, R090, ADR-0024, ADR-0026;
// episode 094). A system declares an access_set: the components its queries read and write, the
// resources it reads and writes by resource_key (an event channel is a resource), structural (it
// records command buffers) and exclusive (it runs alone, on the owner thread, with the whole
// world). Two systems of a phase conflict iff one writes a component or resource the other reads
// or writes, or either is exclusive. build() orders every phase by Kahn's algorithm over the
// before/after constraints with ties broken by registration order; the position is the
// system_index (the ordered_key system_index). A constraint cycle is schedule_cycle naming every
// system on a cycle, an unknown name system_unknown. Each system becomes one frame task whose
// after-edges are its conflicting predecessors in system_index order (transitively reduced, at
// most 8 per task); a phase end hook advances the world change tick, plays the command queue
// back (or discards it in a frame where a system failed) and keeps world::phase_active set while
// system phases run. execution_policy::serial_canonical runs every system of a phase, and every
// par_for_each batch inline in batch order, on the owner thread in ordered_key order: the oracle
// the parallel policy must equal (ARCH3-R090). In Debug and sanitizer builds (NDEBUG undefined)
// the access guard checks every query, resource and command-buffer access of a body against the
// declared set and fails the system with undeclared_access (ARCH3-R125). export_json writes
// sl-runtime-schedule/1 (schemas/runtime/schedule.schema.json), byte-identical across runs and
// worker counts. Sources: src/ecs_system_scheduler.cpp (add, policy, runs, phase hook),
// src/ecs_system_graph.cpp (build), src/ecs_system_export.cpp (export_json),
// src/ecs_system_batches.cpp (inline batches) and src/ecs_access_guard.cpp (the guard).
// Layer L7: includes L0 to L6 runtime headers.
//
// Thread safety (`thread_safety`): system_scheduler add, build, set_policy and export_json are
// owner-thread (the thread that constructed it; add and build return wrong_thread elsewhere);
// set_policy takes effect at the next frame and must not be called during run_frame. A
// system_context is worker-only: valid inside the body it was passed to.

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/command_buffer.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/query.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/replay.hpp"
#include "sl/runtime/time.hpp"

#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace sl::runtime::ecs {

/// `limits` max_systems_per_phase.
inline constexpr std::uint32_t max_systems_per_phase = 256;
static_assert(max_systems_per_phase == max_tasks_per_phase,
              "one frame task per system: the phase limits must agree");

/// True when the access guard is compiled in (Debug and sanitizer builds, NDEBUG undefined).
#ifdef NDEBUG
inline constexpr bool access_guard_enabled = false;
#else
inline constexpr bool access_guard_enabled = true;
#endif

/// What a system touches. add() sorts every list and drops a read that is also a write.
struct access_set {
  component_access components;
  std::vector<resource_key> resource_reads;
  std::vector<resource_key> resource_writes;
  bool structural = false;
  bool exclusive = false;

  friend bool operator==(const access_set &, const access_set &) = default;
};

/// Adds the reads and writes of a query's access set to `into` (sorted and unique).
void add_query_access(access_set &into, const component_access &query_access);

enum class execution_policy : std::uint8_t { parallel, serial_canonical };

class system_scheduler;
class system_context;

/// A noexcept system body invocable on system_context& (ARCH3-R010), held on the heap (a body
/// usually owns its query objects). Move-only; invoked once per phase run.
class system_fn {
public:
  system_fn() noexcept = default;

  template <class F>
    requires(!std::same_as<std::remove_cvref_t<F>, system_fn>)
  // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions,bugprone-forwarding-reference-overload)
  system_fn(F &&f) // cppcheck-suppress noExplicitConstructor ; converts like task_fn
  {
    using T = std::remove_cvref_t<F>;
    static_assert(std::is_nothrow_invocable_v<T &, system_context &>,
                  "a system body must be noexcept-invocable with system_context& (ARCH3-R010)");
    static_assert(std::is_nothrow_constructible_v<T, F &&>,
                  "a system body must be nothrow constructible from its argument");
    body_ = std::make_unique<holder<T>>(std::forward<F>(f));
  }

  [[nodiscard]] explicit operator bool() const noexcept { return body_ != nullptr; }

  /// Runs the body (a no-op when empty).
  void operator()(system_context &ctx) noexcept {
    if (body_ != nullptr) {
      body_->call(ctx);
    }
  }

private:
  struct body {
    body() noexcept = default;
    body(const body &) = delete;
    body(body &&) = delete;
    body &operator=(const body &) = delete;
    body &operator=(body &&) = delete;
    virtual ~body() = default;
    virtual void call(system_context &ctx) noexcept = 0;
  };

  template <class T> struct holder final : body {
    template <class F> explicit holder(F &&f) noexcept : fn(std::forward<F>(f)) {}
    void call(system_context &ctx) noexcept override { fn(ctx); }
    T fn;
  };

  std::unique_ptr<body> body_;
};

/// One system: a unique name (copied), its phase (apply_inputs, pre_tick, tick, post_tick or
/// observe), its declared access, the names of systems of the same phase it runs before and
/// after, its body, whether its par_for_each batches go to the job system (chunked; otherwise
/// they run inline on the system's thread) and the access sets of the queries it iterates (each
/// must lie inside access.components: access_conflict otherwise).
struct system_desc {
  std::string_view name;
  phase ph = phase::tick;
  access_set access;
  std::vector<std::string_view> before;
  std::vector<std::string_view> after;
  system_fn fn;
  bool chunked = false;
  std::vector<component_access> queries;
};

namespace detail {

/// A system as the scheduler keeps it.
struct system_record {
  std::string name;
  phase ph = phase::tick;
  access_set access;
  std::vector<std::string> before;
  std::vector<std::string> after;
  system_fn fn;
  bool chunked = false;
  std::uint32_t id = 0;             // registration order, over every phase
  std::uint16_t index = 0;          // system_index (build)
  std::vector<std::uint16_t> edges; // the system indices its frame task runs after (build)
  std::uint32_t last_run = 0;       // the write tick of its last run (0 before the first)
  errc last_error = errc::ok;       // the code of its latest failure
};

/// The `phases` table name of `ph` ("tick").
[[nodiscard]] std::string_view phase_name(phase ph) noexcept;

/// True when `declared` covers `needed`: every read is a declared read or write, every write a
/// declared write.
[[nodiscard]] bool covers(const component_access &declared,
                          const component_access &needed) noexcept;

/// The argument term kinds of `Terms`, in order.
template <class... Terms> [[nodiscard]] consteval auto argument_kinds() {
  constexpr std::array<term_kind, sizeof...(Terms)> all{Terms::kind...};
  constexpr std::size_t n = (std::size_t{0} + ... + (passes_argument(Terms::kind) ? 1U : 0U));
  std::array<term_kind, n> out{};
  std::size_t j = 0;
  for (const term_kind k : all) {
    if (passes_argument(k)) {
      out[j] = k;
      ++j;
    }
  }
  return out;
}

/// The row `i` of a for_each_chunk span as the matching for_each argument: a reference for read
/// and write, a pointer (null for an absent component) for optional.
template <term_kind K, class S> [[nodiscard]] decltype(auto) element(S s, std::size_t i) noexcept {
  if constexpr (K == term_kind::read || K == term_kind::write) {
    return s[i];
  } else {
    using P = decltype(s.data());
    return s.empty() ? static_cast<P>(nullptr) : s.data() + i;
  }
}

/// Finds the par_for_each batch of each chunk a for_each_chunk run comes from: it walks the
/// archetypes a query matches (required ids present, excluded ids absent) in creation order and
/// their non-empty chunks in order, closing a batch once it holds par_batch_min_rows rows, the
/// partition query::par_for_each uses (ARCH3-R116). Runs must be located in iteration order.
class batch_cursor {
public:
  batch_cursor(const world &w, std::span<const component_id> required,
               std::span<const component_id> excluded) noexcept
      : world_(&w), required_(required), excluded_(excluded) {}

  /// The batch of the chunk whose entity column holds `first`.
  [[nodiscard]] std::uint32_t locate(const entity *first) noexcept;

private:
  [[nodiscard]] bool matches(const archetype &a) const noexcept;

  const world *world_;
  std::span<const component_id> required_;
  std::span<const component_id> excluded_;
  std::uint32_t slot_ = 0;
  std::uint32_t chunk_ = 0;
  std::uint32_t batch_ = 0;
  std::uint32_t rows_in_ = 0;
};

/// The for_each_chunk body of an inline par_for_each: every row of a run calls
/// fn(batch, entity, args...) with the batch of its chunk.
template <class Fn, auto Kinds> struct inline_batch_body {
  Fn *fn = nullptr;
  batch_cursor *cursor = nullptr;

  template <class... Spans>
  void operator()(std::span<const entity> rows, Spans... spans) const noexcept {
    const std::uint32_t batch = cursor->locate(rows.data());
    for (std::size_t i = 0; i < rows.size(); ++i) {
      call(batch, rows[i], i, std::index_sequence_for<Spans...>{}, spans...);
    }
  }

  template <std::size_t... J, class... Spans>
  void call(std::uint32_t batch, entity e, [[maybe_unused]] std::size_t i,
            std::index_sequence<J...> /*seq*/, Spans... spans) const noexcept {
    (*fn)(batch, e, element<Kinds[J]>(spans, i)...);
  }
};

} // namespace detail

// NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members,misc-non-private-member-variables-in-classes)
/// What a system body sees. Worker-only: valid inside the body only. After a guard violation or
/// fail() the system is stopped: iteration visits nothing and resource() returns an error.
class system_context {
public:
  system_context(const system_context &) = delete;
  system_context(system_context &&) = delete;
  system_context &operator=(const system_context &) = delete;
  system_context &operator=(system_context &&) = delete;
  ~system_context() = default;

  world &w;
  /// since = the system's last run tick, write_tick = its tick in this phase run.
  const iter_args &args;
  jobs::job_context &job;
  const time::frame_time &time;

  /// The system's command buffer for this run, keyed {tick, phase, system_index, 0, 0}, opened
  /// at the first call. The guard fails a system that is neither structural nor exclusive.
  [[nodiscard]] command_buffer &commands() noexcept;

  /// The resource under `key`: `T` const needs a declared read or write, `T` non-const a
  /// declared write (undeclared_access from the guard); invalid_state once stopped; the world's
  /// not_found or invalid_argument otherwise.
  template <class T> [[nodiscard]] result<T *> resource(resource_key key) noexcept {
    if (stopped_) {
      return make_error(errc::invalid_state, "system_context::resource: the system stopped");
    }
    if (!allow_resource(key, !std::is_const_v<T>)) {
      return make_error(errc::undeclared_access, "system_context::resource: undeclared access");
    }
    auto found = w.resource<std::remove_const_t<T>>(key);
    if (!found) {
      return std::unexpected(found.error());
    }
    return static_cast<T *>(*found);
  }

  /// query::for_each over the system's world and ticks, after the guard check.
  template <class... Terms, class F> void for_each(query<Terms...> &q, F &&fn) noexcept {
    if (allow_query(q.access())) {
      q.for_each(w, args, std::forward<F>(fn));
    }
  }

  /// query::for_each_chunk over the system's world and ticks, after the guard check.
  template <class... Terms, class F> void for_each_chunk(query<Terms...> &q, F &&fn) noexcept {
    if (allow_query(q.access())) {
      q.for_each_chunk(w, args, std::forward<F>(fn));
    }
  }

  /// query::par_for_each for a chunked system under the parallel policy; otherwise every batch
  /// runs here, in batch order, with the same batch indices. invalid_state once stopped,
  /// undeclared_access from the guard.
  template <class... Terms, class F>
  [[nodiscard]] result<void> par_for_each(query<Terms...> &q, F &&fn) noexcept {
    if (stopped_) {
      return make_error(errc::invalid_state, "system_context::par_for_each: the system stopped");
    }
    if (!allow_query(q.access())) {
      return make_error(errc::undeclared_access, "system_context::par_for_each: undeclared access");
    }
    auto &&body = std::forward<F>(fn);
    if (!inline_batches()) {
      return q.par_for_each(w, jobs(), args, body);
    }
    using Fn = std::remove_reference_t<decltype(body)>;
    constexpr std::size_t n = sizeof...(Terms);
    constexpr std::array<term_kind, n> kinds{Terms::kind...};
    const std::array<result<component_id>, n> found{
        w.registry().id_of<typename Terms::component>()...};
    std::array<component_id, n> required{};
    std::array<component_id, n> excluded{};
    std::size_t nr = 0;
    std::size_t ne = 0;
    for (std::size_t i = 0; i < n; ++i) {
      if (!found[i]) {
        return make_error(errc::invalid_argument, "system_context::par_for_each: foreign query");
      }
      if (kinds[i] == term_kind::without) {
        excluded[ne++] = *found[i];
      } else if (kinds[i] != term_kind::optional_read && kinds[i] != term_kind::optional_write) {
        required[nr++] = *found[i];
      }
    }
    detail::batch_cursor cursor(w, std::span<const component_id>(required).first(nr),
                                std::span<const component_id>(excluded).first(ne));
    q.for_each_chunk(w, args,
                     detail::inline_batch_body<Fn, detail::argument_kinds<Terms...>()>{
                         .fn = &body, .cursor = &cursor});
    return {};
  }

  /// The system's position in its phase's order (the ordered_key system_index).
  [[nodiscard]] std::uint16_t system_index() const noexcept;
  [[nodiscard]] phase current_phase() const noexcept;
  /// The logical tick the phase runs for (the ordered_key tick).
  [[nodiscard]] std::uint64_t tick() const noexcept { return tick_; }
  /// The job system (for par_for_each or nested jobs).
  [[nodiscard]] jobs::job_system &jobs() const noexcept;
  /// Fails the system with `code` (job_context::fail) and stops it; the frame's command buffers
  /// are discarded at the next phase boundary.
  void fail(errc code) noexcept;
  [[nodiscard]] bool stopped() const noexcept { return stopped_; }

private:
  friend class system_scheduler;

  system_context(system_scheduler &scheduler, detail::system_record &record, world &target,
                 const iter_args &ticks, jobs::job_context &context,
                 const time::frame_time &frame_time, std::uint64_t tick) noexcept
      : w(target), args(ticks), job(context), time(frame_time), scheduler_(&scheduler),
        record_(&record), tick_(tick) {}

  // ecs_access_guard.cpp
  [[nodiscard]] bool allow_query(const component_access &needed) noexcept;
  [[nodiscard]] bool allow_resource(resource_key key, bool write) noexcept;
  void check_commands() noexcept;
  void violation() noexcept;

  [[nodiscard]] bool inline_batches() const noexcept;

  system_scheduler *scheduler_;
  detail::system_record *record_;
  command_buffer *commands_ = nullptr;
  std::uint64_t tick_ = 0;
  bool stopped_ = false;
};
// NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members,misc-non-private-member-variables-in-classes)

class system_scheduler {
public:
  /// A scheduler whose systems run on `jobs` (which must outlive it). The calling thread becomes
  /// the owner thread; it must be the job system's and the frame scheduler's owner.
  explicit system_scheduler(jobs::job_system &jobs) noexcept;
  system_scheduler(const system_scheduler &) = delete;
  system_scheduler(system_scheduler &&) = delete;
  system_scheduler &operator=(const system_scheduler &) = delete;
  system_scheduler &operator=(system_scheduler &&) = delete;
  ~system_scheduler() = default;

  /// Registers a system and returns its id (from 0, in registration order). wrong_thread off the
  /// owner thread; schedule_frozen after build; invalid_argument for a phase without systems, an
  /// empty name or body, or a 257th system of one phase; already_exists for a used name;
  /// access_conflict when one of desc.queries needs access desc.access lacks.
  [[nodiscard]] result<std::uint32_t> add(system_desc desc) noexcept;
  /// Orders every phase, registers one frame task per system and the phase end hook on
  /// `frames` (not yet built), and binds `w` and `commands` (all must outlive the scheduler).
  /// wrong_thread; system_unknown for a before or after name that is no system of the phase;
  /// schedule_cycle naming every system on a constraint cycle; the frame scheduler's error from
  /// add_task. Nothing is registered unless every phase orders. A second call returns ok.
  [[nodiscard]] result<void> build(frame_scheduler &frames, world &w,
                                   command_queue &commands) noexcept;
  /// Selects the policy of the next frames (owner thread, between frames).
  void set_policy(execution_policy next) noexcept;
  [[nodiscard]] execution_policy policy() const noexcept { return policy_; }
  /// sl-runtime-schedule/1: the policy and, for every phase with systems, its systems in
  /// system_index order with their access and after-edges (empty phases before build).
  [[nodiscard]] std::string export_json() const;

  [[nodiscard]] std::size_t size() const noexcept { return systems_.size(); }
  [[nodiscard]] bool built() const noexcept { return built_; }
  /// The system_index of system `id`: invalid_state before build, not_found for an unknown id.
  [[nodiscard]] result<std::uint16_t> system_index(std::uint32_t id) const noexcept;
  /// The ids of `ph`'s systems in system_index order (empty before build).
  [[nodiscard]] std::span<const std::uint32_t> order(phase ph) const noexcept;
  /// The system indices the frame task of system `id` runs after (empty before build).
  [[nodiscard]] std::span<const std::uint16_t> edges(std::uint32_t id) const noexcept;
  /// The outcome of the latest phase-boundary playback (job_failed when it was discarded).
  [[nodiscard]] const result<flush_report> &last_flush() const noexcept { return last_flush_; }
  /// Systems failed since construction (fail, the guard or RFP5).
  [[nodiscard]] std::uint64_t failures() const noexcept { return failures_.load(); }
  /// The code of system `id`'s latest failure (undeclared_access from the guard, job_failed from
  /// RFP5, the code given to fail); ok when it never failed or for an unknown id. Read it between
  /// frames.
  [[nodiscard]] errc last_error(std::uint32_t id) const noexcept;

private:
  friend class system_context;

  // The frame task of one system.
  struct system_task {
    system_scheduler *self = nullptr;
    std::uint32_t slot = 0;
    void operator()(task_context &ctx) const noexcept;
  };

  // A main-affinity job running one system on the owner thread.
  struct owner_job {
    system_scheduler *self = nullptr;
    const time::frame_time *time = nullptr;
    std::uint64_t tick = 0;
    std::uint32_t slot = 0;
    void operator()(jobs::job_context &ctx) const noexcept;
  };

  // The phase end hook.
  struct phase_hook {
    system_scheduler *self = nullptr;
    void operator()(phase ph, time::tick tick) const noexcept;
  };

  [[nodiscard]] bool is_owner() const noexcept { return std::this_thread::get_id() == owner_; }

  // ecs_system_scheduler.cpp
  void run_task(std::uint32_t slot, task_context &ctx) noexcept;
  void run_on_owner(std::uint32_t slot, task_context &ctx) noexcept;
  void run_serial_phase(phase ph, task_context &ctx) noexcept;
  void run_system(std::uint32_t slot, jobs::job_context &job, const time::frame_time &time,
                  std::uint64_t tick) noexcept;
  void end_phase(phase ph) noexcept;
  void note_failure(detail::system_record &s, errc code) noexcept;

  // ecs_system_graph.cpp
  [[nodiscard]] result<std::vector<std::uint32_t>> order_phase(phase ph) const;
  [[nodiscard]] bool conflict(std::uint32_t a, std::uint32_t b) const noexcept;
  void plan_edges(std::span<const std::uint32_t> ordered);

  jobs::job_system *jobs_ = nullptr;
  std::thread::id owner_;
  world *world_ = nullptr;
  command_queue *queue_ = nullptr;
  execution_policy policy_ = execution_policy::parallel;
  bool built_ = false;
  std::vector<detail::system_record> systems_;                // registration order
  std::array<std::vector<std::uint32_t>, phase_count> order_; // ids by system_index (build)
  std::atomic<std::uint64_t> failures_{0};
  std::atomic<bool> frame_failed_{false};
  result<flush_report> last_flush_;
};

} // namespace sl::runtime::ecs
