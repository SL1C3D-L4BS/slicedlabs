// System scheduling (WO-094, SL-ARCH-003 ARCH3-R121, R122, R090, ADR-0024, ADR-0026): add, the
// policy, the runs of one system (its frame task, a main-affinity job for exclusive systems and
// for serial_canonical, RFP5 before every body) and the phase end hook (change tick, command
// playback, phase_active).

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/command_buffer.hpp"
#include "sl/runtime/ecs/query.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/faults.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/replay.hpp"
#include "sl/runtime/sync.hpp"
#include "sl/runtime/time.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sl::runtime::ecs {

namespace {

// The phases of the `phases` table with systems: true.
[[nodiscard]] bool takes_systems(phase ph) noexcept {
  switch (ph) {
  case phase::apply_inputs:
  case phase::pre_tick:
  case phase::tick:
  case phase::post_tick:
  case phase::observe:
    return true;
  case phase::begin_frame:
  case phase::present:
  case phase::end_frame:
    return false;
  }
  return false;
}

template <class T> void normalize(std::vector<T> &v) {
  std::ranges::sort(v);
  const auto tail = std::ranges::unique(v);
  v.erase(tail.begin(), tail.end());
}

// Sorts both lists and drops from reads what writes holds.
template <class T> void normalize(std::vector<T> &reads, std::vector<T> &writes) {
  normalize(reads);
  normalize(writes);
  std::erase_if(reads,
                [&writes](const T &r) noexcept { return std::ranges::binary_search(writes, r); });
}

} // namespace

void add_query_access(access_set &into, const component_access &query_access) {
  into.components.reads.insert(into.components.reads.end(), query_access.reads.begin(),
                               query_access.reads.end());
  into.components.writes.insert(into.components.writes.end(), query_access.writes.begin(),
                                query_access.writes.end());
  normalize(into.components.reads, into.components.writes);
}

system_scheduler::system_scheduler(jobs::job_system &jobs) noexcept
    : jobs_(&jobs), owner_(std::this_thread::get_id()) {}

result<std::uint32_t> system_scheduler::add(system_desc desc) noexcept {
  if (!is_owner()) {
    return make_error(errc::wrong_thread, "system_scheduler::add off the owner thread");
  }
  if (built_) {
    return make_error(errc::schedule_frozen, "system_scheduler::add after build");
  }
  if (!takes_systems(desc.ph)) {
    return make_error(errc::invalid_argument, "system_scheduler::add: a phase without systems");
  }
  if (desc.name.empty()) {
    return make_error(errc::invalid_argument, "system_scheduler::add: empty name");
  }
  if (!desc.fn) {
    return make_error(errc::invalid_argument, "system_scheduler::add: empty body");
  }
  for (const detail::system_record &s : systems_) {
    // cppcheck-suppress useStlAlgorithm ; the error names the duplicate
    if (s.name == desc.name) {
      return make_error(errc::already_exists,
                        "system_scheduler::add: the name " + std::string(desc.name) + " is used");
    }
  }
  const auto in_phase = std::ranges::count_if(
      systems_, [ph = desc.ph](const detail::system_record &s) noexcept { return s.ph == ph; });
  if (std::cmp_greater_equal(in_phase, max_systems_per_phase)) {
    return make_error(errc::invalid_argument,
                      "system_scheduler::add: max_systems_per_phase reached");
  }
  access_set access = std::move(desc.access);
  normalize(access.components.reads, access.components.writes);
  normalize(access.resource_reads, access.resource_writes);
  for (const component_access &q : desc.queries) {
    // cppcheck-suppress useStlAlgorithm ; the error names the system
    if (!detail::covers(access.components, q)) {
      return make_error(errc::access_conflict, "system_scheduler::add: a query of " +
                                                   std::string(desc.name) +
                                                   " needs access its access_set lacks");
    }
  }
  const auto id = static_cast<std::uint32_t>(systems_.size());
  detail::system_record rec;
  rec.name = std::string(desc.name);
  rec.ph = desc.ph;
  rec.access = std::move(access);
  rec.before.assign(desc.before.begin(), desc.before.end());
  rec.after.assign(desc.after.begin(), desc.after.end());
  rec.fn = std::move(desc.fn);
  rec.chunked = desc.chunked;
  rec.id = id;
  systems_.push_back(std::move(rec));
  return id;
}

void system_scheduler::set_policy(execution_policy next) noexcept {
  SL_DEBUG_ASSERT(is_owner(), "system_scheduler::set_policy off the owner thread");
  if (is_owner()) {
    policy_ = next;
  }
}

result<std::uint16_t> system_scheduler::system_index(std::uint32_t id) const noexcept {
  if (!built_) {
    return make_error(errc::invalid_state, "system_scheduler::system_index before build");
  }
  if (id >= systems_.size()) {
    return make_error(errc::not_found, "system_scheduler::system_index: unknown id");
  }
  return systems_[id].index;
}

std::span<const std::uint32_t> system_scheduler::order(phase ph) const noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): a phase is < phase_count
  return order_[static_cast<std::size_t>(ph)];
}

std::span<const std::uint16_t> system_scheduler::edges(std::uint32_t id) const noexcept {
  if (id >= systems_.size()) {
    return {};
  }
  return systems_[id].edges;
}

// --- runs ---

void system_scheduler::system_task::operator()(task_context &ctx) const noexcept {
  self->run_task(slot, ctx);
}

void system_scheduler::owner_job::operator()(jobs::job_context &ctx) const noexcept {
  self->run_system(slot, ctx, *time, tick);
}

void system_scheduler::phase_hook::operator()(phase ph, time::tick /*tick*/) const noexcept {
  self->end_phase(ph);
}

void system_scheduler::run_task(std::uint32_t slot, task_context &ctx) noexcept {
  const detail::system_record &s = systems_[slot];
  if (policy_ == execution_policy::serial_canonical) {
    // The phase's first system runs the whole phase on the owner thread; the others are empty.
    if (s.index == 0) {
      run_serial_phase(s.ph, ctx);
    }
    return;
  }
  if (s.access.exclusive) {
    run_on_owner(slot, ctx);
    return;
  }
  run_system(slot, ctx.job, ctx.time, ctx.tick);
}

void system_scheduler::run_on_owner(std::uint32_t slot, task_context &ctx) noexcept {
  job_counter done;
  auto submitted = jobs_->try_submit(
      jobs::job_desc{.fn =
                         owner_job{.self = this, .time = &ctx.time, .tick = ctx.tick, .slot = slot},
                     .prio = jobs::priority::frame,
                     .aff = jobs::affinity::main,
                     .name = std::string_view{systems_[slot].name}},
      {}, &done);
  if (!submitted) {
    ctx.job.fail(submitted.error().code);
    note_failure(systems_[slot], submitted.error().code);
    return;
  }
  // The owner thread runs main-affinity jobs while it waits for the phase (helping).
  const result<void> waited = jobs_->wait(done);
  SL_DEBUG_ASSERT(waited.has_value(), "system_scheduler: a counter wait cannot fail");
}

void system_scheduler::run_serial_phase(phase ph, task_context &ctx) noexcept {
  job_counter done;
  jobs::job_handle previous{};
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): a phase is < phase_count
  for (const std::uint32_t slot : order_[static_cast<std::size_t>(ph)]) {
    std::array<jobs::dependency, 1> deps{jobs::dependency::on(previous)};
    const std::size_t n = previous.generation == 0 ? 0 : 1;
    auto submitted = jobs_->try_submit(
        jobs::job_desc{
            .fn = owner_job{.self = this, .time = &ctx.time, .tick = ctx.tick, .slot = slot},
            .prio = jobs::priority::frame,
            .aff = jobs::affinity::main,
            .name = std::string_view{systems_[slot].name}},
        std::span<const jobs::dependency>(deps).first(n), &done);
    if (!submitted) {
      ctx.job.fail(submitted.error().code);
      note_failure(systems_[slot], submitted.error().code);
      break;
    }
    previous = *submitted;
  }
  const result<void> waited = jobs_->wait(done);
  SL_DEBUG_ASSERT(waited.has_value(), "system_scheduler: a counter wait cannot fail");
}

void system_scheduler::run_system(std::uint32_t slot, jobs::job_context &job,
                                  const time::frame_time &time, std::uint64_t tick) noexcept {
  detail::system_record &s = systems_[slot];
  // ARCH3-R128: the slot is the system's own, so concurrent runs write distinct slots.
  const std::uint32_t key = last_run_key(static_cast<std::uint8_t>(s.ph), s.index);
  const iter_args args{.since = last_runs_.since(key),
                       .write_tick = world_->change_tick() + 1U + std::uint32_t{s.index}};
  system_context ctx(*this, s, *world_, args, job, time, tick);
  s.fn(ctx);
  last_runs_.record(key, args.write_tick);
#if SL_RUNTIME_FAULTS
  if (visit(failpoint::rfp5).fire) { // RFP5 system.fail: the system fails after its body
    job.fail(errc::job_failed);
    note_failure(s, errc::job_failed);
  }
#endif
}

void system_scheduler::note_failure(detail::system_record &s, errc code) noexcept {
  s.last_error = code; // written by the system's own run only, read between frames
  failures_.fetch_add(1);
  frame_failed_.store(true);
}

errc system_scheduler::last_error(std::uint32_t id) const noexcept {
  return id < systems_.size() ? systems_[id].last_error : errc::ok;
}

void system_scheduler::end_phase(phase ph) noexcept {
  if (takes_systems(ph)) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): a phase is < phase_count
    const auto n = static_cast<std::uint32_t>(order_[static_cast<std::size_t>(ph)].size());
    if (n != 0) {
      // Every system of the run had its own write tick; playback stamps the next one.
      world_->set_change_tick(world_->change_tick() + n + 1U);
    }
    if (frame_failed_.load()) {
      queue_->clear(); // ARCH3-R028: a faulted frame plays nothing back
      last_flush_ = make_error(errc::job_failed,
                               "system_scheduler: a system failed this frame; commands discarded");
    } else {
      last_flush_ = queue_->flush(*world_);
    }
  }
  if (ph == phase::begin_frame) {
    frame_failed_.store(false);
  }
  // Systems run from apply_inputs to observe; the boundary steps after observe are outside.
  world_->set_phase_active(ph == phase::begin_frame || ph == phase::apply_inputs ||
                           ph == phase::pre_tick || ph == phase::tick || ph == phase::post_tick);
}

// --- system_context ---

command_buffer &system_context::commands() noexcept {
  if (commands_ == nullptr) {
    // On a violation the system fails; the buffer still records and the frame's playback is
    // discarded at the phase boundary.
    check_commands();
    const det::ordered_key base{.tick = tick_,
                                .phase = static_cast<std::uint8_t>(record_->ph),
                                .system_index = record_->index,
                                .chunk_index = 0,
                                .local_seq = 0};
    commands_ = &scheduler_->queue_->open(base, job.worker());
  }
  return *commands_;
}

std::uint16_t system_context::system_index() const noexcept { return record_->index; }

phase system_context::current_phase() const noexcept { return record_->ph; }

jobs::job_system &system_context::jobs() const noexcept { return *scheduler_->jobs_; }

void system_context::fail(errc code) noexcept {
  job.fail(code);
  if (!stopped_) {
    scheduler_->note_failure(*record_, code);
  }
  stopped_ = true;
}

bool system_context::inline_batches() const noexcept {
  return scheduler_->policy_ == execution_policy::serial_canonical || !record_->chunked;
}

} // namespace sl::runtime::ecs
