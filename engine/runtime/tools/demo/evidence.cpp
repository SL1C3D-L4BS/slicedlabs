// The gate 100 runner's command line and evidence files (WO-099). See scene.hpp.

#include "scene.hpp"

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/memdiag.hpp"
#include "sl/runtime/platform.hpp"
#include "sl/runtime/runtime.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace sl::runtime::demo {

namespace {

/// The most workers the runner accepts.
constexpr std::uint32_t max_workers = 64;

#ifdef __clang__
constexpr std::string_view compiler_name = "clang";
#elifdef __GNUC__
constexpr std::string_view compiler_name = "gcc";
#else
constexpr std::string_view compiler_name = "other";
#endif

[[nodiscard]] std::string hash_text(const sl::fabric::digest &d) { return "sha256:" + d.to_hex(); }

[[nodiscard]] std::string_view mode_name(ecs::execution_policy m) noexcept {
  return m == ecs::execution_policy::serial_canonical ? "serial" : "parallel";
}

template <class T>
[[nodiscard]] result<T> parse_number(std::string_view flag, std::string_view text, T low, T high) {
  T value{};
  const char *first = text.data();
  const char *last = first + text.size();
  auto [end, ec] = std::from_chars(first, last, value);
  if (text.empty() || ec != std::errc{} || end != last || value < low || value > high) {
    return make_error(errc::invalid_argument,
                      std::format("{}: '{}' is not an integer in [{}, {}]", flag, text, low, high));
  }
  return value;
}

[[nodiscard]] std::string hash_list(const std::vector<sl::fabric::digest> &hashes) {
  std::string out = "[";
  for (std::size_t i = 0; i < hashes.size(); ++i) {
    out += std::format("{}\n    \"{}\"", i == 0 ? "" : ",", hash_text(hashes[i]));
  }
  out += hashes.empty() ? "]" : "\n  ]";
  return out;
}

[[nodiscard]] std::string log_text(const demo_summary &s) {
  std::string out;
  for (const std::string &line : s.log) {
    out += line;
    out += '\n';
  }
  return out;
}

[[nodiscard]] result<void> write_text(const file_access &files, std::string_view path,
                                      std::string_view text) {
  return files.atomic_write(path, std::as_bytes(std::span(text.data(), text.size())));
}

} // namespace

std::string_view usage() {
  return "usage: sl_runtime_demo [--seed N] [--ticks N] [--workers N] [--mode serial|parallel]\n"
         "                       [--entities N] [--snapshot-every N] [--evidence-dir DIR]\n"
         "  --seed N            the scene seed (default 1)\n"
         "  --ticks N           steps to run, 1 or more (default 600)\n"
         "  --workers N         job workers, 1 to 64 (default: the hardware threads)\n"
         "  --mode M            serial (serial_canonical) or parallel (default parallel)\n"
         "  --entities N        transient lanterns (default 50000)\n"
         "  --snapshot-every N  steps between snapshots, 0 for none (default 300)\n"
         "  --evidence-dir DIR  where every evidence file goes (default build/runtime/gate)\n"
         "exit codes: 0 ok, 1 divergence or invariant failure, 2 usage, 3 runtime fault\n";
}

result<demo_options> parse_args(std::span<const std::string_view> args) {
  demo_options o;
  std::vector<std::string_view> seen;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view flag = args[i];
    const bool known = flag == "--seed" || flag == "--ticks" || flag == "--workers" ||
                       flag == "--mode" || flag == "--entities" || flag == "--snapshot-every" ||
                       flag == "--evidence-dir";
    if (!known) {
      return make_error(errc::invalid_argument, std::format("unknown argument '{}'", flag));
    }
    if (std::ranges::find(seen, flag) != seen.end()) {
      return make_error(errc::invalid_argument, std::format("{} given twice", flag));
    }
    seen.push_back(flag);
    if (i + 1 >= args.size()) {
      return make_error(errc::invalid_argument, std::format("{} needs a value", flag));
    }
    const std::string_view value = args[++i];
    constexpr auto u32_max = std::numeric_limits<std::uint32_t>::max();
    if (flag == "--seed") {
      auto v =
          parse_number<std::uint64_t>(flag, value, 0, std::numeric_limits<std::uint64_t>::max());
      if (!v) {
        return std::unexpected(v.error());
      }
      o.seed = *v;
    } else if (flag == "--ticks") {
      auto v = parse_number<std::uint32_t>(flag, value, 1, u32_max);
      if (!v) {
        return std::unexpected(v.error());
      }
      o.ticks = *v;
    } else if (flag == "--workers") {
      auto v = parse_number<std::uint32_t>(flag, value, 1, max_workers);
      if (!v) {
        return std::unexpected(v.error());
      }
      o.workers = *v;
    } else if (flag == "--entities") {
      auto v = parse_number<std::uint32_t>(flag, value, 0, u32_max);
      if (!v) {
        return std::unexpected(v.error());
      }
      o.entities = *v;
    } else if (flag == "--snapshot-every") {
      auto v = parse_number<std::uint32_t>(flag, value, 0, u32_max);
      if (!v) {
        return std::unexpected(v.error());
      }
      o.snapshot_every = *v;
    } else if (flag == "--mode") {
      if (value == "serial") {
        o.mode = ecs::execution_policy::serial_canonical;
      } else if (value == "parallel") {
        o.mode = ecs::execution_policy::parallel;
      } else {
        return make_error(errc::invalid_argument,
                          std::format("--mode: '{}' is not serial or parallel", value));
      }
    } else {
      if (value.empty()) {
        return make_error(errc::invalid_argument, "--evidence-dir: empty");
      }
      o.evidence_dir = std::string(value);
    }
  }
  return o;
}

std::string report_json(const demo_summary &s) {
  std::string out = "{\n";
  out += "  \"format\": \"sl-runtime-gate/1\",\n";
  out += std::format("  \"seed\": {},\n  \"ticks\": {},\n  \"entities\": {},\n  \"workers\": {},\n",
                     s.seed, s.ticks, s.entities, s.workers);
  out += std::format("  \"mode\": \"{}\",\n  \"phases\": [", mode_name(s.mode));
  for (std::size_t p = 0; p < phase_count; ++p) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): p < phase_count
    const phase_total &t = s.phases[p];
    out += std::format("{}\n    {{\"phase\": \"{}\", \"jobs\": {}, \"critical_path\": {}, "
                       "\"wall_ns\": {}}}",
                       p == 0 ? "" : ",", ecs::detail::phase_name(static_cast<phase>(p)), t.jobs,
                       t.critical_path, t.wall_ns);
  }
  out += "\n  ],\n";
  const jobs::worker_counters &c = s.scheduler;
  out += std::format("  \"scheduler\": {{\"jobs_run\": {}, \"steals\": {}, \"failed_steals\": {}, "
                     "\"parks\": {}, \"wakes\": {}, \"injection_polls\": {}, "
                     "\"aging_promotions\": {}}},\n",
                     c.jobs_run, c.steals, c.failed_steals, c.parks, c.wakes, c.injection_polls,
                     c.aging_promotions);
  const std::uint64_t fault_tick = s.fault ? s.fault->tick : 0;
  const std::uint64_t restored_tick = s.fault ? s.fault->restored_tick.value_or(0) : 0;
  out += std::format("  \"recovery\": {{\"fault_tick\": {}, \"restored_tick\": {}, "
                     "\"hash_equal\": {}}},\n",
                     fault_tick, restored_tick, s.hash_equal);
  out += std::format(
      "  \"fabric\": {{\"commits\": {}, \"proposed_changes\": {}, \"rejected\": {}}},\n", s.commits,
      s.proposed_changes, s.rejected);
  const std::string final_hash =
      s.fresh.empty() ? hash_text(sl::fabric::digest{}) : hash_text(s.fresh.back());
  out += std::format("  \"final_state_hash\": \"{}\",\n  \"wall_ms\": {}\n}}\n", final_hash,
                     s.wall_ms);
  return out;
}

std::string hashes_json(const demo_summary &s) {
  std::string out = "{\n";
  out +=
      std::format("  \"format\": \"{}\",\n  \"compiler\": \"{}\",\n", hashes_format, compiler_name);
  out += std::format("  \"seed\": {},\n  \"ticks\": {},\n  \"entities\": {},\n  \"workers\": {},\n",
                     s.seed, s.ticks, s.entities, s.workers);
  out += std::format("  \"mode\": \"{}\",\n  \"resume_tick\": {},\n", mode_name(s.mode),
                     s.resume_tick);
  out += std::format("  \"fresh\": {},\n  \"resume\": {},\n  \"replay\": {}\n}}\n",
                     hash_list(s.fresh), hash_list(s.resume), hash_list(s.replay));
  return out;
}

result<void> write_evidence(const demo_options &options, const demo_summary &s) {
  auto files = file_access::make_posix(options.evidence_dir);
  if (!files) {
    return std::unexpected(files.error());
  }
  if (auto ok = write_text(*files, "demo-report.json", report_json(s)); !ok) {
    return ok;
  }
  if (auto ok = write_text(*files, "memreport.json", mem::to_json(s.memory)); !ok) {
    return ok;
  }
  if (s.fault) {
    const std::string name = std::format("fault-{}.json", s.fault->tick);
    if (auto ok = write_text(*files, name, to_json(*s.fault)); !ok) {
      return ok;
    }
  }
  if (!s.schedule_json.empty()) {
    if (auto ok = write_text(*files, "schedule.json", s.schedule_json); !ok) {
      return ok;
    }
  }
  if (auto ok = write_text(*files, "hashes.json", hashes_json(s)); !ok) {
    return ok;
  }
  return write_text(*files, "demo.log", log_text(s));
}

int run_cli(std::span<const std::string_view> args,
            const std::function<void(std::string_view)> &out,
            const std::function<void(std::string_view)> &err) {
  auto options = parse_args(args);
  if (!options) {
    err(std::format("runtime-demo: {}\n", options.error().message));
    err(usage());
    return static_cast<int>(exit_code::usage);
  }
  demo_summary s = run_demo(*options);
  if (auto written = write_evidence(*options, s); !written) {
    s.log.pop_back();
    s.log.push_back(std::format("runtime-demo: FAIL exit {}: evidence not written: {}",
                                static_cast<int>(exit_code::fault), written.error().message));
    s.code = exit_code::fault;
  }
  for (const std::string &line : s.log) {
    out(line);
    out("\n");
  }
  if (s.code != exit_code::ok) {
    err(std::format("runtime-demo: {}\n", s.message.empty() ? "failed" : s.message));
  }
  return static_cast<int>(s.code);
}

} // namespace sl::runtime::demo
