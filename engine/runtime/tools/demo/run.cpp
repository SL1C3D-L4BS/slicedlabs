// The gate 100 runner's passes (WO-099). See scene.hpp.

#include "scene.hpp"
#include "stack.hpp"

#include "sl/core/error.hpp"
#include "sl/core/log.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/headless_platform.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/memdiag.hpp"
#include "sl/runtime/runtime.hpp"
#include "sl/runtime/scheduler_stats.hpp"
#include "sl/runtime/snapshot.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace sl::runtime::demo {

namespace {

/// Counts every log event (the zero-log-events-per-steady-frame check).
class log_counter final : public sl::log::sink {
public:
  void write(const sl::log::event & /*e*/) noexcept override { count_.fetch_add(1); }
  [[nodiscard]] std::uint64_t count() const noexcept { return count_.load(); }

private:
  std::atomic<std::uint64_t> count_{0};
};

/// Attaches a log_counter to the default logger for its lifetime.
class counting_scope {
public:
  counting_scope() : counter_(std::make_shared<log_counter>()) {
    sl::log::default_logger().add_sink(counter_);
  }
  counting_scope(const counting_scope &) = delete;
  counting_scope(counting_scope &&) = delete;
  counting_scope &operator=(const counting_scope &) = delete;
  counting_scope &operator=(counting_scope &&) = delete;
  ~counting_scope() { sl::log::default_logger().remove_sink(counter_.get()); }

  [[nodiscard]] std::uint64_t count() const noexcept { return counter_->count(); }

private:
  std::shared_ptr<log_counter> counter_;
};

[[nodiscard]] std::string hash_text(const sl::fabric::digest &d) { return "sha256:" + d.to_hex(); }

[[nodiscard]] std::string_view mode_name(ecs::execution_policy m) noexcept {
  return m == ecs::execution_policy::serial_canonical ? "serial" : "parallel";
}

/// The first system with a recorded failure: its name and job name.
void name_failure(const scene_stack &s, fault_report &r) {
  for (std::uint32_t id = 0; id < s.systems->size(); ++id) {
    const errc code = s.systems->last_error(id);
    if (code == errc::ok) {
      continue;
    }
    const std::string name = id < s.system_names.size() ? s.system_names[id] : "system";
    auto index = s.systems->system_index(id);
    r.code = code;
    r.system = name;
    r.job = std::format("{}#{}", name, index ? *index : 0U);
    return;
  }
}

/// The fresh pass's bookkeeping.
struct fresh_state {
  std::vector<sl::fabric::digest> hashes;
  std::vector<bool> seen;
  std::vector<snapshot> kept; // the newest two
  std::optional<snapshot> resume;
  std::uint32_t resume_change_tick = 0;
  std::uint32_t quiet_frames = 0; // frames left out of the steady counts after a restore
};

struct pass_result {
  exit_code code = exit_code::ok;
  std::string message;
};

[[nodiscard]] pass_result fault_exit(std::string message) {
  return pass_result{.code = exit_code::fault, .message = std::move(message)};
}

/// Whether the state after `step` steps is hashed.
[[nodiscard]] bool hashed_step(const demo_options &o, std::uint64_t step) noexcept {
  return o.hash_every <= 1 || step % o.hash_every == 0 || step == o.ticks;
}

/// One fresh frame's steady accounting and phase totals.
void account(demo_summary &out, const frame_stats &f, std::uint64_t world_allocs,
             std::uint64_t log_events, bool quiet) {
  for (std::size_t p = 0; p < phase_count; ++p) {
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index): p < phase_count
    out.phases[p].jobs += f.phases[p].jobs;
    out.phases[p].critical_path = std::max(out.phases[p].critical_path, f.phases[p].critical_path);
    out.phases[p].wall_ns += static_cast<std::uint64_t>(f.phases[p].wall.count());
    // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)
  }
  const bool warm = f.frame_index > std::uint64_t{warmup_frames_default} + 1U;
  const bool report_frame = f.frame_index % mem::report_interval_frames == 0;
  if (!warm || quiet || report_frame) {
    return;
  }
  ++out.steady_frames;
  out.steady_upstream += f.upstream_allocs + world_allocs;
  out.steady_log_events += log_events;
}

[[nodiscard]] pass_result recover(scene_stack &s, demo_summary &out, fresh_state &st,
                                  const frame_stats &f, std::uint64_t step) {
  ++out.recoveries;
  fault_report r;
  r.instance = 1;
  r.frame = f.frame_index;
  r.tick = step;
  r.kind = "system_fail";
  r.code = errc::job_failed;
  r.action = "restore";
  name_failure(s, r);
  if (out.recoveries > max_recoveries) {
    r.action = "stop";
    out.fault = r;
    return fault_exit("more than max_recoveries faults");
  }
  auto newest_first = std::views::reverse(st.kept);
  const auto found = std::ranges::find_if(
      newest_first, [step](const snapshot &k) noexcept { return k.tick() <= step; });
  const snapshot *newest = found == newest_first.end() ? nullptr : &*found;
  if (newest == nullptr) {
    r.action = "stop";
    out.fault = r;
    return fault_exit("no snapshot at or before the fault");
  }
  if (auto restored = s.restore(*newest); !restored) {
    r.action = "stop";
    out.fault = r;
    return fault_exit("restore failed: " + restored.error().message);
  }
  r.restored_tick = newest->tick();
  if (!out.fault) {
    out.fault = r;
  }
  st.quiet_frames = 2;
  return {};
}

[[nodiscard]] pass_result run_fresh(scene_stack &s, const demo_options &o, demo_summary &out,
                                    fresh_state &st, const counting_scope &logs) {
  if (auto primed = s.prime(); !primed) {
    return fault_exit("priming frame: " + primed.error().message);
  }
  const auto keep = [&](std::uint64_t step, std::uint64_t frame) -> pass_result {
    auto taken = s.take(frame, step);
    if (!taken) {
      return fault_exit("snapshot failed: " + taken.error().message);
    }
    if (step == out.resume_tick && !st.resume) {
      st.resume = *taken;
      st.resume_change_tick = s.world->change_tick();
    }
    st.kept.push_back(std::move(*taken));
    if (st.kept.size() > 2) {
      st.kept.erase(st.kept.begin());
    }
    return {};
  };
  if (auto kept = keep(0, s.frames_run); kept.code != exit_code::ok) {
    return kept;
  }
  const bool faulting = o.fault_tick.has_value() && *o.fault_tick < o.ticks;
  bool fault_done = false;
  std::uint64_t step = 0;
  while (step < o.ticks) {
    bool armed = false;
    if (faulting && !fault_done && step == *o.fault_tick) {
      fault_done = true;
      armed = o.arm_fault && o.arm_fault();
      if (!armed) {
        s.fail_next.store(true);
      }
    }
    const std::uint64_t failures = s.systems->failures();
    const std::uint64_t world_before = s.world_memory->totals().allocs;
    const std::uint64_t logs_before = logs.count();
    auto frame = s.step();
    if (armed && o.disarm_fault) {
      o.disarm_fault();
    }
    if (!frame) {
      return fault_exit("frame failed: " + frame.error().message);
    }
    const bool quiet = st.quiet_frames > 0;
    if (quiet) {
      --st.quiet_frames;
    }
    if (s.systems->failures() != failures) {
      if (auto r = recover(s, out, st, *frame, step); r.code != exit_code::ok) {
        return r;
      }
      step = s.scene().step;
      continue;
    }
    // cppcheck-suppress duplicateExpression ; the frame between the two reads emits the events
    account(out, *frame, s.world_memory->totals().allocs - world_before, logs.count() - logs_before,
            quiet);
    const std::uint64_t now = s.scene().step;
    if (now != step + 1) {
      return pass_result{.code = exit_code::failure,
                         .message = std::format("step {} ran {} steps", step, now - step)};
    }
    step = now;
    if (o.snapshot_every > 0 && step % o.snapshot_every == 0) {
      if (auto kept = keep(step, frame->frame_index); kept.code != exit_code::ok) {
        return kept;
      }
    }
    if (!hashed_step(o, step)) {
      continue;
    }
    auto h = s.hash();
    if (!h) {
      return fault_exit("state hash failed: " + h.error().message);
    }
    const std::size_t at = step - 1;
    if (st.seen[at] && st.hashes[at] != *h) {
      out.hash_equal = false;
    }
    st.hashes[at] = *h;
    st.seen[at] = true;
  }
  return {};
}

/// Runs a variant stack from step `from` to o.ticks and checks each hash against the fresh pass.
[[nodiscard]] pass_result run_variant(scene_stack &s, const demo_options &o,
                                      const std::vector<sl::fabric::digest> &fresh,
                                      std::uint64_t from, const std::vector<nanos> *samples,
                                      std::vector<sl::fabric::digest> &hashes, std::string name,
                                      std::string &divergence) {
  std::size_t frame = 1;
  for (std::uint64_t step = from; step < o.ticks; ++step, ++frame) {
    auto f =
        samples != nullptr && frame < samples->size() ? s.step_at((*samples)[frame]) : s.step();
    if (!f) {
      return fault_exit(name + " frame failed: " + f.error().message);
    }
    if (s.systems->failures() != 0) {
      return pass_result{.code = exit_code::failure, .message = name + ": a system failed"};
    }
    if (!hashed_step(o, step + 1)) {
      hashes.emplace_back();
      continue;
    }
    auto h = s.hash();
    if (!h) {
      return fault_exit(name + " state hash failed: " + h.error().message);
    }
    hashes.push_back(*h);
    if (divergence.empty() && (step >= fresh.size() || fresh[step] != *h)) {
      divergence = std::format("{} step {}", name, step + 1);
    }
  }
  return {};
}

void fill_fabric(const scene_stack &s, demo_summary &out) {
  const link::link_stats ls = s.link->stats();
  out.records = ls.ingested;
  out.seed_commits = s.host->seed_commits();
  out.commits = s.host->commits();
  out.proposed_patches = ls.proposed_patches;
  out.proposed_changes = ls.proposed_changes;
  out.rejected = ls.rejected;
  out.echo_suppressed = ls.echo_suppressed;
  out.watched_changes = s.watch().changes;
}

void log_line(demo_summary &out, std::string line) {
  out.log.push_back("runtime-demo: " + std::move(line));
}

void fail(demo_summary &out, exit_code code, std::string message) {
  if (out.code == exit_code::ok || (out.code == exit_code::failure && code == exit_code::fault)) {
    out.code = code;
    out.message = std::move(message);
  }
}

} // namespace

demo_summary run_demo(const demo_options &o) {
  const steady_clock_source wall;
  const nanos started = wall.now();
  const counting_scope logs;
  demo_summary out;
  out.seed = o.seed;
  out.ticks = o.ticks;
  out.entities = o.entities;
  out.mode = o.mode;
  out.workers = o.workers.value_or(jobs::default_workers());
  out.resume_tick = o.snapshot_every > 0 && o.snapshot_every < o.ticks ? o.snapshot_every : 0;
  log_line(out, std::format("lantern field seed={} ticks={} entities={} workers={} mode={} "
                            "snapshot_every={} evidence_dir={}",
                            o.seed, o.ticks, o.entities, out.workers, mode_name(o.mode),
                            o.snapshot_every, o.evidence_dir));

  stack_options so;
  so.seed = o.seed;
  so.workers = out.workers;
  so.mode = o.mode;
  so.entities = o.entities;
  so.root_dir = o.evidence_dir;
  so.frames_hint = (std::size_t{o.ticks} * 2U) + 64U;

  fresh_state st;
  st.hashes.resize(o.ticks);
  st.seen.assign(o.ticks, false);
  std::vector<nanos> samples;
  {
    auto made = scene_stack::make(so);
    if (!made) {
      fail(out, exit_code::fault, "the scene does not start: " + made.error().message);
      log_line(out, "FAIL " + out.message);
      return out;
    }
    scene_stack &s = **made;
    log_line(out,
             std::format("workspace domain={} records={} seed_commits={} lanterns={}", scene_domain,
                         s.link->stats().ingested, s.host->seed_commits(), o.entities));
    const pass_result fresh = run_fresh(s, o, out, st, logs);
    if (fresh.code != exit_code::ok) {
      fail(out, fresh.code, fresh.message);
    }
    out.frames = s.frames_run;
    out.population = s.population();
    fill_fabric(s, out);
    out.schedule_json = s.schedule_json;
    out.scheduler = jobs::collect(*s.policy, s.worker_count + 1U).total();
    out.scheduler.jobs_run = s.js->stats().completed;
    samples = s.recorder.samples();
    out.memory = s.finish();
    out.leaks = out.memory.leaks.size();
  }
  out.fresh = st.hashes;
  if (out.code == exit_code::ok && o.variants && st.resume) {
    stack_options ro = so;
    ro.change_tick = st.resume_change_tick;
    auto made = scene_stack::make(ro);
    if (!made) {
      fail(out, exit_code::fault, "the resume stack does not start: " + made.error().message);
    } else {
      scene_stack &s = **made;
      if (auto primed = s.prime(); !primed) {
        fail(out, exit_code::fault, "resume priming frame: " + primed.error().message);
      } else if (auto restored = s.restore(*st.resume); !restored) {
        fail(out, exit_code::fault, "resume restore: " + restored.error().message);
      } else {
        const pass_result r = run_variant(s, o, out.fresh, out.resume_tick, nullptr, out.resume,
                                          "resume", out.divergence);
        if (r.code != exit_code::ok) {
          fail(out, r.code, r.message);
        }
      }
      [[maybe_unused]] const mem::memory_report shut = s.finish();
    }
  }
  if (out.code == exit_code::ok && o.variants) {
    auto made = scene_stack::make(so);
    if (!made) {
      fail(out, exit_code::fault, "the replay stack does not start: " + made.error().message);
    } else {
      scene_stack &s = **made;
      auto primed = samples.empty() ? s.prime() : s.step_at(samples.front());
      if (!primed) {
        fail(out, exit_code::fault, "replay priming frame: " + primed.error().message);
      } else {
        const pass_result r =
            run_variant(s, o, out.fresh, 0, &samples, out.replay, "replay", out.divergence);
        if (r.code != exit_code::ok) {
          fail(out, r.code, r.message);
        }
      }
      [[maybe_unused]] const mem::memory_report shut = s.finish();
    }
  }
  out.wall_ms = static_cast<std::uint64_t>((wall.now() - started).count() / 1'000'000);

  // The invariants (exit 1).
  if (!out.divergence.empty()) {
    fail(out, exit_code::failure, "hash divergence: " + out.divergence);
  }
  if (!out.hash_equal) {
    fail(out, exit_code::failure, "the recovered run diverged from the first run");
  }
  if (out.leaks != 0) {
    fail(out, exit_code::failure, std::format("{} leaked blocks", out.leaks));
  }
  if (out.steady_upstream != 0 || out.steady_log_events != 0) {
    fail(out, exit_code::failure,
         std::format("steady frames: {} upstream allocations, {} log events", out.steady_upstream,
                     out.steady_log_events));
  }
  if (out.rejected != 0) {
    fail(out, exit_code::failure, std::format("{} observations rejected", out.rejected));
  }

  const std::string final_hash = out.fresh.empty() ? "none" : hash_text(out.fresh.back());
  log_line(out, std::format("fresh steps={} frames={} population={} final_state_hash={}",
                            out.fresh.size(), out.frames, out.population, final_hash));
  if (out.fault) {
    log_line(out, std::format("fault tick={} system={} code={} action={} restored_tick={} "
                              "recoveries={} hash_equal={}",
                              out.fault->tick, out.fault->system, sl::to_string(out.fault->code),
                              out.fault->action, out.fault->restored_tick.value_or(0),
                              out.recoveries, out.hash_equal));
  }
  log_line(out, std::format("resume from={} steps={} replay steps={} divergence={}",
                            out.resume_tick, out.resume.size(), out.replay.size(),
                            out.divergence.empty() ? "none" : out.divergence));
  log_line(out, std::format("fabric records={} seed_commits={} commits={} proposed_patches={} "
                            "proposed_changes={} rejected={} echo_suppressed={} watched={}",
                            out.records, out.seed_commits, out.commits, out.proposed_patches,
                            out.proposed_changes, out.rejected, out.echo_suppressed,
                            out.watched_changes));
  log_line(out, std::format("memory leaks={} steady_frames={} steady_upstream={} "
                            "steady_log_events={} wall_ms={}",
                            out.leaks, out.steady_frames, out.steady_upstream,
                            out.steady_log_events, out.wall_ms));
  if (out.code == exit_code::ok) {
    out.log.emplace_back(ok_line);
  } else {
    log_line(out, std::format("FAIL exit {}: {}", static_cast<int>(out.code), out.message));
  }
  return out;
}

} // namespace sl::runtime::demo
