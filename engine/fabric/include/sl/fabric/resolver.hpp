#pragma once

// The reference resolver (WO-060, SL-ARCH-001 ARCH1-R63 to ARCH1-R66; ADR-0009, ADR-0011).
//
// A reference names a record by id, never by name (names never resolve, ARCH1-R63), optionally
// inside one package, with the record types it accepts. resolve() is a pure, total function of
// the project snapshot, the manifest's package pins and the reference: it reads, never writes,
// keeps no state between calls and gives the same answer for the same inputs in any call order.
//
// Order (ARCH1-R63), for a reference with no package:
//   1. the same document: the target is a member of `from_document` in the membership map
//   2. the project: the target's records-trie leaf in the project snapshot
//   3. the pinned packages, in pin order (the manifest's package_pins oset, ascending by order
//      key then element id): the target's leaf in the snapshot of the pinned revision
//   4. otherwise missing
// The first scope that holds a leaf for the target decides: a record leaf whose type is accepted
// is resolved, one whose type is not is wrong_type (the record is returned so the caller sees its
// type), a tombstone leaf is retired. A pinned package that is unavailable (the provider answers
// not_found or object_missing) when the search reaches it is package_missing naming that package:
// a later pin is not consulted, so the answer never depends on which packages happen to be
// mounted. Two pins of one package (refused by validate_manifest) are consulted once, at the
// first.
//
// A reference with a package (ARCH1-R66) resolves only in the revision that package is pinned to:
// no pin for the package, or an unavailable one, is package_missing; otherwise the leaf decides as
// above and no leaf is missing.
//
// An external reference (ARCH1-R65) becomes a record id only through an import record: the import
// record of (system, locator) is the record at import_record_id_for(system, locator) in the scope
// of the reference (the package snapshot when it names a package, else the project). No leaf
// there is missing and a tombstone is retired; a live record that is not a well-formed import
// record of that system and locator, or whose record is not an imported-scheme id, is
// object_corrupt. The import record's record then resolves through the order above, and the
// resolution carries the import record. The reference's content digest is not compared
// (staleness is the impact engine's, WO-056).
//
// Errors are never outcomes (ADR-0009, ARCH1-R64): a failed snapshot read (the store's errors,
// object_corrupt), a provider error other than not_found or object_missing, a provider snapshot
// that is not at the pinned revision (invalid_argument) and an ill-formed reference (a null
// target, both a target and an external reference, an empty system or locator: invalid_argument)
// are returned as errors.
//
// The resolver keeps a copy of the project snapshot and of the pins, and a pointer to the
// provider, which must outlive it. resolve() is const and thread-safe when the store and the
// provider are.

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/gen/support.gen.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/identity.hpp"
#include "sl/fabric/manifest.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/type.hpp"

namespace sl::fabric {

using external_ref = gen::external_ref;

/// A reference to resolve: a target id or an external reference (exactly one), an optional
/// package that scopes it, and the accepted record types (empty: any type).
struct reference {
  std::optional<record_id> package;
  record_id target;
  std::vector<type_id> expected;
  std::optional<external_ref> external;

  friend bool operator==(const reference &, const reference &) = default;
};

/// What a resolution found.
enum class resolution_outcome : std::uint8_t {
  resolved = 0,        // a live record of an accepted type
  retired = 1,         // the first leaf found is a tombstone
  missing = 2,         // no scope holds the target (or no import record for an external ref)
  wrong_type = 3,      // a live record whose type is not accepted
  package_missing = 4, // the package is not pinned, or its pinned revision is unavailable
};

/// "resolved", "retired", "missing", "wrong_type", "package_missing"; "unknown" otherwise.
[[nodiscard]] std::string_view to_string(resolution_outcome o) noexcept;

/// The scope that decided a resolution.
enum class resolution_scope : std::uint8_t {
  none = 0,     // missing, or package_missing
  document = 1, // a member of the referring document
  project = 2,  // the project snapshot
  package = 3,  // a pinned package snapshot
};

/// "none", "document", "project", "package"; "unknown" otherwise.
[[nodiscard]] std::string_view to_string(resolution_scope s) noexcept;

struct resolution {
  resolution_outcome outcome = resolution_outcome::missing;
  resolution_scope scope = resolution_scope::none;
  /// The record found: set for resolved and wrong_type.
  std::optional<snapshot_entry> record;
  /// The package that decided: set for a package scope and for package_missing.
  std::optional<record_id> package;
  /// The id looked up: the reference's target, or the import record's record for an external
  /// reference (null when no import record was found).
  record_id target;
  /// The import record an external reference went through.
  std::optional<import_record> import;

  friend bool operator==(const resolution &, const resolution &) = default;
};

/// Opens the snapshot of a package at a pinned revision. `not_found` or `object_missing` mean the
/// package is unavailable (package_missing); any other error is a read failure.
class package_provider {
public:
  package_provider() = default;
  package_provider(const package_provider &) = delete;
  package_provider(package_provider &&) = delete;
  package_provider &operator=(const package_provider &) = delete;
  package_provider &operator=(package_provider &&) = delete;
  virtual ~package_provider() = default;

  /// The snapshot of revision object `revision` of package `package`.
  [[nodiscard]] virtual result<snapshot> open(const record_id &package, const digest &revision) = 0;
};

/// imported(import, system, locator): the id of the import record of an external object.
[[nodiscard]] record_id import_record_id_for(std::string_view system,
                                             std::string_view locator) noexcept;

class resolver {
public:
  resolver(snapshot project, const project_manifest &manifest, package_provider &packages);

  /// The resolution of `ref` made from document `from_document` (null: from no document).
  [[nodiscard]] result<resolution> resolve(const reference &ref,
                                           const record_id &from_document) const;

  /// The pins in resolution order, one per package.
  [[nodiscard]] const std::vector<package_pin> &pins() const noexcept { return pins_; }

private:
  [[nodiscard]] result<resolution> resolve_id(const std::optional<record_id> &package,
                                              const record_id &target,
                                              const std::vector<type_id> &expected,
                                              const record_id &from_document) const;
  [[nodiscard]] result<resolution> resolve_external(const reference &ref, const external_ref &ext,
                                                    const record_id &from_document) const;
  /// The snapshot of `pin`, or nullopt when the provider says it is unavailable.
  [[nodiscard]] result<std::optional<snapshot>> open_pin(const package_pin &pin) const;
  /// The pin of `package`, or nullptr.
  [[nodiscard]] const package_pin *pin_of(const record_id &package) const noexcept;

  snapshot project_;
  std::vector<package_pin> pins_;
  package_provider *packages_;
};

} // namespace sl::fabric
