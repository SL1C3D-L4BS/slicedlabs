// The runtime core (WO-096, SL-ARCH-003 ARCH3-R022, R023, R032 to R034, ADR-0028): the rt_states
// table, create, the transition and its runtime.lifecycle event, registration before start, the
// any-thread reads and runtime_abi_digest.

#include "sl/runtime/runtime.hpp"

#include "sl/core/assert.hpp"
#include "sl/core/error.hpp"
#include "sl/core/log.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/faults.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/memdiag.hpp"
#include "sl/runtime/memory.hpp"
#include "sl/runtime/platform.hpp"
#include "sl/runtime/replay.hpp"
#include "sl/runtime/snapshot.hpp"
#include "sl/runtime/time.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
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

namespace sl::runtime {

namespace {

struct table_row {
  rt_state from;
  rt_event event;
  rt_state to;
};

// SL-ARCH-003 `rt_states`, in table order.
constexpr std::array<table_row, 15> rt_table = {{
    {.from = rt_state::created, .event = rt_event::start, .to = rt_state::starting},
    {.from = rt_state::starting, .event = rt_event::start_ok, .to = rt_state::ready},
    {.from = rt_state::starting, .event = rt_event::start_fail, .to = rt_state::created},
    {.from = rt_state::ready, .event = rt_event::run_frame, .to = rt_state::running},
    {.from = rt_state::running, .event = rt_event::suspend, .to = rt_state::suspended},
    {.from = rt_state::suspended, .event = rt_event::resume, .to = rt_state::running},
    {.from = rt_state::ready, .event = rt_event::stop, .to = rt_state::stopping},
    {.from = rt_state::running, .event = rt_event::stop, .to = rt_state::stopping},
    {.from = rt_state::suspended, .event = rt_event::stop, .to = rt_state::stopping},
    {.from = rt_state::running, .event = rt_event::fault, .to = rt_state::faulted},
    {.from = rt_state::faulted, .event = rt_event::recover, .to = rt_state::recovering},
    {.from = rt_state::recovering, .event = rt_event::recover_ok, .to = rt_state::running},
    {.from = rt_state::recovering, .event = rt_event::recover_fail, .to = rt_state::stopping},
    {.from = rt_state::faulted, .event = rt_event::stop, .to = rt_state::stopping},
    {.from = rt_state::stopping, .event = rt_event::stopped, .to = rt_state::stopped},
}};

constexpr std::array<std::string_view, rt_state_count> state_names = {
    "Created",  "Starting", "Ready",   "Running",   "Suspended",
    "Stopping", "Stopped",  "Faulted", "Recovering"};

constexpr std::array<std::string_view, rt_event_count> event_names = {
    "start", "start_ok", "start_fail", "run_frame",  "suspend",      "resume",
    "stop",  "fault",    "recover",    "recover_ok", "recover_fail", "stopped"};

[[nodiscard]] result<void> check_config(const runtime_config &c) noexcept {
  if (c.instance_id == 0) {
    return make_error(errc::invalid_argument, "runtime_config: instance_id 0");
  }
  if (c.rate.num == 0 || c.rate.den == 0) {
    return make_error(errc::invalid_argument, "runtime_config: a tick rate term is 0");
  }
  if (c.max_frame_delta <= nanos{0}) {
    return make_error(errc::invalid_argument, "runtime_config: max_frame_delta at or below 0");
  }
  if (c.workers.has_value() && *c.workers > jobs::max_workers) {
    return make_error(errc::invalid_argument, "runtime_config: more than 64 workers");
  }
  if (c.job_capacity == 0 || c.job_capacity > jobs::job_capacity_max) {
    return make_error(errc::invalid_argument, "runtime_config: job_capacity out of range");
  }
  if (c.max_entities == 0) {
    return make_error(errc::invalid_argument, "runtime_config: max_entities 0");
  }
  return {};
}

} // namespace

std::string_view to_string(rt_state s) noexcept {
  const auto i = static_cast<std::size_t>(s);
  return i < state_names.size() ? std::span(state_names)[i] : std::string_view{"unknown"};
}

std::string_view to_string(rt_event e) noexcept {
  const auto i = static_cast<std::size_t>(e);
  return i < event_names.size() ? std::span(event_names)[i] : std::string_view{"unknown"};
}

result<rt_state> next_state(rt_state from, rt_event e) noexcept {
  for (const table_row &row : rt_table) {
    // cppcheck-suppress useStlAlgorithm ; the loop returns the matching row's target state
    if (row.from == from && row.event == e) {
      return row.to;
    }
  }
  return make_error(errc::invalid_state, std::string("rt_states: no transition for ") +
                                             std::string(to_string(e)) + " in " +
                                             std::string(to_string(from)));
}

// --- create ---

result<std::unique_ptr<runtime>> runtime::create(platform &plat,
                                                 const runtime_config &config) noexcept {
  if (auto checked = check_config(config); !checked) {
    return std::unexpected(checked.error());
  }
  std::unique_ptr<runtime> rt(new (std::nothrow) runtime(passkey{}, plat, config));
  if (rt == nullptr) {
    return make_error(errc::out_of_memory, "runtime::create: allocation failed");
  }
  return rt;
}

runtime::runtime(passkey /*key*/, platform &plat, const runtime_config &config)
    : plat_(&plat), config_(config), scope_("runtime/" + std::to_string(config.instance_id)),
      owner_(std::this_thread::get_id()) {}

runtime::~runtime() { teardown(); }

// --- registration ---

result<void> runtime::add_system(ecs::system_desc desc) {
  if (!is_owner()) {
    return make_error(errc::wrong_thread, "runtime::add_system off the owner thread");
  }
  if (state_.load() != rt_state::created) {
    return make_error(errc::invalid_state, "runtime::add_system after start");
  }
  if (desc.name.empty() || !desc.fn) {
    return make_error(errc::invalid_argument, "runtime::add_system: empty name or body");
  }
  staged_systems_.push_back(std::move(desc));
  return {};
}

void runtime::add_contributor(det::snapshot_contributor &contributor) {
  SL_DEBUG_ASSERT(is_owner(), "runtime::add_contributor off the owner thread");
  SL_DEBUG_ASSERT(state_.load() == rt_state::created, "runtime::add_contributor after start");
  if (!is_owner() || state_.load() != rt_state::created) {
    return;
  }
  contributors_.push_back(&contributor);
}

// --- transitions (ARCH3-R022, R034) ---

result<void> runtime::transition(rt_event e) noexcept {
  const rt_state from = state_.load();
  auto to = next_state(from, e);
  if (!to) {
    return std::unexpected(to.error());
  }
  state_.store(*to);
  const time::tick t = current_tick();
  const sl::log::correlation_scope correlation(scope_, t);
  sl::log::emit(sl::log::level::info, "runtime.lifecycle", "transition",
                {{"from", to_string(from)},
                 {"to", to_string(*to)},
                 {"tick", t},
                 {"instance", config_.instance_id}});
  return {};
}

void runtime::apply_event(rt_event e) noexcept {
  [[maybe_unused]] const result<void> moved = transition(e);
  SL_DEBUG_ASSERT(moved.has_value(), "runtime: rt_states refused an internal transition");
}

time::tick runtime::current_tick() const noexcept {
  return frames_ != nullptr ? frames_->current().now : last_tick_;
}

std::vector<const det::snapshot_contributor *> runtime::const_contributors() const {
  return {contributors_.begin(), contributors_.end()};
}

// --- reads ---

std::optional<fault_report> runtime::last_fault() const {
  const std::scoped_lock lock(fault_mutex_);
  return last_fault_;
}

mem::memory_report runtime::memory_report() const {
  const std::scoped_lock lock(diag_mutex_);
  if (shutdown_report_.has_value()) {
    return *shutdown_report_;
  }
  if (!diag_published_ || diag_ == nullptr) {
    mem::memory_report empty;
    empty.instance = config_.instance_id;
    return empty;
  }
  return diag_->make_report(last_frame_.load());
}

std::vector<time::tick> runtime::snapshot_ticks() const {
  std::vector<time::tick> out;
  out.reserve(snapshots_.size());
  for (const snapshot &s : snapshots_) {
    // cppcheck-suppress useStlAlgorithm ; a loop over at most two snapshots reads plainer
    out.push_back(s.tick());
  }
  return out;
}

sl::fabric::digest runtime::runtime_abi_digest() const noexcept {
  detail::snapshot_writer w;
  w.u32(ecs::chunk_bytes);
  w.u32(ecs::entity_index_bits);
  w.u32(ecs::entity_generation_bits);
  w.digest(components_.layout_digest());
  w.u32(error_code_table_version);
  w.u32(failpoint_table_version);
  w.u32(det::ordered_key_layout_version);
  return detail::sha256_of(w.bytes());
}

// --- the composed parts ---

ecs::world &runtime::world() noexcept {
  SL_ASSERT(world_ != nullptr, "runtime::world before start or after shutdown");
  // cppcheck-suppress nullPointerRedundantCheck ; SL_ASSERT above does not return on null
  return *world_;
}

frame_scheduler &runtime::frames() noexcept {
  SL_ASSERT(frames_ != nullptr, "runtime::frames before start or after shutdown");
  // cppcheck-suppress nullPointerRedundantCheck ; SL_ASSERT above does not return on null
  return *frames_;
}

jobs::job_system &runtime::jobs() noexcept {
  SL_ASSERT(jobs_ != nullptr, "runtime::jobs before start");
  // cppcheck-suppress nullPointerRedundantCheck ; SL_ASSERT above does not return on null
  return *jobs_;
}

ecs::system_scheduler &runtime::systems() noexcept {
  SL_ASSERT(systems_ != nullptr, "runtime::systems before start or after shutdown");
  // cppcheck-suppress nullPointerRedundantCheck ; SL_ASSERT above does not return on null
  return *systems_;
}

mem::tracking_resource &runtime::root_memory() noexcept {
  SL_ASSERT(root_ != nullptr, "runtime::root_memory before start");
  // cppcheck-suppress nullPointerRedundantCheck ; SL_ASSERT above does not return on null
  return *root_;
}

} // namespace sl::runtime
