#include <doctest/doctest.h>

#include <array>
#include <string>
#include <string_view>

#include <sl/core/error.hpp>

namespace {

sl::result<int> parse_digit(char c) {
  if (c < '0' || c > '9') {
    return sl::make_error(sl::errc::invalid_argument, std::string("not a digit: ") + c);
  }
  return c - '0';
}

sl::result<void> require_even(int v) {
  if (v % 2 != 0) {
    return sl::make_error(sl::errc::out_of_range, "odd");
  }
  return {};
}

} // namespace

static_assert(sl::domain(sl::errc::ok) == sl::error_domain::none);
static_assert(sl::domain(sl::errc::internal) == sl::error_domain::core);
static_assert(sl::to_string(sl::errc::device_lost) == "device_lost");
static_assert(sl::domain(sl::errc::fabric_failure) == sl::error_domain::fabric);
static_assert(sl::to_string(sl::errc::event_invalid) == "event_invalid");

TEST_CASE("error codes map to their domain by numeric range") {
  using sl::errc;
  using sl::error_domain;
  CHECK(sl::domain(errc::ok) == error_domain::none);
  for (const errc c : {errc::invalid_argument, errc::out_of_range, errc::not_found,
                       errc::already_exists, errc::unsupported, errc::internal}) {
    CHECK(sl::domain(c) == error_domain::core);
    CHECK(static_cast<unsigned>(c) >= 1000);
    CHECK(static_cast<unsigned>(c) <= 1999);
  }
  for (const errc c :
       {errc::platform_failure, errc::io_error, errc::permission_denied, errc::timed_out}) {
    CHECK(sl::domain(c) == error_domain::platform);
  }
  for (const errc c : {errc::graphics_failure, errc::device_lost, errc::out_of_device_memory,
                       errc::unsupported_format}) {
    CHECK(sl::domain(c) == error_domain::graphics);
  }
  for (const errc c : {errc::asset_failure, errc::asset_not_found, errc::asset_corrupt,
                       errc::asset_version_mismatch}) {
    CHECK(sl::domain(c) == error_domain::asset);
  }
  CHECK(sl::domain(static_cast<errc>(9000)) == error_domain::unknown);
  CHECK(sl::to_string(error_domain::graphics) == "graphics");
  CHECK(sl::to_string(error_domain::none) == "none");
}

TEST_CASE("every error code has a name; unknown values say so") {
  using sl::errc;
  CHECK(sl::to_string(errc::ok) == "ok");
  CHECK(sl::to_string(errc::invalid_argument) == "invalid_argument");
  CHECK(sl::to_string(errc::not_found) == "not_found");
  CHECK(sl::to_string(errc::io_error) == "io_error");
  CHECK(sl::to_string(errc::asset_version_mismatch) == "asset_version_mismatch");
  for (const errc c :
       {errc::out_of_range, errc::already_exists, errc::unsupported, errc::internal,
        errc::platform_failure, errc::permission_denied, errc::timed_out, errc::graphics_failure,
        errc::device_lost, errc::out_of_device_memory, errc::unsupported_format,
        errc::asset_failure, errc::asset_not_found, errc::asset_corrupt}) {
    CHECK(sl::to_string(c) != "unknown");
  }
  CHECK(sl::to_string(static_cast<errc>(1999)) == "unknown");
}

TEST_CASE("result<T> carries a value or an error with its source location") {
  const sl::result<int> ok = parse_digit('7');
  REQUIRE(ok.has_value());
  CHECK(*ok == 7);

  const sl::result<int> bad = parse_digit('x');
  REQUIRE_FALSE(bad.has_value());
  CHECK(bad.error().code == sl::errc::invalid_argument);
  CHECK(bad.error().message == "not a digit: x");
  CHECK(std::string_view{bad.error().where.file_name()}.ends_with("error_test.cpp"));
  CHECK(bad.error().where.line() > 0);

  const auto doubled = parse_digit('4').transform([](int v) { return v * 2; });
  CHECK(doubled.value_or(-1) == 8);
  const auto chained =
      parse_digit('3').and_then([](int v) { return parse_digit(v == 3 ? 'q' : '1'); });
  CHECK(chained.error().code == sl::errc::invalid_argument);
  const auto recovered =
      parse_digit('?').or_else([](const sl::error &) -> sl::result<int> { return 0; });
  CHECK(recovered.value() == 0);
}

TEST_CASE("result<void> and describe()") {
  CHECK(require_even(4).has_value());
  const sl::result<void> odd = require_even(3);
  REQUIRE_FALSE(odd.has_value());
  const std::string text = sl::describe(odd.error());
  CHECK(text.starts_with("core.out_of_range (1001): odd [error_test.cpp:"));
  CHECK(text.ends_with(']'));
}

// ADR-0010: the fabric domain 5000-5999, every code with its fixed number and name.
TEST_CASE("fabric error codes: domain, fixed numbers, names and describe()") {
  using sl::errc;
  struct row {
    errc code{};
    std::string_view name;
    unsigned number = 0;
  };
  const auto rows = std::to_array<row>({
      {.code = errc::fabric_failure, .name = "fabric_failure", .number = 5000},
      {.code = errc::id_parse, .name = "id_parse", .number = 5001},
      {.code = errc::id_class_mismatch, .name = "id_class_mismatch", .number = 5002},
      {.code = errc::id_reuse, .name = "id_reuse", .number = 5003},
      {.code = errc::entropy_failure, .name = "entropy_failure", .number = 5004},
      {.code = errc::object_corrupt, .name = "object_corrupt", .number = 5010},
      {.code = errc::object_missing, .name = "object_missing", .number = 5011},
      {.code = errc::ref_cas_failed, .name = "ref_cas_failed", .number = 5012},
      {.code = errc::store_io, .name = "store_io", .number = 5013},
      {.code = errc::format_unsupported, .name = "format_unsupported", .number = 5014},
      {.code = errc::read_only_newer, .name = "read_only_newer", .number = 5015},
      {.code = errc::decode_noncanonical, .name = "decode_noncanonical", .number = 5020},
      {.code = errc::decode_truncated, .name = "decode_truncated", .number = 5021},
      {.code = errc::decode_limit, .name = "decode_limit", .number = 5022},
      {.code = errc::decode_bad_utf8, .name = "decode_bad_utf8", .number = 5023},
      {.code = errc::decode_bad_float, .name = "decode_bad_float", .number = 5024},
      {.code = errc::decode_unknown_tag, .name = "decode_unknown_tag", .number = 5025},
      {.code = errc::decode_unknown_field, .name = "decode_unknown_field", .number = 5026},
      {.code = errc::encode_invalid, .name = "encode_invalid", .number = 5027},
      {.code = errc::text_syntax, .name = "text_syntax", .number = 5028},
      {.code = errc::type_mismatch, .name = "type_mismatch", .number = 5040},
      {.code = errc::schema_invalid, .name = "schema_invalid", .number = 5041},
      {.code = errc::schema_frozen, .name = "schema_frozen", .number = 5042},
      {.code = errc::schema_unknown, .name = "schema_unknown", .number = 5043},
      {.code = errc::schema_incompatible, .name = "schema_incompatible", .number = 5044},
      {.code = errc::migration_failed, .name = "migration_failed", .number = 5050},
      {.code = errc::migration_missing_step, .name = "migration_missing_step", .number = 5051},
      {.code = errc::migration_lossy, .name = "migration_lossy", .number = 5052},
      {.code = errc::ref_dangling, .name = "ref_dangling", .number = 5055},
      {.code = errc::ref_unresolved, .name = "ref_unresolved", .number = 5056},
      {.code = errc::ref_wrong_type, .name = "ref_wrong_type", .number = 5057},
      {.code = errc::dependency_cycle, .name = "dependency_cycle", .number = 5058},
      {.code = errc::ownership_conflict, .name = "ownership_conflict", .number = 5060},
      {.code = errc::ownership_orphan, .name = "ownership_orphan", .number = 5061},
      {.code = errc::authority_denied, .name = "authority_denied", .number = 5062},
      {.code = errc::registry_frozen, .name = "registry_frozen", .number = 5063},
      {.code = errc::provenance_incomplete, .name = "provenance_incomplete", .number = 5065},
      {.code = errc::promotion_denied, .name = "promotion_denied", .number = 5066},
      {.code = errc::validation_failed, .name = "validation_failed", .number = 5070},
      {.code = errc::stale_base, .name = "stale_base", .number = 5071},
      {.code = errc::conflict, .name = "conflict", .number = 5072},
      {.code = errc::locked, .name = "locked", .number = 5073},
      {.code = errc::tx_aborted, .name = "tx_aborted", .number = 5074},
      {.code = errc::tx_closed, .name = "tx_closed", .number = 5075},
      {.code = errc::invariant_violated, .name = "invariant_violated", .number = 5076},
      {.code = errc::commit_indeterminate, .name = "commit_indeterminate", .number = 5077},
      {.code = errc::idempotency_mismatch, .name = "idempotency_mismatch", .number = 5078},
      {.code = errc::undo_empty, .name = "undo_empty", .number = 5090},
      {.code = errc::undo_barrier, .name = "undo_barrier", .number = 5091},
      {.code = errc::undo_conflict, .name = "undo_conflict", .number = 5092},
      {.code = errc::compensation_failed, .name = "compensation_failed", .number = 5093},
      {.code = errc::journal_corrupt, .name = "journal_corrupt", .number = 5095},
      {.code = errc::recovery_required, .name = "recovery_required", .number = 5096},
      {.code = errc::writer_locked, .name = "writer_locked", .number = 5097},
      {.code = errc::quarantined, .name = "quarantined", .number = 5098},
      {.code = errc::checkpoint_invalid, .name = "checkpoint_invalid", .number = 5099},
      {.code = errc::store_corrupt, .name = "store_corrupt", .number = 5100},
      {.code = errc::manifest_invalid, .name = "manifest_invalid", .number = 5110},
      {.code = errc::manifest_missing, .name = "manifest_missing", .number = 5111},
      {.code = errc::module_unmet, .name = "module_unmet", .number = 5112},
      {.code = errc::reader_too_old, .name = "reader_too_old", .number = 5113},
      {.code = errc::capability_unknown, .name = "capability_unknown", .number = 5114},
      {.code = errc::event_invalid, .name = "event_invalid", .number = 5120},
  });
  CHECK(rows.size() == 63);
  for (const row &r : rows) {
    CAPTURE(r.name);
    CHECK(static_cast<unsigned>(r.code) == r.number);
    CHECK(sl::domain(r.code) == sl::error_domain::fabric);
    CHECK(sl::to_string(r.code) == r.name);
    CHECK(r.number >= 5000);
    CHECK(r.number <= 5124);
  }
  CHECK(sl::domain(static_cast<errc>(5999)) == sl::error_domain::fabric);
  CHECK(sl::to_string(static_cast<errc>(5125)) == "unknown");
  CHECK(sl::to_string(sl::error_domain::fabric) == "fabric");
  CHECK(sl::to_string(sl::error_domain::unknown) == "unknown");

  const sl::error e{.code = errc::id_parse, .message = "bad prefix", .where = {}};
  CHECK(sl::describe(e).starts_with("fabric.id_parse (5001): bad prefix ["));
  const auto made = sl::make_error(errc::decode_truncated, "R-SER-TRUNC at offset 3");
  CHECK(
      sl::describe(made.error())
          .starts_with("fabric.decode_truncated (5021): R-SER-TRUNC at offset 3 [error_test.cpp:"));
}
