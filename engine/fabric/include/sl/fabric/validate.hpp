#pragma once

// Validation, lock, observer and merge-driver interfaces (WO-043, SL-ARCH-002 ARCH2-R05,
// ARCH2-R46, ARCH2-R47; ADR-0017, ADR-0019).
//
// The transaction manager (WO-050) runs a validator_registry over a normalised patch and the
// staged state it produced: every validator of every stage of SL-ARCH-002 `validation_stages`, in
// stage order, collecting every diagnostic, sorted by (stage, record, path, rule, message). Locks
// are consulted through a lock_authority (null_lock_authority allows all until Volume 18).
// Commit observers attach summaries to the receipt through a receipt_builder without the manager
// knowing them. Merge drivers are keyed by type id; the default is the field-level three-way
// driver.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

enum class severity : std::uint8_t { info = 1, warning = 2, error = 3 };

[[nodiscard]] std::string_view to_string(severity s) noexcept;

/// The seven stages of SL-ARCH-002 `validation_stages`, in the order they run.
enum class validator_stage : std::uint8_t {
  structural = 1, // paths, op legality over absent, live and tombstoned targets
  schema = 2,     // types, ranges, presence, finite floats, provenance completeness
  authority = 3,  // domain, innermost-domain grants, actor kind (WO-048)
  references = 4, // strong and owned refs live and type-compatible, delete blocking, cascades
  invariants = 5, // id class, owned forest, tombstones, derivation, domain invariants
  policy = 6,     // approval hooks (Volume 20)
  locks = 7,      // lock_authority (Volume 18)
};

inline constexpr std::size_t validator_stage_count = 7;

[[nodiscard]] std::string_view to_string(validator_stage s) noexcept;

/// One finding. `stage` is filled by the registry from the validator that reported it.
struct diagnostic {
  std::string rule;
  fabric::severity severity = fabric::severity::error;
  record_id record;
  fabric::path path;
  std::string message;
  validator_stage stage = validator_stage::structural;

  friend bool operator==(const diagnostic &, const diagnostic &) noexcept = default;
};

/// The ARCH2-R05 order: stage, record id, path, rule, then message.
[[nodiscard]] bool diagnostic_less(const diagnostic &a, const diagnostic &b) noexcept;

/// True when any diagnostic has severity error.
[[nodiscard]] bool has_error(std::span<const diagnostic> diagnostics) noexcept;

/// Who is editing (ADR-0017): the actor and the subsystem it acts through. The actor's kind and
/// grants are resolved by the authority validator (WO-048).
struct edit_origin {
  actor_id actor;
  std::string subsystem;

  friend bool operator==(const edit_origin &, const edit_origin &) noexcept = default;
};

/// What a validator sees: the normalised patch, its footprint, the staged state after it (whose
/// base is the state before it), the frozen schema registry and the origin. Non-owning: every
/// referenced object outlives the context.
class validation_context {
public:
  validation_context(const fabric::patch &normalized, const staged_state &after,
                     const schema_registry &schemas, edit_origin origin);

  [[nodiscard]] const fabric::patch &patch() const noexcept { return *patch_; }
  [[nodiscard]] const fabric::footprint &footprint() const noexcept { return footprint_; }
  [[nodiscard]] const staged_state &after() const noexcept { return *after_; }
  [[nodiscard]] const state_reader &before() const noexcept { return after_->base(); }
  [[nodiscard]] const schema_registry &schemas() const noexcept { return *schemas_; }
  [[nodiscard]] const edit_origin &origin() const noexcept { return origin_; }

private:
  const fabric::patch *patch_;
  const staged_state *after_;
  const schema_registry *schemas_;
  edit_origin origin_;
  fabric::footprint footprint_;
};

/// A validator of one stage. run() appends its findings to `out`; it fails only when it could
/// not judge (a read error), never to report a finding.
class validator {
public:
  virtual ~validator() = default;
  /// A unique name ("fabric.schema"); the registry refuses a repeat.
  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
  [[nodiscard]] virtual validator_stage stage() const noexcept = 0;
  [[nodiscard]] virtual result<void> run(const validation_context &ctx,
                                         std::vector<diagnostic> &out) const = 0;

protected:
  validator() = default;
  validator(const validator &) = default;
  validator &operator=(const validator &) = default;
  validator(validator &&) = default;
  validator &operator=(validator &&) = default;
};

/// Validators by stage, each stage in registration order. Freezes with the workspace
/// (ARCH2-R18): add() is then `registry_frozen`.
class validator_registry {
public:
  /// `invalid_argument` for a null validator, `already_exists` for a repeated name,
  /// `registry_frozen` after freeze().
  [[nodiscard]] result<void> add(std::shared_ptr<const validator> v);
  void freeze() noexcept { frozen_ = true; }
  [[nodiscard]] bool frozen() const noexcept { return frozen_; }
  /// Every validator in run order.
  [[nodiscard]] std::vector<const validator *> in_order() const;
  [[nodiscard]] std::size_t size() const noexcept { return validators_.size(); }

  /// Runs every validator in stage order and returns all diagnostics sorted (ARCH2-R05), each
  /// stamped with its validator's stage; the first validator error otherwise.
  [[nodiscard]] result<std::vector<diagnostic>> run(const validation_context &ctx) const;

private:
  std::vector<std::shared_ptr<const validator>> validators_; // registration order
  bool frozen_ = false;
};

// ---- Locks (ARCH2-R46) --------------------------------------------------------------------

/// Consulted at validation and again under the commit mutex (step C2). check() returns
/// `locked` naming the held lock to deny; any other error is a failure to decide.
class lock_authority {
public:
  virtual ~lock_authority() = default;
  [[nodiscard]] virtual result<void> check(const edit_origin &origin,
                                           const footprint &fp) const = 0;

protected:
  lock_authority() = default;
  lock_authority(const lock_authority &) = default;
  lock_authority &operator=(const lock_authority &) = default;
  lock_authority(lock_authority &&) = default;
  lock_authority &operator=(lock_authority &&) = default;
};

/// Allows everything (the default until Volume 18).
class null_lock_authority final : public lock_authority {
public:
  [[nodiscard]] result<void> check(const edit_origin &origin, const footprint &fp) const override;
};

// ---- Commit observers (SL-ARCH-002 commit step C6) ----------------------------------------

/// What observers learn about one commit.
struct commit_event {
  tx_id tx;
  std::uint64_t seq = 0;
  digest revision;
  digest root;
  edit_origin origin;
  fabric::patch patch;            // normalised
  std::vector<record_id> changed; // ascending
};

/// Collects what observers attach to the receipt, keyed by observer-chosen names and kept in
/// name order. The manager copies the attachments into the receipt (WO-050).
class receipt_builder {
public:
  /// `already_exists` when `name` is attached already.
  [[nodiscard]] result<void> attach(std::string name, value summary);
  [[nodiscard]] const value *find(std::string_view name) const noexcept;
  [[nodiscard]] const std::map<std::string, value, std::less<>> &attachments() const noexcept {
    return attachments_;
  }

private:
  std::map<std::string, value, std::less<>> attachments_;
};

/// Notified after the commit point in registration order; a failure is logged by the manager
/// and never undoes the commit.
class commit_observer {
public:
  virtual ~commit_observer() = default;
  [[nodiscard]] virtual result<void> on_commit(const commit_event &event,
                                               receipt_builder &receipt) = 0;

protected:
  commit_observer() = default;
  commit_observer(const commit_observer &) = default;
  commit_observer &operator=(const commit_observer &) = default;
  commit_observer(commit_observer &&) = default;
  commit_observer &operator=(commit_observer &&) = default;
};

// ---- Merge drivers (ARCH2-R47) ------------------------------------------------------------

/// One record in three versions; nullopt is an absent (or deleted) record body.
struct merge_input {
  record_id record;
  type_id type;
  std::optional<value> base;
  std::optional<value> ours;
  std::optional<value> theirs;
};

/// A path both sides changed differently.
struct merge_conflict {
  fabric::path path;
  std::optional<value> base;
  std::optional<value> ours;
  std::optional<value> theirs;

  friend bool operator==(const merge_conflict &, const merge_conflict &) noexcept = default;
};

/// The merged body (nullopt: the record is absent) and the conflicts, sorted by path. Where a
/// conflict sits, the merged body keeps ours.
struct merge_result {
  std::optional<value> merged;
  std::vector<merge_conflict> conflicts;

  [[nodiscard]] bool clean() const noexcept { return conflicts.empty(); }
};

class merge_driver {
public:
  virtual ~merge_driver() = default;
  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
  [[nodiscard]] virtual result<merge_result> merge(const merge_input &in) const = 0;

protected:
  merge_driver() = default;
  merge_driver(const merge_driver &) = default;
  merge_driver &operator=(const merge_driver &) = default;
  merge_driver(merge_driver &&) = default;
  merge_driver &operator=(merge_driver &&) = default;
};

/// The default driver (ADR-0019): three-way by field. A side equal to base yields the other
/// side; equal sides agree; otherwise structs merge by field number, maps by key, osets by
/// element id (values recursively, order keys three-way), variants of one case by payload, and
/// anything else (scalars, lists, a kind or case change, delete against edit) is a conflict at
/// its path.
class field_merge_driver final : public merge_driver {
public:
  [[nodiscard]] std::string_view name() const noexcept override { return "fabric.field"; }
  [[nodiscard]] result<merge_result> merge(const merge_input &in) const override;
};

/// Drivers keyed by type id; types without one use the default field driver.
class merge_driver_registry {
public:
  merge_driver_registry();

  /// `invalid_argument` for a null driver, `already_exists` for a type with a driver,
  /// `registry_frozen` after freeze().
  [[nodiscard]] result<void> add(const type_id &type, std::shared_ptr<const merge_driver> d);
  void freeze() noexcept { frozen_ = true; }
  [[nodiscard]] bool frozen() const noexcept { return frozen_; }
  [[nodiscard]] const merge_driver &driver_for(const type_id &type) const noexcept;
  [[nodiscard]] const merge_driver &default_driver() const noexcept { return *default_; }

private:
  std::shared_ptr<const merge_driver> default_;
  std::map<type_id, std::shared_ptr<const merge_driver>> drivers_;
  bool frozen_ = false;
};

} // namespace sl::fabric
