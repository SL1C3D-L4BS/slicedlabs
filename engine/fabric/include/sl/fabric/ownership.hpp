#pragma once

// The domain registry: the F2 enforcement point (WO-048, SL-ARCH-001 ARCH1-R36 to ARCH1-R38,
// ADR-0017; master plan section 3, F2: "identity registry rejects a second ownership claim").
//
// A data domain is a dotted name ("content.scene") under one of the twelve reserved prefixes of
// SL-ARCH-001 `domain_registry`, one per subsystem of master plan section 2.2. Every domain has
// exactly one owner subsystem; claim() refuses, records and counts every claim that would give a
// domain a second owner:
//   ownership_conflict  the domain is claimed already by another owner, or the claimant is not the
//                       subsystem its prefix is reserved to (the message names both), or a type
//                       of the spec is bound to another domain already
//   ownership_orphan    the name is not a dotted domain under a reserved prefix, or a `fabric.*`
//                       domain outside SL-ARCH-001 `fabric_domains`
//   schema_invalid      R-OWN-STATE: no state class, or one the prefix does not admit
//   already_exists      the same owner claims the same domain twice
//   registry_frozen     a claim after freeze() (ARCH1-R38: the registry freezes when the first
//                       workspace opens)
// bind_types() gives every non-built-in type of a schema registry its domain: the claimed domain
// equal to the type's domain, or the innermost claimed ancestor of it; a type with no domain or no
// claimed one is `ownership_orphan` (ARCH1-R37). Built-in types belong to subsystem 02.
//
// f2() is the F2 measure: numerator = claimed domains with more than one owner (the claimant and
// the subsystem the prefix is reserved to are both counted), denominator = claimed domains,
// threshold 0. By construction claim() keeps the numerator at 0; the rejected claims are the
// evidence that the enforcement point fired.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/type.hpp"

namespace sl::fabric {

/// The twelve subsystems of master plan section 2.2; the number is the owner code ("01".."12").
enum class subsystem : std::uint8_t {
  shell = 1,
  fabric = 2,
  runtime = 3,
  gfx = 4,
  content = 5,
  sim = 6,
  code = 7,
  asset = 8,
  obs = 9,
  collab = 10,
  verify = 11,
  gov = 12,
};

inline constexpr std::size_t subsystem_count = 12;

/// The reserved prefix of the subsystem ("shell", ..., "gov"); "unknown" otherwise.
[[nodiscard]] std::string_view to_string(subsystem s) noexcept;
/// The two-digit owner code ("01", ..., "12"); "00" otherwise.
[[nodiscard]] std::string_view owner_code(subsystem s) noexcept;
/// Parses an owner code ("05") or a prefix name ("content").
[[nodiscard]] std::optional<subsystem> subsystem_from_string(std::string_view text) noexcept;

/// One row of SL-ARCH-001 `domain_registry`.
struct reserved_prefix {
  std::string_view prefix;
  subsystem owner = subsystem::fabric;
  std::string_view owns;
  std::span<const state_class> state_classes;
};

/// The twelve rows, in owner order (FAB-INV-OWN-03).
[[nodiscard]] std::span<const reserved_prefix> reserved_prefixes() noexcept;
/// The row whose prefix is the first segment of `domain`; null when there is none.
[[nodiscard]] const reserved_prefix *find_reserved_prefix(std::string_view domain) noexcept;
/// SL-ARCH-001 `fabric_domains`: the only domains under the `fabric` prefix.
[[nodiscard]] std::span<const std::string_view> fabric_domains() noexcept;
/// SL-ARCH-001 `actor_kinds` and `default_deny`, as names (authority.hpp holds the enum).
[[nodiscard]] std::span<const std::string_view> actor_kind_names() noexcept;
[[nodiscard]] std::span<const std::string_view> default_deny_names() noexcept;

/// Two or more dot-separated identifiers of [a-z][a-z0-9_]* (the sl-type-def/1 domain pattern).
[[nodiscard]] bool is_domain_name(std::string_view name) noexcept;
/// True when `domain` equals `ancestor` or is a dotted descendant of it.
[[nodiscard]] bool domain_within(std::string_view domain, std::string_view ancestor) noexcept;

/// One claim: a domain, its owner, the state classes its records may have and the types it
/// declares up front (bind_types() adds the rest).
struct domain_spec {
  std::string name;
  subsystem owner = subsystem::fabric;
  std::vector<state_class> state_classes;
  std::vector<type_id> types;

  friend bool operator==(const domain_spec &, const domain_spec &) = default;
};

/// A refused claim: what was claimed, by whom, who holds it (when another owner does) and why.
struct rejected_claim {
  std::string domain;
  subsystem claimant = subsystem::fabric;
  std::optional<subsystem> holder;
  errc code = errc::ownership_conflict;
  std::string message;

  friend bool operator==(const rejected_claim &, const rejected_claim &) = default;
};

/// The F2 measure (SL-GOV-006 row F2).
struct f2_report {
  std::size_t numerator = 0;   // claimed domains with more than one owner
  std::size_t denominator = 0; // claimed domains
  std::size_t conflicts = 0;   // claims refused with ownership_conflict

  friend bool operator==(const f2_report &, const f2_report &) noexcept = default;
};

class domain_registry {
public:
  /// Claims a domain (rules in the file comment); a refused claim is recorded in rejected().
  [[nodiscard]] result<void> claim(domain_spec spec);
  /// Binds every non-built-in type of `schemas` to its domain, all or nothing:
  /// `ownership_orphan` listing every type with no domain or no claimed one (sorted by name),
  /// `ownership_conflict` for a type a spec bound to another domain, `registry_frozen` after
  /// freeze().
  [[nodiscard]] result<void> bind_types(const schema_registry &schemas);
  /// Freezes the registry (ARCH1-R38); later claims and binds are `registry_frozen`. Freezing a
  /// frozen registry succeeds. `invariant_violated` (never expected) when the F2 numerator is not
  /// 0.
  [[nodiscard]] result<void> freeze();
  [[nodiscard]] bool frozen() const noexcept { return frozen_; }

  /// The claim of exactly `domain`; null when unclaimed.
  [[nodiscard]] const domain_spec *find(std::string_view domain) const noexcept;
  /// The claim of `domain` or of its innermost claimed ancestor; null when none.
  [[nodiscard]] const domain_spec *owner_of(std::string_view domain) const noexcept;
  /// The domain a type is bound to; null for an unbound (or built-in) type.
  [[nodiscard]] const domain_spec *domain_of(const type_id &type) const noexcept;

  [[nodiscard]] f2_report f2() const;
  [[nodiscard]] std::span<const rejected_claim> rejected() const noexcept { return rejected_; }
  /// Every claim, in domain name order.
  [[nodiscard]] const std::map<std::string, domain_spec, std::less<>> &domains() const noexcept {
    return domains_;
  }

private:
  result<void> check_claim(const domain_spec &spec) const;
  result<void> reject(const domain_spec &spec, std::optional<subsystem> holder, const error &why);

  std::map<std::string, domain_spec, std::less<>> domains_;
  std::map<type_id, std::string> type_domain_;
  std::vector<rejected_claim> rejected_;
  bool frozen_ = false;
};

/// The domains Fabric itself owns: each of `fabric_domains` claimed by subsystem 02 with the
/// state classes of the `fabric` prefix row.
[[nodiscard]] result<void> claim_fabric_domains(domain_registry &registry);

} // namespace sl::fabric
