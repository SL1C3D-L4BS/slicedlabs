// Fault capture (WO-096, SL-ARCH-003 ARCH3-R028, R029, ADR-0028): fatal job reports, failed
// systems and out_of_memory during playback are collected during the frame and handled in the
// end_frame slot fault_check: Running -> Faulted, the frame's commands cleared unplayed,
// frame_faulted() set for the link, then the recovery policy, and one sl-runtime-fault/1 report
// (last_fault and one runtime.fault log event). The frame scheduler's frame is marked faulted from
// the phase end that saw the failure (events neither delivered nor flipped) and unmarked after a
// restore, whose event lists are the snapshot's and flip at arena_flip as that frame's (WO-109).

#include "sl/runtime/runtime.hpp"

#include "sl/core/error.hpp"
#include "sl/core/log.hpp"
#include "sl/runtime/ecs/command_buffer.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/jobs.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sl::runtime {

void runtime::on_fatal(const jobs::fatal_report &report) noexcept {
  const std::scoped_lock lock(fatal_mutex_);
  fatal_.push_back(report);
}

void runtime::discard_after_fatal() noexcept {
  bool fatal = false;
  {
    const std::scoped_lock lock(fatal_mutex_);
    fatal = !fatal_.empty();
  }
  if (fatal) {
    queue_->clear(); // ARCH3-R028: the faulted frame's command buffers are never played back
  }
  // ARCH3-R028: from this phase end on, the frame's events are neither delivered nor flipped.
  if (fatal || systems_->failures() != failures_seen_) {
    frames_->set_frame_faulted(true);
  }
}

void runtime::on_phase_end(phase ph) noexcept {
  // The system phases replace last_flush at their end; job_failed is a discarded playback.
  if (ph == phase::begin_frame || ph == phase::present || ph == phase::end_frame) {
    return;
  }
  const result<ecs::flush_report> &flushed = systems_->last_flush();
  if (!flushed && flushed.error().code == errc::out_of_memory) {
    playback_oom_ = true;
    frames_->set_frame_faulted(true);
  }
}

std::optional<fault_report> runtime::detect_fault(const boundary_context &bc) {
  std::vector<jobs::fatal_report> fatal;
  {
    const std::scoped_lock lock(fatal_mutex_);
    fatal.swap(fatal_);
  }
  const std::uint64_t failures = systems_->failures();
  const bool system_failed = failures != failures_seen_;
  failures_seen_ = failures;
  const bool oom = playback_oom_;
  playback_oom_ = false;
  // The latest code of every system: one that changed since the last check names the system.
  std::vector<errc> errors(system_names_.size(), errc::ok);
  for (std::size_t id = 0; id < errors.size(); ++id) {
    errors[id] = systems_->last_error(static_cast<std::uint32_t>(id));
  }
  const std::vector<errc> seen = std::exchange(errors_seen_, errors);
  if (fatal.empty() && !system_failed && !oom) {
    return std::nullopt;
  }
  fault_report r;
  r.instance = config_.instance_id;
  r.frame = bc.frame_index;
  r.tick = bc.tick;
  if (!fatal.empty()) {
    r.kind = "job_fatal";
    r.code = fatal.front().code;
    r.job = std::string(fatal.front().name);
  } else if (system_failed) {
    r.kind = "system_fail";
    r.code = errc::job_failed;
    std::optional<std::size_t> pick;
    for (std::size_t id = 0; id < errors.size() && !pick; ++id) {
      if (errors[id] != errc::ok && (id >= seen.size() || seen[id] != errors[id])) {
        pick = id;
      }
    }
    for (std::size_t id = 0; id < errors.size() && !pick; ++id) {
      if (errors[id] != errc::ok) {
        pick = id;
      }
    }
    if (pick) {
      r.code = errors[*pick];
      r.system = system_names_[*pick];
      const auto index = systems_->system_index(static_cast<std::uint32_t>(*pick));
      r.job = r.system + "#" + std::to_string(index ? *index : 0U);
    }
  } else {
    r.kind = "playback_oom";
    r.code = errc::out_of_memory;
  }
  return r;
}

void runtime::on_fault_check(const boundary_context &bc) noexcept {
  last_frame_.store(bc.frame_index);
  last_tick_ = bc.tick;
  if (state_.load() != rt_state::running) {
    return;
  }
  std::optional<fault_report> found = detect_fault(bc);
  if (!found) {
    return;
  }
  fault_report report = std::move(*found);
  frame_faulted_.store(true);
  frames_->set_frame_faulted(true);
  queue_->clear(); // ARCH3-R028: nothing more of the faulted frame is played back
  apply_event(rt_event::fault);
  report.action = "stop";
  if (config_.recovery == recovery_policy::stop) {
    apply_event(rt_event::stop);
    frame_error_ = errc::runtime_faulted;
  } else if (over_recovery_limit(bc.tick)) {
    apply_event(rt_event::stop);
    frame_error_ = errc::recovery_failed;
  } else {
    apply_event(rt_event::recover);
    const result<time::tick> restored = restore_newest(bc.tick);
    if (restored) {
      {
        const std::scoped_lock lock(fatal_mutex_);
        fatal_.clear();
      }
      recoveries_.fetch_add(1);
      frames_->set_frame_faulted(false); // the snapshot's event lists flip on as its frame's
      report.action = "restore";
      report.restored_tick = *restored;
      apply_event(rt_event::recover_ok);
    } else {
      apply_event(rt_event::recover_fail);
      frame_error_ = errc::recovery_failed;
    }
  }
  publish_fault(report);
}

void runtime::publish_fault(const fault_report &report) {
  {
    const std::scoped_lock lock(fault_mutex_);
    last_fault_ = report;
  }
  const std::string json = to_json(report);
  const sl::log::correlation_scope correlation(scope_, report.tick);
  sl::log::emit(sl::log::level::error, "runtime.fault", "fault",
                {{"kind", report.kind},
                 {"code", sl::to_string(report.code)},
                 {"system", report.system},
                 {"job", report.job},
                 {"action", report.action},
                 {"frame", report.frame},
                 {"tick", report.tick},
                 {"instance", report.instance},
                 {"report", json}});
}

// --- sl-runtime-fault/1 ---

namespace {

void put_number(std::string &out, std::string_view key, std::uint64_t value) {
  out += "  \"";
  out += key;
  out += "\": ";
  out += std::to_string(value);
  out += ",\n";
}

void put_text(std::string &out, std::string_view key, std::string_view value) {
  out += "  \"";
  out += key;
  out += "\": ";
  sl::log::append_json_string(out, value);
  out += ",\n";
}

} // namespace

std::string to_json(const fault_report &report) {
  std::string out = "{\n  \"format\": \"sl-runtime-fault/1\",\n";
  put_number(out, "instance", report.instance);
  put_number(out, "frame", report.frame);
  put_number(out, "tick", report.tick);
  put_text(out, "kind", report.kind);
  put_text(out, "code", sl::to_string(report.code));
  put_number(out, "number", static_cast<std::uint64_t>(report.code));
  put_text(out, "system", report.system);
  put_text(out, "job", report.job);
  put_text(out, "action", report.action);
  out += "  \"restored_tick\": ";
  out += report.restored_tick ? std::to_string(*report.restored_tick) : std::string("null");
  out += "\n}\n";
  return out;
}

} // namespace sl::runtime
