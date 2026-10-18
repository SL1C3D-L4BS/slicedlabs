#pragma once

// Actors, grants and the authority validator (WO-048, SL-ARCH-001 ARCH1-R39, ARCH1-R40,
// ADR-0017; SL-ARCH-002 validation stage `authority`).
//
// An actor is human, tool, plugin, agent or service (SL-ARCH-001 `actor_kinds`) and holds grants
// "write:<domain>" and "accept:<domain>"; a grant covers the named domain and every dotted
// descendant ("write:content" covers "content.scene"), and its target is a reserved prefix or a
// domain under one. A write to domain D is allowed when the actor holds write:D (or an ancestor),
// or when the actor kind is not denied by default and the edit comes through D's owner subsystem
// (edit_origin::subsystem, an owner code "05" or a prefix name "content"). Agent and service
// actors are denied by default: only an explicit grant lets them write (ARCH1-R40). Accepting
// (promotion to accepted, ARCH1-R42) always needs an explicit accept grant.
//
// The authority_validator (stage authority, name "fabric.authority") walks what every op of the
// normalised patch writes, typed by the frozen schema registry: the record type, the types along
// the op's path, and every typed value inside the written (and the replaced or removed) subtree.
// Every typed value whose type declares a domain different from its parent's is a boundary, and
// each boundary needs that domain's authority as well as the record's (ARCH1-R39: a physics
// component inside a scene entity needs `sim` as well as `content`). A type with an empty domain
// inherits its parent's; built-in types never change the domain. One diagnostic per (record,
// domain), at the first path that needed it, with the rules:
//   R-AUTH-DENIED   the actor may not write the domain (authority_denied)
//   R-AUTH-ORPHAN   the domain has no claimed owner (ownership_orphan)
//   R-AUTH-ACTOR    the origin's actor is not in the actor directory (denied)
// A move_elem writes only the oset's order key, so it needs the domains along the path to the
// element, not those inside it. Unknown types and paths that do not fit are left to the
// structural and schema stages.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/validate.hpp"

namespace sl::fabric {

/// SL-ARCH-001 `actor_kinds`; numbers are stable.
enum class actor_kind : std::uint8_t { human = 1, tool = 2, plugin = 3, agent = 4, service = 5 };

/// "human", ..., "service"; "unknown" otherwise.
[[nodiscard]] std::string_view to_string(actor_kind k) noexcept;
[[nodiscard]] std::optional<actor_kind> actor_kind_from_string(std::string_view name) noexcept;
/// True for the kinds of SL-ARCH-001 `default_deny` (agent, service).
[[nodiscard]] bool denied_by_default(actor_kind k) noexcept;

struct actor {
  actor_id id;
  actor_kind kind = actor_kind::human;
  std::vector<std::string> grants; // "write:<domain>", "accept:<domain>"

  friend bool operator==(const actor &, const actor &) = default;
};

/// "write:" or "accept:" followed by a reserved prefix or a domain under one.
[[nodiscard]] bool is_valid_grant(std::string_view grant) noexcept;
/// True when `a` holds "<verb>:<g>" with `domain` within g; verb is "write" or "accept".
[[nodiscard]] bool holds_grant(const actor &a, std::string_view verb,
                               std::string_view domain) noexcept;

/// May `a`, editing through `subsystem`, write records of `domain`? `ownership_orphan` when no
/// subsystem owns the domain, `authority_denied` naming the actor, its kind, the subsystem and
/// the owner otherwise (rules in the file comment).
[[nodiscard]] result<void> check_write(const domain_registry &domains, const actor &a,
                                       std::string_view subsystem, std::string_view domain);
/// May `a` promote records of `domain` to accepted? Needs an explicit accept grant;
/// `ownership_orphan` or `authority_denied` otherwise.
[[nodiscard]] result<void> check_accept(const domain_registry &domains, const actor &a,
                                        std::string_view domain);

/// The actors the authority validator knows, by id.
class actor_directory {
public:
  /// `invalid_argument` for a null id, an unknown kind or a malformed grant; `already_exists` for
  /// a repeated id.
  [[nodiscard]] result<void> add(actor a);
  [[nodiscard]] const actor *find(const actor_id &id) const noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return actors_.size(); }

private:
  std::map<actor_id, actor> actors_;
};

/// The validator of stage authority. Non-owning: the domain registry and the actor directory
/// outlive it.
class authority_validator final : public validator {
public:
  authority_validator(const domain_registry &domains, const actor_directory &actors) noexcept
      : domains_(&domains), actors_(&actors) {}

  [[nodiscard]] std::string_view name() const noexcept override { return "fabric.authority"; }
  [[nodiscard]] validator_stage stage() const noexcept override {
    return validator_stage::authority;
  }
  /// Appends one diagnostic per denied (record, domain); fails only when a record cannot be
  /// read.
  [[nodiscard]] result<void> run(const validation_context &ctx,
                                 std::vector<diagnostic> &out) const override;

private:
  const domain_registry *domains_;
  const actor_directory *actors_;
};

} // namespace sl::fabric
