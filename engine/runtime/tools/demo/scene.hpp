#pragma once

// The gate 100 runner (WO-099, SL-ARCH-003 ARCH3-R167 to R170, ADR-0028): the lantern-field
// scene of stack.hpp run headless for --ticks steps, in three passes over one binary:
//   fresh   from ingest, a snapshot every --snapshot-every steps (the newest two kept), the
//           system.fail fault on the step after fault_tick (RFP5 when the caller arms it, the
//           scripted fail otherwise) recovered by restoring the newest snapshot at or before the
//           fault (restore_snapshot, at most max_recoveries), the re-run steps checked against the
//           first run (recovery.hash_equal);
//   resume  a new stack restoring the fresh pass's snapshot of step resume_tick, then run to the
//           end;
//   replay  a new stack fed the fresh pass's recorded wall samples, no fault.
// The state hash after every step of each pass must equal the fresh pass's (D1, ARCH3-R170).
// The evidence (gate_evidence) goes to --evidence-dir only: demo.log, demo-report.json
// (sl-runtime-gate/1), memreport.json (sl-runtime-memreport/1), fault-<tick>.json
// (sl-runtime-fault/1, when a fault ran), schedule.json (sl-runtime-schedule/1) and hashes.json
// (sl-runtime-hashes/1, determinism_check.py's input). Exit codes (ARCH3-R168): 0 ok, 1 a
// divergence or an invariant failure, 2 usage, 3 a runtime fault that stops the run. Owner thread.

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/memdiag.hpp"
#include "sl/runtime/runtime.hpp"
#include "sl/runtime/work_queue.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sl::runtime::demo {

/// The `limits` gate rows.
inline constexpr std::uint32_t gate_lanterns = 50000;
inline constexpr std::uint32_t gate_ticks = 600;
inline constexpr std::uint32_t gate_snapshot_every = 300;
inline constexpr std::uint32_t gate_fault_tick = 450;
inline constexpr std::uint32_t max_recoveries = 3;
inline constexpr std::string_view default_evidence_dir = "build/runtime/gate";
inline constexpr std::string_view ok_line = "runtime-demo: ok";
inline constexpr std::string_view hashes_format = "sl-runtime-hashes/1";

enum class exit_code : int { ok = 0, failure = 1, usage = 2, fault = 3 };

struct demo_options {
  std::uint64_t seed = 1;
  std::uint32_t ticks = gate_ticks;
  /// Workers; nullopt is jobs::default_workers().
  std::optional<std::uint32_t> workers;
  ecs::execution_policy mode = ecs::execution_policy::parallel;
  std::uint32_t entities = gate_lanterns;
  std::uint32_t snapshot_every = gate_snapshot_every;
  std::string evidence_dir = std::string(default_evidence_dir);

  // --- not on the command line (the gate tests) ---

  /// The step after which the fault fires; nullopt (or a step at or past ticks) runs none.
  std::optional<std::uint32_t> fault_tick = gate_fault_tick;
  /// Called right before the faulting frame: true when it armed RFP5 itself (a test linking
  /// sl_runtime_testing), false to use the scripted system.fail.
  std::function<bool()> arm_fault;
  /// Called after the faulting frame when arm_fault returned true.
  std::function<void()> disarm_fault;
  /// Run the resume and replay passes.
  bool variants = true;
  /// Hash after every this many steps (and after the last); the command line always hashes every
  /// step (D1). Steps not hashed hold a zero digest in every pass.
  std::uint32_t hash_every = 1;
};

/// One phase over the fresh pass: jobs summed, the longest critical path, wall time summed.
struct phase_total {
  std::uint64_t jobs = 0;
  std::uint32_t critical_path = 0;
  std::uint64_t wall_ns = 0;
};

struct demo_summary {
  exit_code code = exit_code::ok;
  /// The failure, empty on success.
  std::string message;
  /// The run log (demo.log), one line each, "runtime-demo: ok" last on success.
  std::vector<std::string> log;

  std::uint64_t seed = 0;
  std::uint32_t ticks = 0;
  std::uint32_t entities = 0;
  std::uint32_t workers = 0;
  ecs::execution_policy mode = ecs::execution_policy::parallel;
  std::uint64_t frames = 0;
  std::uint64_t population = 0;

  /// Hash after step k + 1 at index k.
  std::vector<sl::fabric::digest> fresh;
  /// Hash after step resume_tick + 1 + k at index k.
  std::uint64_t resume_tick = 0;
  std::vector<sl::fabric::digest> resume;
  std::vector<sl::fabric::digest> replay;
  /// The first divergence: "<variant> step <n>".
  std::string divergence;

  std::optional<fault_report> fault;
  std::uint32_t recoveries = 0;
  bool hash_equal = true;

  std::array<phase_total, phase_count> phases{};
  jobs::worker_counters scheduler;

  std::uint64_t records = 0;
  std::uint64_t seed_commits = 0;
  std::uint64_t commits = 0;
  std::uint64_t proposed_patches = 0;
  std::uint64_t proposed_changes = 0;
  std::uint64_t rejected = 0;
  std::uint64_t echo_suppressed = 0;
  std::uint64_t watched_changes = 0;

  std::uint64_t leaks = 0;
  std::uint64_t steady_frames = 0;
  std::uint64_t steady_upstream = 0;
  std::uint64_t steady_log_events = 0;
  std::uint64_t wall_ms = 0;

  std::string schedule_json;
  mem::memory_report memory;
};

/// The usage text (ends with a newline).
[[nodiscard]] std::string_view usage();
/// Parses the flags (argv without the program name): invalid_argument naming the flag for an
/// unknown flag, a missing or bad value or a repeated flag.
[[nodiscard]] result<demo_options> parse_args(std::span<const std::string_view> args);
/// Runs the three passes; never writes a file.
[[nodiscard]] demo_summary run_demo(const demo_options &options);
/// sl-runtime-gate/1 of a summary.
[[nodiscard]] std::string report_json(const demo_summary &s);
/// sl-runtime-hashes/1 of a summary.
[[nodiscard]] std::string hashes_json(const demo_summary &s);
/// Writes the evidence files of `s` under options.evidence_dir (and nowhere else).
[[nodiscard]] result<void> write_evidence(const demo_options &options, const demo_summary &s);
/// The whole runner: parse, run, write, print. `out` receives each log line, `err` the usage
/// and errors. Returns the exit code.
[[nodiscard]] int run_cli(std::span<const std::string_view> args,
                          const std::function<void(std::string_view)> &out,
                          const std::function<void(std::string_view)> &err);

} // namespace sl::runtime::demo
