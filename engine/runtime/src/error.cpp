// The runtime error-code table (WO-076, SL-ARCH-003 `error_codes`, ADR-0022). The rows are the
// sl-data table in its order; the static_assert below binds every row to its sl::errc enumerator
// (number and to_string name) at compile time. Immutable: constexpr data only.

#include "sl/runtime/error.hpp"

#include "sl/core/error.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace sl::runtime {

namespace {

constexpr auto error_codes = std::to_array<error_code_info>({
    {.name = "runtime_failure", .number = 6000, .area = "general"},
    {.name = "invalid_state", .number = 6001, .area = "lifecycle"},
    {.name = "wrong_thread", .number = 6002, .area = "lifecycle"},
    {.name = "restart_required", .number = 6003, .area = "lifecycle"},
    {.name = "recovery_failed", .number = 6004, .area = "lifecycle"},
    {.name = "swap_rejected", .number = 6005, .area = "lifecycle"},
    {.name = "runtime_faulted", .number = 6006, .area = "lifecycle"},
    {.name = "thread_spawn_failed", .number = 6010, .area = "platform"},
    {.name = "platform_unavailable", .number = 6011, .area = "platform"},
    {.name = "out_of_memory", .number = 6020, .area = "memory"},
    {.name = "bad_alignment", .number = 6021, .area = "memory"},
    {.name = "arena_exhausted", .number = 6022, .area = "memory"},
    {.name = "leak_detected", .number = 6023, .area = "memory"},
    {.name = "tag_registry_frozen", .number = 6024, .area = "memory"},
    {.name = "tag_reserved", .number = 6025, .area = "memory"},
    {.name = "job_capacity", .number = 6030, .area = "jobs"},
    {.name = "job_cancelled", .number = 6031, .area = "jobs"},
    {.name = "job_failed", .number = 6032, .area = "jobs"},
    {.name = "scheduler_stopped", .number = 6033, .area = "jobs"},
    {.name = "deadlock_suspected", .number = 6034, .area = "jobs"},
    {.name = "stale_job", .number = 6035, .area = "jobs"},
    {.name = "invalid_tick_rate", .number = 6040, .area = "time"},
    {.name = "schedule_frozen", .number = 6045, .area = "frame"},
    {.name = "stale_entity", .number = 6050, .area = "ecs"},
    {.name = "entity_capacity", .number = 6051, .area = "ecs"},
    {.name = "component_unknown", .number = 6052, .area = "ecs"},
    {.name = "component_missing", .number = 6053, .area = "ecs"},
    {.name = "component_exists", .number = 6054, .area = "ecs"},
    {.name = "component_invalid", .number = 6055, .area = "ecs"},
    {.name = "component_registry_frozen", .number = 6056, .area = "ecs"},
    {.name = "archetype_limit", .number = 6057, .area = "ecs"},
    {.name = "structural_change_forbidden", .number = 6058, .area = "ecs"},
    {.name = "query_invalid", .number = 6059, .area = "ecs"},
    {.name = "schedule_cycle", .number = 6070, .area = "scheduling"},
    {.name = "undeclared_access", .number = 6071, .area = "scheduling"},
    {.name = "access_conflict", .number = 6072, .area = "scheduling"},
    {.name = "system_unknown", .number = 6073, .area = "scheduling"},
    {.name = "type_unknown", .number = 6080, .area = "reflection"},
    {.name = "field_unknown", .number = 6081, .area = "reflection"},
    {.name = "reflect_mismatch", .number = 6082, .area = "reflection"},
    {.name = "type_registry_frozen", .number = 6083, .area = "reflection"},
    {.name = "component_schema_mismatch", .number = 6084, .area = "reflection"},
    {.name = "event_overflow", .number = 6090, .area = "events"},
    {.name = "event_type_unknown", .number = 6091, .area = "events"},
    {.name = "event_payload_invalid", .number = 6092, .area = "events"},
    {.name = "snapshot_corrupt", .number = 6100, .area = "snapshot"},
    {.name = "snapshot_version", .number = 6101, .area = "snapshot"},
    {.name = "snapshot_schema_mismatch", .number = 6102, .area = "snapshot"},
    {.name = "snapshot_mid_tick", .number = 6103, .area = "snapshot"},
    {.name = "snapshot_abi_mismatch", .number = 6104, .area = "snapshot"},
    {.name = "snapshot_contributor_unknown", .number = 6105, .area = "snapshot"},
    {.name = "link_observation_rejected", .number = 6110, .area = "link"},
    {.name = "link_transient_component", .number = 6111, .area = "link"},
    {.name = "projection_stale", .number = 6112, .area = "link"},
});

// Every row names its own enumerator: the number lies in the runtime domain and to_string of it is
// the row's name.
constexpr bool rows_match_errc() noexcept {
  return std::ranges::all_of(error_codes, [](const error_code_info &row) {
    const auto code = static_cast<errc>(row.number);
    return domain(code) == error_domain::runtime && to_string(code) == row.name;
  });
}
static_assert(rows_match_errc(), "error_codes row and sl::errc disagree");

} // namespace

std::span<const error_code_info> error_code_table() noexcept { return error_codes; }

} // namespace sl::runtime
