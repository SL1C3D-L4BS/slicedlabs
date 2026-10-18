#pragma once

// Schema versioning and migration (WO-057, SL-ARCH-001 ARCH1-R21, ARCH1-R23 to ARCH1-R25,
// ADR-0012, ADR-0013, ADR-0018).
//
// classify() is the C++ twin of tools/fabric/schema_compat.py: two versions of one type compare
// fields, enumerators and cases by number, and the result is the worst class of every difference
// (identical < additive < backward_only < breaking < forbidden), exactly as the Python classifier
// decides the rows of tests/fixtures/fabric/schema/compat_table.json (FAB-INV-SCH-04). Names,
// docs, since_minor and deprecated_in are labels (identical). The codes beyond the SL-ARCH-001
// table follow the Python: a removed field number left unreserved is forbidden, a document_root
// change is breaking. Ref specs are compared only on fields of the built-in type `ref`, as the
// Python does. Two types with different names are forbidden.
//
// Migration (ARCH1-R25). A migration_step is a pure, deterministic function over one record body
// of one type, from one major version to a later one:
//   add_field     sets field `field` to `fill` when the body lacks it (no fill: nothing to do, the
//                 field is optional)
//   remove_field  drops field `field`; it loses data, so it must carry a loss note
//   retype        replaces the value of field `field`, when present, with converter(value)
//   identity      leaves the body unchanged (the backward_only bump)
//   native        replaces the whole body with converter(body)
// The steps of one (type, from major) form one hop; they share `from` and `to` and apply in
// registration order. A hop takes a record of version from.major.m with m <= from.minor (an older
// minor of the same major is readable as `from`: additive changes need no step). plan_migration()
// composes hops into a chain for every live record whose major is older than its type's major in
// the target registry; records at the target major are left alone, records written by a newer
// minor of their type are listed as preserved (ARCH1-R23) and never written, a newer major is
// `format_unsupported`. A missing hop is `migration_missing_step` naming the type and version.
//
// run_migration() applies a plan as ONE transaction over the planned revision (reject_stale), by
// the system actor migration_system_actor() with epistemic class calculated: one retype_record per
// planned record (same type, converted body; the manager stamps the target version) and one
// annotate op recording the migration. A converter error, or a converted body the validators
// refuse, aborts the transaction, leaves the head unchanged and fails with `migration_failed`
// naming the record and the path (FAB-INV-SCH-06). The caller registers the system actor in the
// actor directory (a tool with the grants its domains need).
//
// Newer minors and deprecation (ARCH1-R23, ARCH1-R24) are validators the workspace registers
// (workspace_options::validators): newer_minor_validator refuses every write (set, retype or
// delete) to a record written by a newer minor (or major) of its type with a `read_only_newer`
// diagnostic, so such a record stays byte-identical, unknown fields included; check_writable()
// gives the same verdict as an error. deprecation_validator adds a warning diagnostic for every
// write of a field its type declares deprecated (deprecated_in <= the type's minor). A deprecated
// field still decodes and conforms.
//
// Module loading. types_from_module() reads an sl-type-def/1 module (schemas/fabric/README.md):
// versions "major.minor", classes, kinds and presence by name, type references by qualified name
// (type_id_of), defaults by field type. As in the generator (WO-045), a type with no record class
// and no document_root keeps no state class.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

// ---- Compatibility (ARCH1-R21) ------------------------------------------------------------

/// SL-ARCH-001 `compatibility` classes, worst last.
enum class compat : std::uint8_t {
  identical = 0,
  additive = 1,
  backward_only = 2,
  breaking = 3,
  forbidden = 4,
};

/// "identical", "additive", "backward_only", "breaking", "forbidden"; "unknown" otherwise.
[[nodiscard]] std::string_view to_string(compat c) noexcept;

/// The worst class over every difference from `from` to `to` (file comment).
[[nodiscard]] compat classify(const type_desc &from, const type_desc &to) noexcept;

// ---- Steps and the registry ---------------------------------------------------------------

enum class step_kind : std::uint8_t {
  add_field = 1,
  remove_field = 2,
  retype = 3,
  identity = 4,
  native = 5,
};

/// "add_field", ..., "native"; "unknown" otherwise.
[[nodiscard]] std::string_view to_string(step_kind k) noexcept;

/// A pure, deterministic conversion of one value (a field value for retype, the body for native).
/// An error fails the migration.
using migration_converter = std::function<result<value>(const value &)>;

struct migration_step {
  type_id type;
  type_version from;
  type_version to;
  step_kind kind = step_kind::identity;
  /// The field of add_field, remove_field and retype (>= 1); 0 otherwise.
  std::uint32_t field = 0;
  /// add_field: the value written when the body lacks the field.
  std::optional<value> fill;
  /// retype and native: the registered converter's name.
  std::string converter;
  /// What the step loses; required for remove_field, empty for a lossless step.
  std::string loss;

  friend bool operator==(const migration_step &, const migration_step &) = default;
};

class migration_registry {
public:
  /// `invalid_argument` for an empty name or an empty function; `already_exists` for a repeat.
  [[nodiscard]] result<void> add_converter(std::string name, migration_converter fn);
  /// Adds a step to the hop (type, from.major). `invalid_argument` for a null type, to.major <=
  /// from.major, a field number on the wrong kind (or none where one is needed), a fill outside
  /// add_field, or a converter name on a kind that takes none (or none on retype and native);
  /// `not_found` for an unregistered converter; `migration_lossy` for remove_field without a
  /// loss note; `already_exists` when the hop holds steps with another from or to.
  [[nodiscard]] result<void> add(migration_step step);

  /// The steps of hop (type, major) in registration order; empty when there is none.
  [[nodiscard]] std::span<const migration_step> steps_from(const type_id &type,
                                                           std::uint32_t major) const noexcept;
  /// The converter named `name`, or null.
  [[nodiscard]] std::shared_ptr<const migration_converter>
  converter(std::string_view name) const noexcept;

  /// The class of the change `from` -> `to` when the registry holds what it demands (ARCH1-R21):
  /// `schema_invalid` for a forbidden change; `migration_missing_step` for a backward_only or
  /// breaking change with no hop from from.major ending at to.major.
  [[nodiscard]] result<compat> check_change(const type_desc &from, const type_desc &to) const;

private:
  std::map<std::string, std::shared_ptr<const migration_converter>, std::less<>> converters_;
  std::map<std::pair<type_id, std::uint32_t>, std::vector<migration_step>> hops_;
};

// ---- Plans --------------------------------------------------------------------------------

/// One record the plan migrates: its chain of steps, hop after hop.
struct planned_record {
  record_id id;
  type_id type;
  type_version from;
  type_version to;
  std::vector<migration_step> steps;

  friend bool operator==(const planned_record &, const planned_record &) = default;
};

struct migration_plan {
  /// The ref run_migration commits to.
  std::string ref = "main";
  /// The revision planned over (nullopt for an empty snapshot).
  std::optional<digest> base;
  /// The schema_set digest of the target registry.
  digest target_schema_set;
  /// Ascending by id.
  std::vector<planned_record> records;
  /// Records written by a newer minor of their type: preserved, never written. Ascending.
  std::vector<record_id> preserved;
  /// The converters the steps name.
  std::map<std::string, std::shared_ptr<const migration_converter>, std::less<>> converters;

  /// The loss notes of every planned step, sorted and without repeats.
  [[nodiscard]] std::vector<std::string> losses() const;
};

/// The plan that brings every live record of `s` to its type's version in `target` (file
/// comment). `schema_unknown` for a record whose type `target` does not register;
/// `format_unsupported` for a record of a newer major; `migration_missing_step` for a record with
/// no chain; the snapshot's read errors.
[[nodiscard]] result<migration_plan>
plan_migration(const snapshot &s, const schema_registry &target, const migration_registry &steps);

/// The system actor migrations run as: derive(actor, well_known(namespace, 1),
/// "fabric.migration").
[[nodiscard]] actor_id migration_system_actor() noexcept;

/// Runs `plan` as one transaction on `m` (file comment). The origin gives the subsystem, the
/// correlation and the F3 provenance; its actor and epistemic class are replaced by the system
/// actor and calculated. `invalid_argument` when the manager's schema_set is not the plan's
/// target; `migration_failed` naming record and path for a converter error or a refused body;
/// begin and commit errors otherwise (`stale_base` when the ref moved since the plan).
[[nodiscard]] result<commit_receipt>
run_migration(transaction_manager &m, const migration_plan &plan, const tx_origin &origin);

// ---- Newer minors and deprecation (ARCH1-R23, ARCH1-R24) ----------------------------------

/// `read_only_newer` when `e` was written by a newer minor of its type than `schemas` registers,
/// `format_unsupported` for a newer major, `schema_unknown` for an unregistered type.
[[nodiscard]] result<void> check_writable(const snapshot_entry &e, const schema_registry &schemas);

/// Stage schema, name "fabric.schema.newer": rule R-SCH-NEWER (error, message starting
/// "read_only_newer: " or "format_unsupported: ") for every written or deleted record whose
/// stored version is newer than its registered type. Judges only when the state before is a
/// snapshot (as under the transaction manager).
class newer_minor_validator final : public validator {
public:
  [[nodiscard]] std::string_view name() const noexcept override { return "fabric.schema.newer"; }
  [[nodiscard]] validator_stage stage() const noexcept override { return validator_stage::schema; }
  [[nodiscard]] result<void> run(const validation_context &ctx,
                                 std::vector<diagnostic> &out) const override;
};

/// Stage schema, name "fabric.schema.deprecated": rule R-SCH-DEPRECATED (warning, message starting
/// "deprecated: ") at the path of every deprecated field a write adds or changes, in the record's
/// struct and in structs nested through struct-typed fields.
class deprecation_validator final : public validator {
public:
  [[nodiscard]] std::string_view name() const noexcept override {
    return "fabric.schema.deprecated";
  }
  [[nodiscard]] validator_stage stage() const noexcept override { return validator_stage::schema; }
  [[nodiscard]] result<void> run(const validation_context &ctx,
                                 std::vector<diagnostic> &out) const override;
};

// ---- Modules ------------------------------------------------------------------------------

/// One type in module form (file comment); `default_domain` applies when it names none.
/// `invalid_argument` naming the member for a malformed definition.
[[nodiscard]] result<type_desc> type_from_json(const json_value &t,
                                               std::string_view default_domain = {});
/// Every type of an sl-type-def/1 module, in document order.
[[nodiscard]] result<std::vector<type_desc>> types_from_module(const json_value &module);

} // namespace sl::fabric
