// The job system (WO-082, SL-ARCH-003 ARCH3-R030, R061 to R071, ADR-0024; episode 086): submit,
// the worker loop, helping waits, failure cancellation, correlation propagation, background
// results and stop. Every atomic operation is seq_cst (no explicit memory order). Queues: each
// worker's local queue comes from job_config::make_queue; the frame and background injection
// queues and the main-affinity queue are locked_deques (multi-producer). A job becomes ready when
// its remaining count (predecessors plus the submit guard) reaches zero; it is pushed to the
// main queue (affinity main), the background injection queue (priority background), the local
// queue of a worker that released it, or the frame injection queue. A cancelled job (a failed or
// cancelled predecessor, or stop) is completed as cancelled by whoever takes it, without running.
// Every thread that waits for work parks on one event_count after spin_limit empty rounds, and
// every push and completion calls notify_all, so a park never misses work (ARCH3-R060).

#include "sl/core/assert.hpp"
#include "sl/core/log.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/faults.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/sync.hpp"
#include "sl/runtime/work_queue.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace sl::runtime::jobs {

namespace {

constexpr bool is_final(job_status s) noexcept {
  return s == job_status::done || s == job_status::failed || s == job_status::cancelled;
}

constexpr std::uint16_t clamp16(std::size_t n) noexcept {
  return static_cast<std::uint16_t>(std::min<std::size_t>(n, 0xFFFFU));
}

result<std::unique_ptr<locked_deque>> make_shared_queue(std::size_t capacity) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays): a nothrow ring
  std::unique_ptr<std::uint32_t[]> cells(new (std::nothrow) std::uint32_t[capacity]);
  if (!cells) {
    return make_error(errc::out_of_memory, "job_system::start: injection queue allocation failed");
  }
  std::unique_ptr<locked_deque> queue(new (std::nothrow) locked_deque(std::move(cells), capacity));
  if (!queue) {
    return make_error(errc::out_of_memory, "job_system::start: injection queue allocation failed");
  }
  return queue;
}

// RFP4 (worker.stall): a bounded stall at before_pop, after_steal, before_complete and in_wait.
void stall() noexcept {
#if SL_RUNTIME_FAULTS
  const fault_action action = visit(failpoint::rfp4);
  if (action.fire) {
    std::this_thread::sleep_for(
        std::chrono::microseconds(std::min(action.param, worker_stall_max_us)));
  }
#endif
}

// RFP2 (job.error) and RFP3 (job.fatal), visited before a body runs; true when the body must be
// skipped because one fired.
bool fault_before_body(job_context &ctx) noexcept {
#if SL_RUNTIME_FAULTS
  if (visit(failpoint::rfp2).fire) {
    ctx.fail(errc::job_failed);
    return true;
  }
  if (visit(failpoint::rfp3).fire) {
    ctx.fail_fatal(errc::job_failed);
    return true;
  }
#else
  static_cast<void>(ctx);
#endif
  return false;
}

} // namespace

std::uint32_t default_workers(std::uint32_t reserved_threads) noexcept {
  const std::uint64_t hardware = std::thread::hardware_concurrency();
  const std::uint64_t used = 1U + std::uint64_t{reserved_threads};
  if (hardware <= used) {
    return 1;
  }
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(hardware - used, max_workers));
}

// --- job_context ---

void job_context::fail(errc code) noexcept {
  if (code_ == errc::ok) {
    code_ = code == errc::ok ? errc::job_failed : code;
  }
}

void job_context::fail_fatal(errc code) noexcept {
  fail(code);
  fatal_ = true;
}

result<void> job_context::post_result(std::uint32_t kind,
                                      std::span<const std::byte> bytes) noexcept {
  if (prio_ != priority::background) {
    return make_error(errc::invalid_state, "job_context::post_result from a frame job");
  }
  system_->post(seq_, kind, bytes);
  return {};
}

// --- start and stop ---

job_system::job_system(platform &plat, job_config &config) noexcept
    : platform_(&plat), workers_(config.workers), capacity_(config.capacity),
      watchdog_ms_(config.watchdog_ms), instance_id_(config.instance_id),
      make_queue_(std::move(config.make_queue)),
      policy_(config.policy ? std::move(config.policy) : make_reference_policy()),
      on_fatal_(std::move(config.on_fatal)), seed_(config.seed), slots_(config.workers + 1U) {}

result<std::unique_ptr<job_system>> job_system::start(platform &plat, job_config config) noexcept {
  if (config.workers > max_workers) {
    return make_error(errc::invalid_argument, "job_system::start: more than 64 workers");
  }
  if (config.capacity == 0 || config.capacity > job_capacity_max) {
    return make_error(errc::invalid_argument, "job_system::start: capacity outside 1..2^24");
  }
  if (!config.make_queue) {
    return make_error(errc::invalid_argument, "job_system::start: no queue factory");
  }
  std::unique_ptr<job_system> sys(new (std::nothrow) job_system(plat, config));
  if (!sys || !sys->policy_) {
    return make_error(errc::out_of_memory, "job_system::start: allocation failed");
  }
  if (auto r = sys->init_records(); !r) {
    return std::unexpected(std::move(r.error()));
  }
  auto frame = make_shared_queue(sys->capacity_);
  auto background = make_shared_queue(sys->capacity_);
  auto main = make_shared_queue(sys->capacity_);
  if (!frame || !background || !main) {
    return make_error(errc::out_of_memory, "job_system::start: queue allocation failed");
  }
  sys->frame_queue_ = std::move(*frame);
  sys->background_queue_ = std::move(*background);
  sys->main_queue_ = std::move(*main);
  for (std::uint32_t k = 0; k < sys->workers_; ++k) {
    auto queue = sys->make_queue_(sys->capacity_);
    if (!queue) {
      return std::unexpected(std::move(queue.error()));
    }
    if (!*queue) {
      return make_error(errc::invalid_argument, "job_system::start: make_queue returned null");
    }
    sys->slots_[k].local = std::move(*queue);
  }
  sys->gates_.reserve(64);
  sys->owner_ = std::this_thread::get_id();
  sys->slots_[sys->workers_].id.store(sys->owner_);
  sys->policy_->on_start(sys->workers_, sys->seed_);
  for (std::uint32_t k = 0; k < sys->workers_; ++k) {
    job_system *self = sys.get();
    auto spawned = sys->threads_.spawn(plat.threads, "w" + std::to_string(k),
                                       [self, k] noexcept { self->worker_main(k); });
    if (!spawned) {
      return std::unexpected(std::move(spawned.error())); // ~job_system joins the started ones
    }
  }
  if (auto r = sys->start_watchdog(); !r) {
    return std::unexpected(std::move(r.error()));
  }
  return sys;
}

job_system::~job_system() {
  if (!stopped_) {
    shutdown();
  }
}

result<void> job_system::stop() noexcept {
  if (!is_owner()) {
    return make_error(errc::wrong_thread, "job_system::stop off the owner thread");
  }
  if (!stopped_) {
    shutdown();
  }
  return {};
}

void job_system::shutdown() noexcept {
  stopping_.store(true);
  events_.notify_all();
  if (records_ && frame_queue_ && background_queue_ && main_queue_) {
    // Every job not yet started is taken and completed as cancelled (run_job sees stopping_);
    // the caller acts as the owner thread so main-affinity jobs drain too.
    thread_slot &me = slots_[workers_];
    spin_wait spinner(sl::runtime::spin_limit);
    while (pending_.load() != 0) {
      if (auto index = take(me)) {
        run_job(*index, me);
        spinner.reset();
        continue;
      }
      if (spinner.spin()) {
        continue;
      }
      const event_count::key key = events_.prepare_wait();
      if (pending_.load() == 0) {
        events_.cancel_wait();
        break;
      }
      if (auto index = take(me)) {
        events_.cancel_wait();
        run_job(*index, me);
        continue;
      }
      events_.wait(key);
    }
  }
  exiting_.store(true);
  events_.notify_all();
  static_cast<void>(threads_.release());
  stop_watchdog();
  stopped_ = true;
}

// --- submit ---

job_handle job_system::submit(job_desc desc, std::span<const dependency> deps,
                              job_counter *done) noexcept {
  auto handle = submit_impl(desc, deps, done, true);
  if (!handle) {
    rejected_.fetch_add(1);
    return {};
  }
  return *handle;
}

result<job_handle> job_system::try_submit(job_desc desc, std::span<const dependency> deps,
                                          job_counter *done) noexcept {
  return submit_impl(desc, deps, done, false);
}

std::uint64_t job_system::register_background() noexcept {
  const std::scoped_lock lock(results_mutex_);
  const std::uint64_t seq = seq_.fetch_add(1);
  background_.push_back(background_entry{.seq = seq, .done = false, .results = {}});
  return seq;
}

result<job_handle> job_system::submit_impl(job_desc &desc, std::span<const dependency> deps,
                                           job_counter *done, bool may_help) noexcept {
  if (deps.size() > job_max_dependencies) {
    return make_error(errc::invalid_argument, "submit: more than 8 dependencies");
  }
  if (!desc.fn) {
    return make_error(errc::invalid_argument, "submit: empty job body");
  }
  for (const dependency &d : deps) {
    bool valid = false;
    switch (d.what) {
    case dependency::kind::job:
      valid = d.job.generation == 0 || d.job.index < capacity_;
      break;
    case dependency::kind::counter:
      valid = d.counter != nullptr;
      break;
    case dependency::kind::fence:
      valid = d.fence != nullptr;
      break;
    }
    if (!valid) {
      return make_error(errc::invalid_argument, "submit: invalid dependency");
    }
  }
  if (stopping_.load()) {
    return make_error(errc::scheduler_stopped, "submit after stop");
  }
  thread_slot *slot = find_slot();
  std::optional<std::uint32_t> got = allocate_record();
  if (!got) {
    if (!may_help) {
      return make_error(errc::job_capacity, "try_submit: no free job record");
    }
    capacity_waits_.fetch_add(1);
    spin_wait spinner(policy_->spin_limit());
    for (;;) {
      if (stopping_.load()) {
        return make_error(errc::scheduler_stopped, "submit after stop");
      }
      if (slot != nullptr) {
        if (auto index = take(*slot)) {
          run_job(*index, *slot);
          got = allocate_record();
          if (got) {
            break;
          }
          continue;
        }
      }
      if (deps.empty()) {
        run_inline(desc, done, slot); // no ready job and no predecessor (ARCH3-R065)
        return job_handle{};
      }
      got = allocate_record();
      if (got) {
        break;
      }
      if (spinner.spin()) {
        continue;
      }
      const event_count::key key = events_.prepare_wait();
      got = allocate_record();
      if (got) {
        events_.cancel_wait();
        break;
      }
      events_.wait(key);
    }
  }

  const std::uint32_t index = *got;
  record &r = records_[index];
  record_cold &c = cold_[index];
  r.ops = desc.fn.ops_;
  r.ops->relocate(r.payload.data(), desc.fn.storage_.data());
  desc.fn.ops_ = nullptr;
  // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage): stored with its length (name_len)
  r.name_ptr.store(desc.name.data());
  r.name_len.store(clamp16(desc.name.size()));
  const sl::log::correlation corr = sl::log::current_correlation();
  r.scope_ptr = corr.scope.data();
  r.scope_len = clamp16(corr.scope.size());
  r.corr_id = corr.id;
  r.corr_tick = corr.tick;
  r.prio = desc.prio;
  r.aff = desc.aff;
  r.remaining.store(1); // the submit guard, dropped below
  c.done = done;
  c.error.store(0);
  c.cancel.store(false);
  if (done != nullptr) {
    done->add();
  }
  pending_.fetch_add(1);
  submitted_.fetch_add(1);
  c.seq = desc.prio == priority::background ? register_background() : seq_.fetch_add(1);
  const job_handle handle{.index = index, .generation = r.generation.load()};

  gate held;
  for (std::size_t k = 0; k < deps.size(); ++k) {
    const dependency &d = deps[k];
    switch (d.what) {
    case dependency::kind::job:
      if (attach(d.job, index, static_cast<std::uint32_t>(k)) == attach_result::cancel) {
        c.cancel.store(true);
      }
      break;
    case dependency::kind::counter:
      if (d.counter->value() != 0) {
        held.deps[held.count++] = d; // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index):
                                     // count <= deps.size() <= 8
      }
      break;
    case dependency::kind::fence:
      if (!d.fence->is_signalled()) {
        held.deps[held.count++] = d; // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index):
                                     // count <= deps.size() <= 8
      }
      break;
    }
  }
  if (held.count != 0) {
    held.index = index;
    r.remaining.fetch_add(1);
    const std::scoped_lock lock(gate_mutex_);
    gates_.push_back(held);
    gated_.fetch_add(1);
  }
  release_one(index, false, slot);
  return handle;
}

void job_system::run_inline(job_desc &desc, job_counter *done, thread_slot *slot) noexcept {
  inline_runs_.fetch_add(1);
  submitted_.fetch_add(1);
  const bool background = desc.prio == priority::background;
  const std::uint64_t seq = background ? register_background() : seq_.fetch_add(1);
  if (done != nullptr) {
    done->add();
  }
  job_context ctx(*this, slot != nullptr ? slot_index(slot) : external_thread, desc.prio, seq);
  std::uint8_t previous = no_priority;
  if (slot != nullptr) {
    previous = slot->prio;
    slot->prio = static_cast<std::uint8_t>(desc.prio);
  }
  if (!fault_before_body(ctx)) {
    desc.fn(ctx);
  }
  desc.fn = job_fn{};
  if (slot != nullptr) {
    slot->prio = previous;
  }
  if (ctx.fatal_) {
    report_fatal(ctx, job_handle{}, desc.name);
  }
  if (ctx.code_ != errc::ok) {
    failed_.fetch_add(1);
  }
  if (background) {
    background_done(seq);
  }
  if (done != nullptr) {
    done->sub();
  }
  completions_.fetch_add(1);
  events_.notify_all();
}

// --- scheduling ---

std::optional<std::uint32_t> job_system::take(thread_slot &slot) noexcept {
  stall(); // RFP4 before_pop
  const std::uint32_t self = slot_index(&slot);
  const bool owner = !slot.local;
  const bool may_background = !owner || workers_ == 0 || stopping_.load();
  ++slot.rounds;
  for (int pass = 0; pass < 2; ++pass) {
    if (owner) {
      if (auto index = main_queue_->steal()) {
        return index;
      }
    }
    if (may_background) {
      ++slot.since_background;
      // The background floor: every aging_limit-th round polls the background queue first,
      // so a saturating frame stream cannot starve it (RT-INV-JOB-08).
      const bool first =
          slot.rounds % aging_limit == 0 || policy_->promote_background(slot.since_background);
      if (first) {
        if (auto index = background_queue_->steal()) {
          slot.since_background = 0;
          return index;
        }
      }
    }
    if (!owner) {
      if (policy_->poll_injection(self, slot.local_pops)) {
        if (auto index = frame_queue_->steal()) {
          return index;
        }
      }
      if (auto index = slot.local->pop()) {
        slot.depth.fetch_sub(1);
        ++slot.local_pops;
        return index;
      }
    }
    if (auto index = frame_queue_->steal()) {
      return index;
    }
    if (may_background) {
      if (auto index = background_queue_->steal()) {
        slot.since_background = 0;
        return index;
      }
    }
    const std::uint32_t attempts = 2U * workers_;
    for (std::uint32_t a = 0; a < attempts; ++a) {
      const std::uint32_t v = policy_->victim(self, a);
      if (v == self || v >= workers_) {
        continue;
      }
      auto index = slots_[v].local->steal();
      policy_->on_steal(self, index.has_value());
      if (index) {
        slots_[v].depth.fetch_sub(1);
        stall(); // RFP4 after_steal
        return index;
      }
    }
    if (pass != 0 || gated_.load() == 0 || !poll_gates(&slot)) {
      break;
    }
  }
  return std::nullopt;
}

bool job_system::poll_gates(thread_slot *slot) noexcept {
  std::array<std::uint32_t, 16> released{};
  std::array<bool, 16> cancel{};
  std::size_t n = 0;
  {
    const std::unique_lock lock(gate_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
      return false;
    }
    const bool stopping = stopping_.load();
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index): k < g.count <= 8, n <
    // released.size()
    for (std::size_t i = 0; i < gates_.size() && n < released.size();) {
      const gate &g = gates_[i];
      bool ready = true;
      for (std::uint32_t k = 0; k < g.count && ready; ++k) {
        const dependency &d = g.deps[k];
        ready =
            d.what == dependency::kind::counter ? d.counter->value() == 0 : d.fence->is_signalled();
      }
      if (!ready && !stopping) {
        ++i;
        continue;
      }
      released[n] = g.index;
      cancel[n] = !ready;
      ++n;
      gates_[i] = gates_.back();
      gates_.pop_back();
      gated_.fetch_sub(1);
    }
  }
  for (std::size_t i = 0; i < n; ++i) {
    release_one(released[i], cancel[i], slot);
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)
  return n != 0;
}

void job_system::release_one(std::uint32_t index, bool cancel, thread_slot *slot) noexcept {
  if (cancel) {
    cold_[index].cancel.store(true);
  }
  if (records_[index].remaining.fetch_sub(1) == 1) {
    make_ready(index, slot);
  }
}

void job_system::make_ready(std::uint32_t index, thread_slot *slot) noexcept {
  const record &r = records_[index];
  const bool cancelled = cold_[index].cancel.load();
  bool pushed = false;
  if (!cancelled && r.aff == affinity::main) {
    pushed = main_queue_->push(index);
  } else if (!cancelled && r.prio == priority::background) {
    pushed = background_queue_->push(index);
  } else if (slot != nullptr && slot->local) {
    slot->depth.fetch_add(1);
    pushed = slot->local->push(index);
    if (!pushed) {
      slot->depth.fetch_sub(1);
    }
  }
  // The shared queues hold `capacity` cells and a record sits in at most one queue, so the frame
  // queue always has room.
  while (!pushed) {
    pushed = frame_queue_->push(index);
  }
  events_.notify_all();
}

void job_system::run_job(std::uint32_t index, thread_slot &slot) noexcept {
  record &r = records_[index];
  const record_cold &c = cold_[index];
  if (c.cancel.load() || stopping_.load()) {
    r.ops->destroy(r.payload.data());
    r.ops = nullptr;
    finish(index, job_status::cancelled, errc::job_cancelled, &slot);
    return;
  }
  const job_handle handle{.index = index, .generation = r.generation.load()};
  r.status.store(job_status::running);
  const std::uint64_t previous_job = slot.current.exchange(pack(handle));
  const std::uint8_t previous_prio = slot.prio;
  slot.prio = static_cast<std::uint8_t>(r.prio);
  job_context ctx(*this, slot_index(&slot), r.prio, c.seq);
  if (!fault_before_body(ctx)) {
    const sl::log::correlation_scope scope(sl::log::correlation{
        .id = r.corr_id, .scope = std::string_view{r.scope_ptr, r.scope_len}, .tick = r.corr_tick});
    r.ops->invoke(r.payload.data(), ctx);
  }
  r.ops->destroy(r.payload.data());
  r.ops = nullptr;
  slot.prio = previous_prio;
  slot.current.store(previous_job);
  stall(); // RFP4 before_complete
  if (ctx.fatal_) {
    report_fatal(ctx, handle, std::string_view{r.name_ptr.load(), r.name_len.load()});
  }
  finish(index, ctx.code_ == errc::ok ? job_status::done : job_status::failed, ctx.code_, &slot);
}

void job_system::finish(std::uint32_t index, job_status final_status, errc code,
                        thread_slot *slot) noexcept {
  const record &r = records_[index];
  record_cold &c = cold_[index];
  c.error.store(static_cast<std::uint32_t>(code));
  // Before the status turns final, so a waiter that returns sees the results and the counter.
  if (r.prio == priority::background) {
    background_done(c.seq);
  }
  if (c.done != nullptr) {
    c.done->sub();
  }
  if (final_status == job_status::failed) {
    failed_.fetch_add(1);
  } else if (final_status == job_status::cancelled) {
    cancelled_.fetch_add(1);
  }
  release_dependents(index, final_status, slot);
  completions_.fetch_add(1);
  pending_.fetch_sub(1);
  free_record(index);
  events_.notify_all();
}

void job_system::report_fatal(const job_context &ctx, job_handle handle,
                              std::string_view name) noexcept {
  fatal_.fetch_add(1);
  if (on_fatal_) {
    on_fatal_(fatal_report{.code = ctx.code_, .job = handle, .name = name, .worker = ctx.worker_});
  }
}

template <class Done> void job_system::help_until(Done done, const job_counter *counter) noexcept {
  thread_slot *slot = find_slot();
  spin_wait spinner(policy_->spin_limit());
  while (!done()) {
    if (slot != nullptr) {
      if (auto index = take(*slot)) {
        run_job(*index, *slot);
        spinner.reset();
        continue;
      }
    }
    if (spinner.spin()) {
      continue;
    }
    if (slot != nullptr && gated_.load() != 0) {
      std::this_thread::yield(); // a gate needs a polling thread; do not park
      continue;
    }
    stall(); // RFP4 in_wait
    if (counter != nullptr && pending_.load() == 0) {
      counter->wait_zero(); // no job can help: only an outside sub() can release the counter
      continue;
    }
    const event_count::key key = events_.prepare_wait();
    if (done()) {
      events_.cancel_wait();
      break;
    }
    if (slot != nullptr) {
      if (auto index = take(*slot)) {
        events_.cancel_wait();
        run_job(*index, *slot);
        spinner.reset();
        continue;
      }
    }
    events_.wait(key);
  }
}

void job_system::worker_main(std::uint32_t k) noexcept {
  thread_slot &slot = slots_[k];
  slot.id.store(std::this_thread::get_id());
  spin_wait spinner(policy_->spin_limit());
  for (;;) {
    if (auto index = take(slot)) {
      run_job(*index, slot);
      spinner.reset();
      continue;
    }
    if (exiting_.load()) {
      return;
    }
    if (spinner.spin()) {
      continue;
    }
    if (gated_.load() != 0) {
      std::this_thread::yield(); // a gate needs a polling thread; do not park
      continue;
    }
    const event_count::key key = events_.prepare_wait();
    if (exiting_.load()) {
      events_.cancel_wait();
      return;
    }
    if (auto index = take(slot)) {
      events_.cancel_wait();
      run_job(*index, slot);
      spinner.reset();
      continue;
    }
    policy_->on_park(k);
    events_.wait(key);
    policy_->on_wake(k);
  }
}

// --- waits and queries ---

result<void> job_system::wait(job_handle handle) noexcept {
  if (handle.generation == 0 || handle.index >= capacity_) {
    return {};
  }
  const record &r = records_[handle.index];
  thread_slot *slot = find_slot();
  const std::uint64_t previous = slot != nullptr ? slot->waiting.exchange(pack(handle)) : 0;
  help_until(
      [&r, handle] noexcept {
        return r.generation.load() != handle.generation || is_final(r.status.load());
      },
      nullptr);
  if (slot != nullptr) {
    slot->waiting.store(previous);
  }
  const job_status s = r.status.load();
  const auto code = static_cast<errc>(cold_[handle.index].error.load());
  if (r.generation.load() != handle.generation) {
    return {};
  }
  if (s == job_status::failed) {
    return make_error(errc::job_failed, "job " + std::to_string(handle.index) + ":" +
                                            std::to_string(handle.generation) + " failed with " +
                                            std::string(sl::to_string(code)));
  }
  if (s == job_status::cancelled) {
    return make_error(errc::job_cancelled, "job " + std::to_string(handle.index) + ":" +
                                               std::to_string(handle.generation) + " cancelled");
  }
  return {};
}

result<void> job_system::wait(const job_counter &counter) noexcept {
  help_until([&counter] noexcept { return counter.value() == 0; }, &counter);
  return {};
}

result<job_status> job_system::status(job_handle handle) const noexcept {
  if (handle.generation == 0 || handle.index >= capacity_) {
    return make_error(errc::stale_job, "status: null or out-of-range handle");
  }
  const record &r = records_[handle.index];
  const job_status s = r.status.load();
  if (r.generation.load() != handle.generation) {
    return make_error(errc::stale_job, "status: the record was reused");
  }
  return s;
}

job_stats job_system::stats() const noexcept {
  job_stats s;
  s.submitted = submitted_.load();
  s.completed = completions_.load();
  s.failed = failed_.load();
  s.cancelled = cancelled_.load();
  s.fatal = fatal_.load();
  s.inline_runs = inline_runs_.load();
  s.capacity_waits = capacity_waits_.load();
  s.rejected = rejected_.load();
  s.pending = pending_.load();
  return s;
}

std::optional<priority> job_system::current_job_priority() const noexcept {
  const std::thread::id me = std::this_thread::get_id();
  for (const thread_slot &s : slots_) {
    if (s.id.load() == me) {
      if (s.prio == no_priority) {
        return std::nullopt;
      }
      return static_cast<priority>(s.prio);
    }
  }
  return std::nullopt;
}

// --- owner-thread calls ---

void job_system::run_main_affinity() noexcept {
  SL_DEBUG_ASSERT(is_owner(), "job_system::run_main_affinity off the owner thread");
  if (!is_owner()) {
    return;
  }
  thread_slot &me = slots_[workers_];
  while (auto index = main_queue_->steal()) {
    run_job(*index, me);
  }
}

void job_system::drain_results(const std::function<void(const background_result &)> &deliver) {
  SL_DEBUG_ASSERT(is_owner(), "job_system::drain_results off the owner thread");
  if (!is_owner()) {
    return;
  }
  std::vector<background_result> out;
  {
    const std::scoped_lock lock(results_mutex_);
    while (!background_.empty() && background_.front().done) {
      for (background_result &res : background_.front().results) {
        out.push_back(std::move(res));
      }
      background_.pop_front();
    }
  }
  if (deliver) {
    for (const background_result &res : out) {
      deliver(res);
    }
  }
}

void job_system::quiesce() noexcept {
  SL_DEBUG_ASSERT(is_owner(), "job_system::quiesce off the owner thread");
  if (!is_owner()) {
    return;
  }
  help_until([this] noexcept { return pending_.load() == 0; }, nullptr);
  for (thread_slot &s : slots_) {
    if (s.local) {
      s.local->quiesce();
    }
  }
  frame_queue_->quiesce();
  background_queue_->quiesce();
  main_queue_->quiesce();
}

// --- background results ---

void job_system::post(std::uint64_t seq, std::uint32_t kind,
                      std::span<const std::byte> bytes) noexcept {
  const std::scoped_lock lock(results_mutex_);
  const auto it = std::ranges::lower_bound(background_, seq, {}, &background_entry::seq);
  if (it == background_.end() || it->seq != seq || it->done) {
    return;
  }
  it->results.push_back(
      background_result{.job_seq = seq,
                        .post_index = static_cast<std::uint32_t>(it->results.size()),
                        .kind = kind,
                        .bytes = std::vector<std::byte>(bytes.begin(), bytes.end())});
}

void job_system::background_done(std::uint64_t seq) noexcept {
  const std::scoped_lock lock(results_mutex_);
  const auto it = std::ranges::lower_bound(background_, seq, {}, &background_entry::seq);
  if (it != background_.end() && it->seq == seq) {
    it->done = true;
  }
}

// --- threads ---

job_system::thread_slot *job_system::find_slot() noexcept {
  const std::thread::id me = std::this_thread::get_id();
  for (thread_slot &s : slots_) {
    if (s.id.load() == me) {
      return &s;
    }
  }
  return nullptr;
}

std::uint32_t job_system::slot_index(const thread_slot *slot) const noexcept {
  return static_cast<std::uint32_t>(slot - slots_.data());
}

bool job_system::is_owner() const noexcept { return std::this_thread::get_id() == owner_; }

} // namespace sl::runtime::jobs
