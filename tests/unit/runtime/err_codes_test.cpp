// RT-INV-ERR-01 (SL-ARCH-003 ARCH3-R017, ADR-0022): the runtime errc values are unique, lie in
// 6000-6999, and to_string round trips every name of the `error_codes` table. The expected rows
// below are a copy of the sl-data table of SL-ARCH-003 (name, number, area, in table order); the
// contract lock (WO-100) compares the dump with the document itself.

#include "sl/core/error.hpp"
#include "sl/runtime/error.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <string_view>

namespace sl::runtime {
namespace {
// An unqualified make_error inside sl::runtime: the const char * overload wins over sl::make_error
// (found by ADL on errc), so runtime code can write it without qualification.
result<int> refuse_start() { return make_error(errc::invalid_state, "start after shutdown"); }
} // namespace
} // namespace sl::runtime

namespace {

using sl::errc;
using sl::runtime::error_code_info;

struct expected_row {
  std::string_view name;
  std::uint32_t number = 0;
  std::string_view area;
};

constexpr auto expected = std::to_array<expected_row>({
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

} // namespace

TEST_CASE("RT-INV-ERR-01: error_code_table() is the SL-ARCH-003 error_codes table, in order") {
  const auto table = sl::runtime::error_code_table();
  const std::span<const expected_row> rows(expected);
  REQUIRE(table.size() == rows.size());
  for (std::size_t i = 0; i < rows.size(); ++i) {
    INFO("row " << i << " " << std::string(rows[i].name));
    CHECK(table[i].name == rows[i].name);
    CHECK(table[i].number == rows[i].number);
    CHECK(table[i].area == rows[i].area);
  }
  CHECK(sl::runtime::error_code_table_version == 1);
}

TEST_CASE("RT-INV-ERR-01: runtime codes are unique and lie in 6000-6999") {
  std::set<std::uint32_t> numbers;
  std::set<std::string_view> names;
  for (const error_code_info &row : sl::runtime::error_code_table()) {
    INFO(std::string(row.name));
    CHECK(row.number >= 6000);
    CHECK(row.number <= 6999);
    CHECK(numbers.insert(row.number).second);
    CHECK(names.insert(row.name).second);
  }
  CHECK(numbers.size() == expected.size());
}

TEST_CASE("RT-INV-ERR-01: to_string round trips every name and no other 6000-6999 value is named") {
  std::set<std::uint32_t> listed;
  for (const expected_row &row : expected) {
    const auto code = static_cast<errc>(row.number);
    INFO(std::string(row.name));
    CHECK(sl::to_string(code) == row.name);
    CHECK(sl::runtime::is_runtime_code(code));
    listed.insert(row.number);
  }
  // Reserved numbers (6007-6009, ..., 6113-6999) are no enumerator and no runtime code.
  for (std::uint32_t n = 6000; n <= 6999; ++n) {
    const auto code = static_cast<errc>(n);
    if (listed.contains(n)) {
      continue;
    }
    INFO("number " << n);
    CHECK(sl::to_string(code) == "unknown");
    CHECK_FALSE(sl::runtime::is_runtime_code(code));
  }
  CHECK_FALSE(sl::runtime::is_runtime_code(errc::ok));
  CHECK_FALSE(sl::runtime::is_runtime_code(errc::invalid_argument));
  CHECK_FALSE(sl::runtime::is_runtime_code(errc::event_invalid));
}

TEST_CASE("RT-INV-ERR-01: make_error carries the runtime code, the message and the call site") {
  const auto r = sl::runtime::refuse_start();
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == errc::invalid_state);
  CHECK(r.error().message == "start after shutdown");
  const std::string text = sl::describe(r.error());
  CHECK(text.starts_with("runtime.invalid_state (6001): start after shutdown ["));
  constexpr std::string_view view = "a view";
  const auto e = sl::runtime::make_error(errc::job_capacity, view);
  CHECK(e.error().code == errc::job_capacity);
  CHECK(e.error().message == "a view");
}
