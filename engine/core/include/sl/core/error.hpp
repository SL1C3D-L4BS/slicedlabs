#pragma once

// Error-code taxonomy and the result type (WO-030, SL-EP-056, ADR-0009). Engine code reports
// recoverable failures as values, `sl::result<T>` (std::expected), never by throwing.
//
// Codes are grouped by domain in numeric ranges: core 1000-1999, platform 2000-2999,
// graphics 3000-3999, asset 4000-4999, fabric 5000-5999 (ADR-0010; 5125-5999 reserved), runtime
// 6000-6999 (SL-ARCH-003 `error_codes`, ADR-0022; the gaps are reserved); ok is 0.
// A code keeps its number for life.

#include <cstdint>
#include <expected>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace sl {

enum class errc : std::uint32_t {
  ok = 0,
  // core 1000-1999
  invalid_argument = 1000,
  out_of_range = 1001,
  not_found = 1002,
  already_exists = 1003,
  unsupported = 1004,
  internal = 1005,
  // platform 2000-2999
  platform_failure = 2000,
  io_error = 2001,
  permission_denied = 2002,
  timed_out = 2003,
  // graphics 3000-3999
  graphics_failure = 3000,
  device_lost = 3001,
  out_of_device_memory = 3002,
  unsupported_format = 3003,
  // asset 4000-4999
  asset_failure = 4000,
  asset_not_found = 4001,
  asset_corrupt = 4002,
  asset_version_mismatch = 4003,
  // fabric general 5000
  fabric_failure = 5000,
  // fabric identity 5001-5009
  id_parse = 5001,
  id_class_mismatch = 5002,
  id_reuse = 5003,
  entropy_failure = 5004,
  // fabric objects and store 5010-5019
  object_corrupt = 5010,
  object_missing = 5011,
  ref_cas_failed = 5012,
  store_io = 5013,
  format_unsupported = 5014,
  read_only_newer = 5015,
  // fabric codec 5020-5039
  decode_noncanonical = 5020,
  decode_truncated = 5021,
  decode_limit = 5022,
  decode_bad_utf8 = 5023,
  decode_bad_float = 5024,
  decode_unknown_tag = 5025,
  decode_unknown_field = 5026,
  encode_invalid = 5027,
  text_syntax = 5028,
  // fabric types and schemas 5040-5049
  type_mismatch = 5040,
  schema_invalid = 5041,
  schema_frozen = 5042,
  schema_unknown = 5043,
  schema_incompatible = 5044,
  // fabric migration 5050-5054
  migration_failed = 5050,
  migration_missing_step = 5051,
  migration_lossy = 5052,
  // fabric references 5055-5059
  ref_dangling = 5055,
  ref_unresolved = 5056,
  ref_wrong_type = 5057,
  dependency_cycle = 5058,
  // fabric ownership and authority 5060-5064
  ownership_conflict = 5060,
  ownership_orphan = 5061,
  authority_denied = 5062,
  registry_frozen = 5063,
  // fabric provenance 5065-5069
  provenance_incomplete = 5065,
  promotion_denied = 5066,
  // fabric transactions 5070-5089
  validation_failed = 5070,
  stale_base = 5071,
  conflict = 5072,
  locked = 5073,
  tx_aborted = 5074,
  tx_closed = 5075,
  invariant_violated = 5076,
  commit_indeterminate = 5077,
  idempotency_mismatch = 5078,
  // fabric undo 5090-5094
  undo_empty = 5090,
  undo_barrier = 5091,
  undo_conflict = 5092,
  compensation_failed = 5093,
  // fabric recovery and corruption 5095-5109
  journal_corrupt = 5095,
  recovery_required = 5096,
  writer_locked = 5097,
  quarantined = 5098,
  checkpoint_invalid = 5099,
  store_corrupt = 5100,
  // fabric manifest 5110-5119
  manifest_invalid = 5110,
  manifest_missing = 5111,
  module_unmet = 5112,
  reader_too_old = 5113,
  capability_unknown = 5114,
  // fabric events 5120-5124
  event_invalid = 5120,
  // runtime 6000-6999 (SL-ARCH-003, ADR-0022)
  // runtime general
  runtime_failure = 6000,
  // runtime lifecycle
  invalid_state = 6001,
  wrong_thread = 6002,
  restart_required = 6003,
  recovery_failed = 6004,
  swap_rejected = 6005,
  runtime_faulted = 6006,
  // runtime platform
  thread_spawn_failed = 6010,
  platform_unavailable = 6011,
  // runtime memory
  out_of_memory = 6020,
  bad_alignment = 6021,
  arena_exhausted = 6022,
  leak_detected = 6023,
  tag_registry_frozen = 6024,
  tag_reserved = 6025,
  // runtime jobs
  job_capacity = 6030,
  job_cancelled = 6031,
  job_failed = 6032,
  scheduler_stopped = 6033,
  deadlock_suspected = 6034,
  stale_job = 6035,
  // runtime time
  invalid_tick_rate = 6040,
  // runtime frame
  schedule_frozen = 6045,
  // runtime ecs
  stale_entity = 6050,
  entity_capacity = 6051,
  component_unknown = 6052,
  component_missing = 6053,
  component_exists = 6054,
  component_invalid = 6055,
  component_registry_frozen = 6056,
  archetype_limit = 6057,
  structural_change_forbidden = 6058,
  query_invalid = 6059,
  // runtime scheduling
  schedule_cycle = 6070,
  undeclared_access = 6071,
  access_conflict = 6072,
  system_unknown = 6073,
  // runtime reflection
  type_unknown = 6080,
  field_unknown = 6081,
  reflect_mismatch = 6082,
  type_registry_frozen = 6083,
  component_schema_mismatch = 6084,
  // runtime events
  event_overflow = 6090,
  event_type_unknown = 6091,
  event_payload_invalid = 6092,
  // runtime snapshot
  snapshot_corrupt = 6100,
  snapshot_version = 6101,
  snapshot_schema_mismatch = 6102,
  snapshot_mid_tick = 6103,
  snapshot_abi_mismatch = 6104,
  snapshot_contributor_unknown = 6105,
  // runtime link
  link_observation_rejected = 6110,
  link_transient_component = 6111,
  projection_stale = 6112,
};

// runtime is appended after unknown so no existing domain value moves (ADR-0022).
enum class error_domain : std::uint8_t {
  none,
  core,
  platform,
  graphics,
  asset,
  fabric,
  unknown,
  runtime
};

/// The domain a code belongs to, by its numeric range (ok is `none`).
[[nodiscard]] constexpr error_domain domain(errc code) noexcept {
  const auto n = static_cast<std::uint32_t>(code);
  if (n == 0) {
    return error_domain::none;
  }
  switch (n / 1000) {
  case 1:
    return error_domain::core;
  case 2:
    return error_domain::platform;
  case 3:
    return error_domain::graphics;
  case 4:
    return error_domain::asset;
  case 5:
    return error_domain::fabric;
  case 6:
    return error_domain::runtime;
  default:
    return error_domain::unknown;
  }
}

[[nodiscard]] constexpr std::string_view to_string(error_domain d) noexcept {
  switch (d) {
  case error_domain::none:
    return "none";
  case error_domain::core:
    return "core";
  case error_domain::platform:
    return "platform";
  case error_domain::graphics:
    return "graphics";
  case error_domain::asset:
    return "asset";
  case error_domain::fabric:
    return "fabric";
  case error_domain::unknown:
    return "unknown";
  case error_domain::runtime:
    return "runtime";
  }
  return "unknown";
}

/// The enumerator name ("invalid_argument"); "unknown" for a value that is not an enumerator.
[[nodiscard]] constexpr std::string_view to_string(errc code) noexcept {
  switch (code) {
  case errc::ok:
    return "ok";
  case errc::invalid_argument:
    return "invalid_argument";
  case errc::out_of_range:
    return "out_of_range";
  case errc::not_found:
    return "not_found";
  case errc::already_exists:
    return "already_exists";
  case errc::unsupported:
    return "unsupported";
  case errc::internal:
    return "internal";
  case errc::platform_failure:
    return "platform_failure";
  case errc::io_error:
    return "io_error";
  case errc::permission_denied:
    return "permission_denied";
  case errc::timed_out:
    return "timed_out";
  case errc::graphics_failure:
    return "graphics_failure";
  case errc::device_lost:
    return "device_lost";
  case errc::out_of_device_memory:
    return "out_of_device_memory";
  case errc::unsupported_format:
    return "unsupported_format";
  case errc::asset_failure:
    return "asset_failure";
  case errc::asset_not_found:
    return "asset_not_found";
  case errc::asset_corrupt:
    return "asset_corrupt";
  case errc::asset_version_mismatch:
    return "asset_version_mismatch";
  case errc::fabric_failure:
    return "fabric_failure";
  case errc::id_parse:
    return "id_parse";
  case errc::id_class_mismatch:
    return "id_class_mismatch";
  case errc::id_reuse:
    return "id_reuse";
  case errc::entropy_failure:
    return "entropy_failure";
  case errc::object_corrupt:
    return "object_corrupt";
  case errc::object_missing:
    return "object_missing";
  case errc::ref_cas_failed:
    return "ref_cas_failed";
  case errc::store_io:
    return "store_io";
  case errc::format_unsupported:
    return "format_unsupported";
  case errc::read_only_newer:
    return "read_only_newer";
  case errc::decode_noncanonical:
    return "decode_noncanonical";
  case errc::decode_truncated:
    return "decode_truncated";
  case errc::decode_limit:
    return "decode_limit";
  case errc::decode_bad_utf8:
    return "decode_bad_utf8";
  case errc::decode_bad_float:
    return "decode_bad_float";
  case errc::decode_unknown_tag:
    return "decode_unknown_tag";
  case errc::decode_unknown_field:
    return "decode_unknown_field";
  case errc::encode_invalid:
    return "encode_invalid";
  case errc::text_syntax:
    return "text_syntax";
  case errc::type_mismatch:
    return "type_mismatch";
  case errc::schema_invalid:
    return "schema_invalid";
  case errc::schema_frozen:
    return "schema_frozen";
  case errc::schema_unknown:
    return "schema_unknown";
  case errc::schema_incompatible:
    return "schema_incompatible";
  case errc::migration_failed:
    return "migration_failed";
  case errc::migration_missing_step:
    return "migration_missing_step";
  case errc::migration_lossy:
    return "migration_lossy";
  case errc::ref_dangling:
    return "ref_dangling";
  case errc::ref_unresolved:
    return "ref_unresolved";
  case errc::ref_wrong_type:
    return "ref_wrong_type";
  case errc::dependency_cycle:
    return "dependency_cycle";
  case errc::ownership_conflict:
    return "ownership_conflict";
  case errc::ownership_orphan:
    return "ownership_orphan";
  case errc::authority_denied:
    return "authority_denied";
  case errc::registry_frozen:
    return "registry_frozen";
  case errc::provenance_incomplete:
    return "provenance_incomplete";
  case errc::promotion_denied:
    return "promotion_denied";
  case errc::validation_failed:
    return "validation_failed";
  case errc::stale_base:
    return "stale_base";
  case errc::conflict:
    return "conflict";
  case errc::locked:
    return "locked";
  case errc::tx_aborted:
    return "tx_aborted";
  case errc::tx_closed:
    return "tx_closed";
  case errc::invariant_violated:
    return "invariant_violated";
  case errc::commit_indeterminate:
    return "commit_indeterminate";
  case errc::idempotency_mismatch:
    return "idempotency_mismatch";
  case errc::undo_empty:
    return "undo_empty";
  case errc::undo_barrier:
    return "undo_barrier";
  case errc::undo_conflict:
    return "undo_conflict";
  case errc::compensation_failed:
    return "compensation_failed";
  case errc::journal_corrupt:
    return "journal_corrupt";
  case errc::recovery_required:
    return "recovery_required";
  case errc::writer_locked:
    return "writer_locked";
  case errc::quarantined:
    return "quarantined";
  case errc::checkpoint_invalid:
    return "checkpoint_invalid";
  case errc::store_corrupt:
    return "store_corrupt";
  case errc::manifest_invalid:
    return "manifest_invalid";
  case errc::manifest_missing:
    return "manifest_missing";
  case errc::module_unmet:
    return "module_unmet";
  case errc::reader_too_old:
    return "reader_too_old";
  case errc::capability_unknown:
    return "capability_unknown";
  case errc::event_invalid:
    return "event_invalid";
  case errc::runtime_failure:
    return "runtime_failure";
  case errc::invalid_state:
    return "invalid_state";
  case errc::wrong_thread:
    return "wrong_thread";
  case errc::restart_required:
    return "restart_required";
  case errc::recovery_failed:
    return "recovery_failed";
  case errc::swap_rejected:
    return "swap_rejected";
  case errc::runtime_faulted:
    return "runtime_faulted";
  case errc::thread_spawn_failed:
    return "thread_spawn_failed";
  case errc::platform_unavailable:
    return "platform_unavailable";
  case errc::out_of_memory:
    return "out_of_memory";
  case errc::bad_alignment:
    return "bad_alignment";
  case errc::arena_exhausted:
    return "arena_exhausted";
  case errc::leak_detected:
    return "leak_detected";
  case errc::tag_registry_frozen:
    return "tag_registry_frozen";
  case errc::tag_reserved:
    return "tag_reserved";
  case errc::job_capacity:
    return "job_capacity";
  case errc::job_cancelled:
    return "job_cancelled";
  case errc::job_failed:
    return "job_failed";
  case errc::scheduler_stopped:
    return "scheduler_stopped";
  case errc::deadlock_suspected:
    return "deadlock_suspected";
  case errc::stale_job:
    return "stale_job";
  case errc::invalid_tick_rate:
    return "invalid_tick_rate";
  case errc::schedule_frozen:
    return "schedule_frozen";
  case errc::stale_entity:
    return "stale_entity";
  case errc::entity_capacity:
    return "entity_capacity";
  case errc::component_unknown:
    return "component_unknown";
  case errc::component_missing:
    return "component_missing";
  case errc::component_exists:
    return "component_exists";
  case errc::component_invalid:
    return "component_invalid";
  case errc::component_registry_frozen:
    return "component_registry_frozen";
  case errc::archetype_limit:
    return "archetype_limit";
  case errc::structural_change_forbidden:
    return "structural_change_forbidden";
  case errc::query_invalid:
    return "query_invalid";
  case errc::schedule_cycle:
    return "schedule_cycle";
  case errc::undeclared_access:
    return "undeclared_access";
  case errc::access_conflict:
    return "access_conflict";
  case errc::system_unknown:
    return "system_unknown";
  case errc::type_unknown:
    return "type_unknown";
  case errc::field_unknown:
    return "field_unknown";
  case errc::reflect_mismatch:
    return "reflect_mismatch";
  case errc::type_registry_frozen:
    return "type_registry_frozen";
  case errc::component_schema_mismatch:
    return "component_schema_mismatch";
  case errc::event_overflow:
    return "event_overflow";
  case errc::event_type_unknown:
    return "event_type_unknown";
  case errc::event_payload_invalid:
    return "event_payload_invalid";
  case errc::snapshot_corrupt:
    return "snapshot_corrupt";
  case errc::snapshot_version:
    return "snapshot_version";
  case errc::snapshot_schema_mismatch:
    return "snapshot_schema_mismatch";
  case errc::snapshot_mid_tick:
    return "snapshot_mid_tick";
  case errc::snapshot_abi_mismatch:
    return "snapshot_abi_mismatch";
  case errc::snapshot_contributor_unknown:
    return "snapshot_contributor_unknown";
  case errc::link_observation_rejected:
    return "link_observation_rejected";
  case errc::link_transient_component:
    return "link_transient_component";
  case errc::projection_stale:
    return "projection_stale";
  }
  return "unknown";
}

/// A failure: its code, a human message and where it was made.
struct error {
  errc code = errc::internal;
  std::string message;
  std::source_location where;
};

/// The engine's result type: a value or an sl::error (ADR-0009).
template <class T> using result = std::expected<T, error>;

/// `return sl::make_error(sl::errc::not_found, "no such mesh");` from a function returning
/// result<T>.
[[nodiscard]] inline std::unexpected<error>
make_error(errc code, std::string message,
           std::source_location where = std::source_location::current()) {
  return std::unexpected<error>(error{.code = code, .message = std::move(message), .where = where});
}

/// "core.not_found (1002): no such mesh [file:line]".
[[nodiscard]] std::string describe(const error &e);

} // namespace sl
