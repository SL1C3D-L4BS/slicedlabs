// RT-INV-FLT-02 (SL-ARCH-003 ARCH3-R020, ADR-0022): failpoint_table() lists RFP0-RFP10 with the
// names and sites of the `failpoints` table, in order. The expected rows are a copy of the sl-data
// table of SL-ARCH-003; the table is constexpr data, the same in every build type.

#include "sl/runtime/faults.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace {

using sl::runtime::failpoint;

struct expected_row {
  std::string_view id;
  std::string_view name;
  std::string_view site;
};

constexpr auto expected = std::to_array<expected_row>({
    {.id = "RFP0",
     .name = "alloc.fail",
     .site = "mem::resource::try_allocate (Nth call or per tag)"},
    {.id = "RFP1", .name = "thread.spawn", .site = "platform thread_factory::spawn"},
    {.id = "RFP2", .name = "job.error", .site = "jobs::job_system before running a job body"},
    {.id = "RFP3", .name = "job.fatal", .site = "jobs::job_system before running a job body"},
    {.id = "RFP4",
     .name = "worker.stall",
     .site = "jobs worker loop at before_pop, after_steal, before_complete and in_wait"},
    {.id = "RFP5", .name = "system.fail", .site = "ecs::system_scheduler after a system body"},
    {.id = "RFP6", .name = "clock.jump", .site = "time::frame_clock::sample"},
    {.id = "RFP7", .name = "event.overflow", .site = "event_channel<E>::write"},
    {.id = "RFP8", .name = "snapshot.io", .site = "snapshot save, once before atomic_write"},
    {.id = "RFP9",
     .name = "suspend.request",
     .site = "runtime at each boundary slot and mid-phase"},
    {.id = "RFP10", .name = "entity.exhaust", .site = "ecs::entity_allocator::create"},
});

} // namespace

TEST_CASE("RT-INV-FLT-02: failpoint_table() lists RFP0-RFP10 with the table's names and sites") {
  const auto table = sl::runtime::failpoint_table();
  const std::span<const expected_row> rows(expected);
  REQUIRE(table.size() == rows.size());
  CHECK(sl::runtime::failpoint_count == 11);
  for (std::size_t i = 0; i < rows.size(); ++i) {
    INFO("row " << i);
    CHECK(table[i].id == rows[i].id);
    CHECK(table[i].id == "RFP" + std::to_string(i));
    CHECK(table[i].name == rows[i].name);
    CHECK(table[i].site == rows[i].site);
  }
  CHECK(sl::runtime::failpoint_table_version == 1);
}

TEST_CASE("RT-INV-FLT-02: the failpoint enumerators are the RFP indices") {
  CHECK(static_cast<std::size_t>(failpoint::rfp0) == 0);
  CHECK(static_cast<std::size_t>(failpoint::rfp1) == 1);
  CHECK(static_cast<std::size_t>(failpoint::rfp2) == 2);
  CHECK(static_cast<std::size_t>(failpoint::rfp3) == 3);
  CHECK(static_cast<std::size_t>(failpoint::rfp4) == 4);
  CHECK(static_cast<std::size_t>(failpoint::rfp5) == 5);
  CHECK(static_cast<std::size_t>(failpoint::rfp6) == 6);
  CHECK(static_cast<std::size_t>(failpoint::rfp7) == 7);
  CHECK(static_cast<std::size_t>(failpoint::rfp8) == 8);
  CHECK(static_cast<std::size_t>(failpoint::rfp9) == 9);
  CHECK(static_cast<std::size_t>(failpoint::rfp10) == 10);
}
