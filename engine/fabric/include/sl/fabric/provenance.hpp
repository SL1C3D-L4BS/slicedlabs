#pragma once

// Provenance and epistemic classes (WO-049, SL-ARCH-001 ARCH1-R42 to ARCH1-R45, SL-ARCH-002
// ARCH2-R14; master plan section 3, F3; ADR-0017).
//
// Epistemic classes. Every record write carries one of six classes (the generated
// fabric.provenance.epistemic_class). The transition matrix is data (legal_transitions()): the
// only legal change of class is a promotion of imported, asserted, calculated or inferred to
// accepted, and only an actor holding the grant accept:<domain> may make it. observed is
// immutable (a new observation supersedes), accepted is final (demotion is a new asserted record
// that supersedes), and a write that keeps its class is not a transition. check_transition()
// reports:
//   invariant_violated  the pair is not in the matrix (the message names the rule)
//   promotion_denied    the pair is a legal promotion but the actor lacks accept:<domain>
//
// Provenance (F3). A derived record carries a provenance record with exactly the five fields
// input_ids, input_revisions (record content digests, one per id), toolchain_id,
// parameters_digest and environment_hash; none of the build-record fields of sl-provenance
// (schema, target, artifact, built_at). The type is the generated gen::fabric_provenance struct
// whose fields are optional so that a gap is a finding, not a decode error: check_provenance()
// returns `provenance_incomplete` naming the first missing field. A field counts as missing when
// absent, when the id list is empty or holds a null id, when the digest list differs in length
// from the id list or holds the all-zero digest, when the toolchain id is empty, or when a digest
// is all zero (the "no digest" sentinel of the log backend).
//
// Derivation key (ARCH1-R44). SHA-256 over the canonical SLB/1 encoding of the struct
//   {1 inputs: list of {1 id, 2 digest} sorted by (id bytes, digest), 2 toolchain id (string),
//    3 parameters digest, 4 environment hash}
// so the key is a pure function of the five fields, independent of input order, and changes with
// every one of them. A derived resource id may be derived from it (ARCH1-R07).
//
// The provenance validator (stage invariants, name "fabric.provenance") needs three facts the
// patch itself does not carry: the provenance of the transaction (origin), a per-op override, and
// the epistemic class changes the transaction makes. They come from a provenance_source, which the
// transaction manager (WO-050) implements over its tx record; fixed_provenance_source is the
// plain implementation. Rules of the diagnostics (all severity error, message starts with the
// error code name):
//   R-PRV-INCOMPLETE  a record of a type with state class derived is written with no provenance,
//                     or provenance that check_provenance refuses (provenance_incomplete)
//   R-PRV-TRANSITION  an epistemic class change outside the matrix (invariant_violated)
//   R-PRV-PROMOTION   a legal promotion without accept:<domain> (promotion_denied)
//   R-PRV-ACTOR       the origin's actor is not in the actor directory (denied)

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/authority.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/gen/fabric.provenance.gen.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/validate.hpp"

namespace sl::fabric {

// ---- Epistemic classes --------------------------------------------------------------------

using epistemic_class = gen::fabric_provenance::epistemic_class;
using provenance = gen::fabric_provenance::provenance;

/// The six classes in number order.
inline constexpr std::array<epistemic_class, 6> epistemic_classes{
    epistemic_class::observed,   epistemic_class::imported, epistemic_class::asserted,
    epistemic_class::calculated, epistemic_class::inferred, epistemic_class::accepted,
};

/// "observed", ..., "accepted"; "unknown" for a number outside 1..6.
[[nodiscard]] std::string_view to_string(epistemic_class c) noexcept;

/// One legal change of class.
struct class_transition {
  epistemic_class from;
  epistemic_class to;

  friend constexpr bool operator==(const class_transition &,
                                   const class_transition &) noexcept = default;
};

/// The exact SL-ARCH-001 `epistemic.transitions` table: four promotions to accepted.
inline constexpr std::array<class_transition, 4> legal_transitions{{
    {.from = epistemic_class::imported, .to = epistemic_class::accepted},
    {.from = epistemic_class::asserted, .to = epistemic_class::accepted},
    {.from = epistemic_class::calculated, .to = epistemic_class::accepted},
    {.from = epistemic_class::inferred, .to = epistemic_class::accepted},
}};

/// True when (from, to) is in the matrix, whoever asks.
[[nodiscard]] constexpr bool is_legal_transition(epistemic_class from,
                                                 epistemic_class to) noexcept {
  for (const class_transition &t : legal_transitions) {
    if (t.from == from && t.to == to) {
      return true;
    }
  }
  return false;
}

/// May `who` change a record of `domain` from class `from` to `to`? Ok for a pair in the matrix
/// when `who` holds accept:<domain> (or an ancestor); `invariant_violated` for any other pair
/// (the same class included: that is no transition); `promotion_denied` naming the grant
/// otherwise.
[[nodiscard]] result<void> check_transition(epistemic_class from, epistemic_class to,
                                            const actor &who, std::string_view domain);

// ---- The F3 record ------------------------------------------------------------------------

/// The five F3 field names in field-number order: "input_ids", "input_revisions",
/// "toolchain_id", "parameters_digest", "environment_hash".
inline constexpr std::array<std::string_view, 5> f3_field_names{
    "input_ids", "input_revisions", "toolchain_id", "parameters_digest", "environment_hash",
};

/// The first missing field in field order, or nullopt when the record is complete (rules in the
/// file comment).
[[nodiscard]] std::optional<std::string_view> first_missing_field(const provenance &p) noexcept;

/// Ok for a complete record; `provenance_incomplete` naming the first missing field otherwise.
[[nodiscard]] result<void> check_provenance(const provenance &p);

/// The canonical SLB/1 bytes the derivation key hashes. `provenance_incomplete` for an
/// incomplete record.
[[nodiscard]] result<std::vector<std::uint8_t>> derivation_preimage(const provenance &p);

/// The derivation key (ARCH1-R44): SHA-256 of derivation_preimage(p). `provenance_incomplete`
/// for an incomplete record: an incomplete derivation has no key.
[[nodiscard]] result<digest> derivation_key(const provenance &p);

// ---- The validator ------------------------------------------------------------------------

/// A change of epistemic class one transaction makes to one record.
struct class_change {
  record_id record;
  epistemic_class from = epistemic_class::asserted;
  epistemic_class to = epistemic_class::asserted;

  friend bool operator==(const class_change &, const class_change &) noexcept = default;
};

/// What the provenance validator needs from the transaction (rules in the file comment). Every
/// method fails only when it could not judge.
class provenance_source {
public:
  virtual ~provenance_source() = default;
  /// The provenance on the transaction's origin; nullopt when the transaction has none.
  [[nodiscard]] virtual result<std::optional<provenance>>
  origin(const validation_context &ctx) const = 0;
  /// The per-op override of op number `op_index` of the patch; nullopt when it has none.
  [[nodiscard]] virtual result<std::optional<provenance>>
  override_for(const validation_context &ctx, std::size_t op_index) const = 0;
  /// The epistemic class changes the transaction makes.
  [[nodiscard]] virtual result<std::vector<class_change>>
  class_changes(const validation_context &ctx) const = 0;

protected:
  provenance_source() = default;
  provenance_source(const provenance_source &) = default;
  provenance_source &operator=(const provenance_source &) = default;
  provenance_source(provenance_source &&) = default;
  provenance_source &operator=(provenance_source &&) = default;
};

/// A provenance_source over plain data: an origin, overrides by op index and class changes.
class fixed_provenance_source final : public provenance_source {
public:
  fixed_provenance_source() = default;
  explicit fixed_provenance_source(std::optional<provenance> origin) : origin_(std::move(origin)) {}

  void set_origin(std::optional<provenance> p) { origin_ = std::move(p); }
  void set_override(std::size_t op_index, provenance p) {
    overrides_.insert_or_assign(op_index, std::move(p));
  }
  void add_change(class_change c) { changes_.push_back(std::move(c)); }

  [[nodiscard]] result<std::optional<provenance>>
  origin(const validation_context &ctx) const override;
  [[nodiscard]] result<std::optional<provenance>> override_for(const validation_context &ctx,
                                                               std::size_t op_index) const override;
  [[nodiscard]] result<std::vector<class_change>>
  class_changes(const validation_context &ctx) const override;

private:
  std::optional<provenance> origin_;
  std::map<std::size_t, provenance> overrides_;
  std::vector<class_change> changes_;
};

/// The validator of stage invariants: a record whose type has state class derived must carry
/// complete provenance on the tx origin or a per-op override (an override replaces the origin
/// for its op), and every class change must be a legal transition the origin's actor may make
/// (ARCH2-R14; WO-050 registers it in the built-in chain). Non-owning: the source, the actor
/// directory and the domain registry outlive it.
class provenance_validator final : public validator {
public:
  provenance_validator(const provenance_source &source, const actor_directory &actors,
                       const domain_registry &domains) noexcept
      : source_(&source), actors_(&actors), domains_(&domains) {}

  [[nodiscard]] std::string_view name() const noexcept override { return "fabric.provenance"; }
  [[nodiscard]] validator_stage stage() const noexcept override {
    return validator_stage::invariants;
  }
  [[nodiscard]] result<void> run(const validation_context &ctx,
                                 std::vector<diagnostic> &out) const override;

private:
  const provenance_source *source_;
  const actor_directory *actors_;
  const domain_registry *domains_;
};

} // namespace sl::fabric
