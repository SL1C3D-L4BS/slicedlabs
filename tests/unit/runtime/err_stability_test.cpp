// RT-INV-ERR-03 (SL-ARCH-003 ARCH3-R017, ADR-0022): every errc number below 6000 equals the
// committed list of the pre-Volume-05 enumerators (no renumbering). The list below is the golden:
// every enumerator of sl/core/error.hpp before WO-076 (ok, core, platform, graphics, asset and
// fabric), name and number. It changes never; a new code below 6000 belongs to its own domain
// work order and is added here together with its ADR.

#include "sl/core/error.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>

namespace {

using sl::errc;

struct golden_row {
  std::string_view name;
  std::uint32_t number = 0;
};

constexpr auto golden = std::to_array<golden_row>({
    {.name = "ok", .number = 0},
    {.name = "invalid_argument", .number = 1000},
    {.name = "out_of_range", .number = 1001},
    {.name = "not_found", .number = 1002},
    {.name = "already_exists", .number = 1003},
    {.name = "unsupported", .number = 1004},
    {.name = "internal", .number = 1005},
    {.name = "platform_failure", .number = 2000},
    {.name = "io_error", .number = 2001},
    {.name = "permission_denied", .number = 2002},
    {.name = "timed_out", .number = 2003},
    {.name = "graphics_failure", .number = 3000},
    {.name = "device_lost", .number = 3001},
    {.name = "out_of_device_memory", .number = 3002},
    {.name = "unsupported_format", .number = 3003},
    {.name = "asset_failure", .number = 4000},
    {.name = "asset_not_found", .number = 4001},
    {.name = "asset_corrupt", .number = 4002},
    {.name = "asset_version_mismatch", .number = 4003},
    {.name = "fabric_failure", .number = 5000},
    {.name = "id_parse", .number = 5001},
    {.name = "id_class_mismatch", .number = 5002},
    {.name = "id_reuse", .number = 5003},
    {.name = "entropy_failure", .number = 5004},
    {.name = "object_corrupt", .number = 5010},
    {.name = "object_missing", .number = 5011},
    {.name = "ref_cas_failed", .number = 5012},
    {.name = "store_io", .number = 5013},
    {.name = "format_unsupported", .number = 5014},
    {.name = "read_only_newer", .number = 5015},
    {.name = "decode_noncanonical", .number = 5020},
    {.name = "decode_truncated", .number = 5021},
    {.name = "decode_limit", .number = 5022},
    {.name = "decode_bad_utf8", .number = 5023},
    {.name = "decode_bad_float", .number = 5024},
    {.name = "decode_unknown_tag", .number = 5025},
    {.name = "decode_unknown_field", .number = 5026},
    {.name = "encode_invalid", .number = 5027},
    {.name = "text_syntax", .number = 5028},
    {.name = "type_mismatch", .number = 5040},
    {.name = "schema_invalid", .number = 5041},
    {.name = "schema_frozen", .number = 5042},
    {.name = "schema_unknown", .number = 5043},
    {.name = "schema_incompatible", .number = 5044},
    {.name = "migration_failed", .number = 5050},
    {.name = "migration_missing_step", .number = 5051},
    {.name = "migration_lossy", .number = 5052},
    {.name = "ref_dangling", .number = 5055},
    {.name = "ref_unresolved", .number = 5056},
    {.name = "ref_wrong_type", .number = 5057},
    {.name = "dependency_cycle", .number = 5058},
    {.name = "ownership_conflict", .number = 5060},
    {.name = "ownership_orphan", .number = 5061},
    {.name = "authority_denied", .number = 5062},
    {.name = "registry_frozen", .number = 5063},
    {.name = "provenance_incomplete", .number = 5065},
    {.name = "promotion_denied", .number = 5066},
    {.name = "validation_failed", .number = 5070},
    {.name = "stale_base", .number = 5071},
    {.name = "conflict", .number = 5072},
    {.name = "locked", .number = 5073},
    {.name = "tx_aborted", .number = 5074},
    {.name = "tx_closed", .number = 5075},
    {.name = "invariant_violated", .number = 5076},
    {.name = "commit_indeterminate", .number = 5077},
    {.name = "idempotency_mismatch", .number = 5078},
    {.name = "undo_empty", .number = 5090},
    {.name = "undo_barrier", .number = 5091},
    {.name = "undo_conflict", .number = 5092},
    {.name = "compensation_failed", .number = 5093},
    {.name = "journal_corrupt", .number = 5095},
    {.name = "recovery_required", .number = 5096},
    {.name = "writer_locked", .number = 5097},
    {.name = "quarantined", .number = 5098},
    {.name = "checkpoint_invalid", .number = 5099},
    {.name = "store_corrupt", .number = 5100},
    {.name = "manifest_invalid", .number = 5110},
    {.name = "manifest_missing", .number = 5111},
    {.name = "module_unmet", .number = 5112},
    {.name = "reader_too_old", .number = 5113},
    {.name = "capability_unknown", .number = 5114},
    {.name = "event_invalid", .number = 5120},
});

} // namespace

TEST_CASE("RT-INV-ERR-03: every pre-Volume-05 enumerator keeps its number and name") {
  CHECK(golden.size() == 82);
  for (const golden_row &row : golden) {
    INFO(std::string(row.name) << " = " << row.number);
    CHECK(sl::to_string(static_cast<errc>(row.number)) == row.name);
  }
  CHECK(static_cast<std::uint32_t>(errc::ok) == 0);
  CHECK(static_cast<std::uint32_t>(errc::invalid_argument) == 1000);
  CHECK(static_cast<std::uint32_t>(errc::io_error) == 2001);
  CHECK(static_cast<std::uint32_t>(errc::timed_out) == 2003);
  CHECK(static_cast<std::uint32_t>(errc::event_invalid) == 5120);
}

TEST_CASE("RT-INV-ERR-03: no number below 6000 is named except the committed list") {
  std::set<std::uint32_t> listed;
  for (const golden_row &row : golden) {
    CHECK(listed.insert(row.number).second);
  }
  std::size_t named = 0;
  for (std::uint32_t n = 0; n < 6000; ++n) {
    if (sl::to_string(static_cast<errc>(n)) == "unknown") {
      continue;
    }
    INFO("number " << n);
    CHECK(listed.contains(n));
    ++named;
  }
  CHECK(named == golden.size());
}
