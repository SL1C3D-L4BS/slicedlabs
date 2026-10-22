// sl_runtime_dump_contract (WO-100): the C++ side of `just runtime-contract-lock` (RT-INV-GATE-06,
// SL-ARCH-003 ARCH3-R020 and R174).
//
//   sl_runtime_dump_contract        (no argument; the JSON goes to stdout)
//
// Prints the contract tables as sl_runtime compiles them, read from the code that defines them (the
// tables, enums and constants of the headers; no copy of the document in here), as one JSON
// document with sorted keys, two-space indentation and one trailing newline, byte-stable:
//
//   {"boundary_slots": [name...], "error_codes": [{area, name, number}...],
//    "failpoints": [{id, name, site}...], "format": "sl-runtime-contract/1",
//    "gate_evidence": [{file, format}...], "limits": {name: value...},
//    "phases": [{index, phase}...],
//    "rt_states": {events: [...], states: [...], transitions: [[from, event, to]...]},
//    "table_versions": {error_codes, failpoints}}
//
// rt_states.transitions are every (state, event) pair next_state() accepts, in (state, event) enum
// order. limits holds every constant of the `limits` table the code defines (headers, the
// runtime_config and swap_request defaults, and the gate constants of tools/demo); the rows the
// code cannot expose (test tiers, CMake scales, formulas, constants private to one .cpp) are marked
// "bound": false in the document. gate_evidence lists the files the C++ side writes: the format of
// memreport.json, fault-<tick>.json and hashes.json comes from the code, the others are the
// literals the gate scene writes (gate_demo_test, RT-INV-GATE-01, checks the written files).
// tools/runtime/contract_lock.py adds the sections the Python tools own (stress_matrix, layers,
// the Python-written evidence formats) and compares the whole with SL-ARCH-003 and
// schemas/runtime/contract.lock.json. Exit 0, or 2 with a usage line when given an argument. Owner
// thread; no file is opened (ARCH3-R035): the recipe redirects stdout under build/.

#include "demo/host.hpp"
#include "demo/scene.hpp"

#include "sl/runtime/allocators.hpp"
#include "sl/runtime/bridge.hpp"
#include "sl/runtime/ecs/change.hpp"
#include "sl/runtime/ecs/command_buffer.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/query.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/events.hpp"
#include "sl/runtime/faults.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/memdiag.hpp"
#include "sl/runtime/memory.hpp"
#include "sl/runtime/platform.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/replay.hpp"
#include "sl/runtime/runtime.hpp"
#include "sl/runtime/scheduler_stats.hpp"
#include "sl/runtime/snapshot.hpp"
#include "sl/runtime/sync.hpp"
#include "sl/runtime/time.hpp"
#include "sl/runtime/ws_deque.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace sl::runtime;

std::string quote(std::string_view s) {
  std::string out = "\"";
  for (const char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
    } else {
      out += c;
    }
  }
  return out + "\"";
}

// A JSON value already rendered (a number or a quoted string).
using rendered = std::string;

rendered num(std::uint64_t v) { return std::format("{}", v); }

rendered hex_byte(std::byte b) {
  return quote(std::format("0x{:02X}", static_cast<unsigned>(std::to_integer<unsigned>(b))));
}

// The `limits` rows the code defines, keyed by the table name (std::map keeps them sorted). A row
// with a runtime_config field reads the default the runtime uses (the config default).
std::map<std::string, rendered, std::less<>> limits() {
  const runtime_config config{};
  const swap_request swap{};
  const jobs::steal_options steal{};
  using change_tick_t = decltype(std::declval<const ecs::world &>().change_tick());
  const auto config_delta_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(config.max_frame_delta).count();
  std::map<std::string, rendered, std::less<>> m;
  // ECS storage (ecs/world.hpp, ecs/component.hpp, ecs/query.hpp); component columns are aligned to
  // chunk_alignment (src/ecs_archetype_layout.cpp align_up), so column_alignment is that constant.
  m["chunk_bytes"] = num(ecs::chunk_bytes);
  m["chunk_alignment"] = num(ecs::chunk_alignment);
  m["column_alignment"] = num(ecs::chunk_alignment);
  m["max_rows_per_chunk"] = num(ecs::max_rows_per_chunk);
  m["max_components_per_archetype"] = num(ecs::max_components_per_archetype);
  m["max_component_types"] = num(ecs::max_component_types);
  m["max_component_size"] = num(ecs::max_component_size);
  m["max_component_alignment"] = num(ecs::max_component_alignment);
  m["max_archetypes"] = num(ecs::max_archetypes);
  m["chunk_pool_slab_bytes"] = num(ecs::chunk_pool_slab_bytes);
  m["chunk_pool_slab_alignment"] = num(ecs::chunk_pool_slab_alignment);
  m["max_query_terms"] = num(ecs::max_query_terms);
  m["par_batch_min_rows"] = num(ecs::par_batch_min_rows);
  // entities (ecs/entity.hpp) and the runtime_config default
  m["entity_index_bits"] = num(ecs::entity_index_bits);
  m["entity_generation_bits"] = num(ecs::entity_generation_bits);
  m["entity_generation_first"] = num(ecs::entity_generation_first);
  m["entity_generation_retire"] = num(ecs::entity_generation_retire);
  m["entity_reuse_fifo_threshold"] = num(ecs::entity_reuse_fifo_threshold);
  m["entity_page_records"] = num(ecs::entity_page_records);
  m["max_entities_default"] = num(config.max_entities);
  // jobs (jobs.hpp, ws_deque.hpp, scheduler_stats.hpp, sync.hpp)
  m["job_capacity_default"] = num(config.job_capacity);
  m["job_capacity_max"] = num(jobs::job_capacity_max);
  m["job_record_bytes"] = num(jobs::job_record_bytes);
  m["job_record_alignment"] = num(jobs::job_record_alignment);
  m["job_inline_payload_bytes"] = num(jobs::job_inline_payload_bytes);
  m["job_payload_alignment"] = num(jobs::job_payload_alignment);
  m["job_max_dependencies"] = num(jobs::job_max_dependencies);
  m["job_nested_wait_depth"] = num(jobs::job_nested_wait_depth);
  m["ws_deque_initial_cells"] = num(jobs::ws_deque_initial_cells);
  m["ws_deque_max_cells"] = num(jobs::ws_deque_max_cells);
  m["injection_poll_every"] = num(steal.injection_poll_every);
  m["spin_limit"] = num(spin_limit);
  m["steal_spin_limit"] = num(steal.spin_limit);
  m["aging_limit"] = num(jobs::aging_limit);
  m["steal_aging_limit"] = num(steal.aging_limit);
  m["max_workers"] = num(jobs::max_workers);
  m["reserved_threads_default"] = num(config.reserved_threads);
  m["watchdog_ms"] = num(config.watchdog_ms);
  m["watchdog_period_ms"] = num(jobs::watchdog_period_ms);
  m["fault_stall_max_us"] = num(jobs::worker_stall_max_us);
  // time (time.hpp) and the runtime_config defaults
  m["tick_rate_default"] = quote(std::format("{}/{}", config.rate.num, config.rate.den));
  m["max_frame_delta"] = num(static_cast<std::uint64_t>(config_delta_ms));
  m["max_catchup"] = num(config.max_catchup);
  // change tracking (ecs/change.hpp, ecs/world.hpp)
  m["change_tick_bits"] = num(std::numeric_limits<change_tick_t>::digits);
  m["change_tick_sweep_every"] = num(ecs::change_tick_sweep_every);
  m["change_tick_max_age"] = num(ecs::change_tick_max_age);
  m["last_run_slots"] = num(ecs::last_run_slots);
  // frame memory and the frame scheduler (allocators.hpp, ecs/command_buffer.hpp, frame.hpp)
  m["frame_arena_block_bytes"] = num(mem::frame_arena_block_bytes);
  m["command_buffer_block_bytes"] = num(ecs::command_buffer_block_bytes);
  m["command_buffer_block_alignment"] = num(ecs::command_buffer_block_alignment);
  m["warmup_frames"] = num(warmup_frames_default);
  m["max_tasks_per_phase"] = num(max_tasks_per_phase);
  m["max_systems_per_phase"] = num(ecs::max_systems_per_phase);
  // events, reflection, diagnostics (events.hpp, reflect.hpp, memdiag.hpp)
  m["event_capacity_default"] = num(event_capacity_default);
  m["max_fields_per_type"] = num(max_fields_per_type);
  m["max_method_arity"] = num(max_method_arity);
  m["report_interval_frames"] = num(config.report_interval_frames);
  // lifecycle, recovery and swaps (runtime.hpp defaults)
  m["max_recoveries"] = num(config.max_recoveries);
  m["recovery_window_ticks"] = num(config.recovery_window_ticks);
  m["health_ticks_default"] = num(swap.health_ticks);
  // link, memory and allocators (bridge.hpp, memory.hpp, allocators.hpp)
  m["max_projections_held"] = num(link::max_projections_held);
  m["max_alignment"] = num(mem::max_alignment);
  m["max_memory_tags"] = num(mem::max_memory_tags);
  m["pool_slab_bytes"] = num(mem::pool_slab_bytes);
  m["pool_min_block_bytes"] = num(mem::pool_min_block_bytes);
  m["arena_block_bytes_default"] = num(mem::arena_block_bytes_default);
  m["debug_fill_fresh"] = hex_byte(mem::debug_fill_fresh);
  m["debug_fill_freed"] = hex_byte(mem::debug_fill_freed);
  // platform (platform.hpp)
  m["input_payload_bytes"] = num(input_payload_bytes);
  // versions (error.hpp, faults.hpp, replay.hpp, snapshot.hpp)
  m["error_code_table_version"] = num(error_code_table_version);
  m["failpoint_table_version"] = num(failpoint_table_version);
  m["ordered_key_layout_version"] = num(det::ordered_key_layout_version);
  m["snapshot_major"] = num(snapshot_major);
  m["snapshot_minor"] = num(snapshot_minor);
  // the gate scene (tools/demo/scene.hpp, tools/demo/host.hpp)
  m["gate_records"] = num(std::uint64_t{demo::beacon_records} + demo::ward_records);
  m["gate_lanterns"] = num(demo::gate_lanterns);
  m["gate_ticks"] = num(demo::gate_ticks);
  m["gate_snapshot_every"] = num(demo::gate_snapshot_every);
  m["gate_fault_tick"] = num(demo::gate_fault_tick);
  m["gate_max_recoveries"] = num(demo::max_recoveries);
  return m;
}

std::string_view slot_name(boundary_slot s) noexcept {
  switch (s) {
  case boundary_slot::fault_check:
    return "fault_check";
  case boundary_slot::observation_flush:
    return "observation_flush";
  case boundary_slot::projection_rebase:
    return "projection_rebase";
  case boundary_slot::snapshot:
    return "snapshot";
  case boundary_slot::swap:
    return "swap";
  case boundary_slot::lifecycle_requests:
    return "lifecycle_requests";
  case boundary_slot::retire_after:
    return "retire_after";
  case boundary_slot::arena_flip:
    return "arena_flip";
  case boundary_slot::diagnostics:
    return "diagnostics";
  }
  return "unknown";
}

// The value of the first "format" key of a JSON text the code wrote.
std::string format_of(std::string_view json) {
  constexpr std::string_view key = R"("format": ")";
  const std::size_t at = json.find(key);
  if (at == std::string_view::npos) {
    return "unknown";
  }
  const std::size_t from = at + key.size();
  const std::size_t to = json.find('"', from);
  return std::string(json.substr(from, to == std::string_view::npos ? 0 : to - from));
}

// Joins rendered items as an indented JSON list at `indent` spaces.
std::string list(const std::vector<std::string> &items, std::size_t indent) {
  if (items.empty()) {
    return "[]";
  }
  const std::string pad(indent + 2, ' ');
  std::string out = "[\n";
  for (std::size_t i = 0; i < items.size(); ++i) {
    out += pad + items[i] + (i + 1 < items.size() ? ",\n" : "\n");
  }
  return out + std::string(indent, ' ') + "]";
}

std::string contract() {
  std::vector<std::string> slots;
  slots.reserve(boundary_slot_count);
  for (std::size_t i = 0; i < boundary_slot_count; ++i) {
    slots.push_back(quote(slot_name(static_cast<boundary_slot>(i))));
  }
  std::vector<std::string> codes;
  for (const error_code_info &c : error_code_table()) {
    codes.push_back(std::format(R"({{"area": {}, "name": {}, "number": {}}})", quote(c.area),
                                quote(c.name), c.number));
  }
  std::vector<std::string> fps;
  for (const failpoint_info &f : failpoint_table()) {
    fps.push_back(std::format(R"({{"id": {}, "name": {}, "site": {}}})", quote(f.id), quote(f.name),
                              quote(f.site)));
  }
  const std::vector<std::pair<std::string_view, std::string>> evidence = {
      {"demo-report.json", "sl-runtime-gate/1"},
      {"f5.json", "sl-runtime-f5/1"},
      {"fault-450.json", format_of(to_json(fault_report{}))},
      {"hashes.json", std::string(demo::hashes_format)},
      {"memreport.json", std::string(mem::memreport_format)},
      {"schedule.json", "sl-runtime-schedule/1"},
  };
  std::vector<std::string> files;
  files.reserve(evidence.size());
  for (const auto &[file, format] : evidence) {
    files.push_back(std::format(R"({{"file": {}, "format": {}}})", quote(file), quote(format)));
  }
  std::vector<std::string> lims;
  for (const auto &[name, value] : limits()) {
    lims.push_back(std::format("{}: {}", quote(name), value));
  }
  std::vector<std::string> phases;
  phases.reserve(phase_count);
  for (std::size_t i = 0; i < phase_count; ++i) {
    phases.push_back(std::format(R"({{"index": {}, "phase": {}}})", i,
                                 quote(ecs::detail::phase_name(static_cast<phase>(i)))));
  }
  std::vector<std::string> states;
  std::vector<std::string> events;
  std::vector<std::string> transitions;
  events.reserve(rt_event_count);
  states.reserve(rt_state_count);
  for (std::size_t e = 0; e < rt_event_count; ++e) {
    events.push_back(quote(to_string(static_cast<rt_event>(e))));
  }
  for (std::size_t s = 0; s < rt_state_count; ++s) {
    const auto from = static_cast<rt_state>(s);
    states.push_back(quote(to_string(from)));
    for (std::size_t e = 0; e < rt_event_count; ++e) {
      const auto ev = static_cast<rt_event>(e);
      if (const auto to = next_state(from, ev); to) {
        transitions.push_back(std::format("[{}, {}, {}]", quote(to_string(from)),
                                          quote(to_string(ev)), quote(to_string(*to))));
      }
    }
  }
  std::string out = "{\n";
  out += "  \"boundary_slots\": " + list(slots, 2) + ",\n";
  out += "  \"error_codes\": " + list(codes, 2) + ",\n";
  out += "  \"failpoints\": " + list(fps, 2) + ",\n";
  out += "  \"format\": \"sl-runtime-contract/1\",\n";
  out += "  \"gate_evidence\": " + list(files, 2) + ",\n";
  out += "  \"limits\": {\n";
  for (std::size_t i = 0; i < lims.size(); ++i) {
    out += "    " + lims[i] + (i + 1 < lims.size() ? ",\n" : "\n");
  }
  out += "  },\n";
  out += "  \"phases\": " + list(phases, 2) + ",\n";
  out += "  \"rt_states\": {\n";
  out += "    \"events\": " + list(events, 4) + ",\n";
  out += "    \"states\": " + list(states, 4) + ",\n";
  out += "    \"transitions\": " + list(transitions, 4) + "\n";
  out += "  },\n";
  out += std::format("  \"table_versions\": {{\"error_codes\": {}, \"failpoints\": {}}}\n",
                     error_code_table_version, failpoint_table_version);
  return out + "}\n";
}

} // namespace

// engine/runtime has no try or catch (R-RT-THROW): an allocation or std::format failure while
// rendering terminates, as everywhere in the runtime (ADR-0009).
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char ** /*argv*/) {
  if (argc != 1) {
    std::fputs("usage: sl_runtime_dump_contract   (no argument; writes the JSON to stdout)\n",
               stderr);
    return 2;
  }
  const std::string text = contract();
  std::fwrite(text.data(), 1, text.size(), stdout);
  return std::fflush(stdout) == 0 ? 0 : 2;
}
