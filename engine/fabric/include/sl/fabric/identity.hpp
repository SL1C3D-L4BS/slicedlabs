#pragma once

// Identity lifetime (WO-058, SL-ARCH-001 ARCH1-R04 to ARCH1-R07, ARCH1-R17, ARCH1-R65; ADR-0011,
// ADR-0018).
//
// Lifetime. An id is minted (random) or computed (derived, well-known, imported) before it is
// committed; while no revision holds it, it is unborn. A create makes it live. A delete retires it
// for good: the revision builder (WO-046) replaces its records-trie leaf by a tombstone leaf whose
// tombstone object (generated fabric.identity.tombstone) keeps the id, the object id of the last
// record object written under it and the tx id of the retiring transaction, for every id class.
// A tombstone is never purged (retention is a founder decision, out of scope here) and the id is
// never reused: the only way back to live is the undo of the transaction that retired it, which
// re-creates the captured body under the same id (ARCH2-R27). WO-052's undo and redo keep this:
// an undone create leaves a tombstone retired by the undo, so the redo (whose undo_of names the
// undo) may revive it; a redone delete records the redo as the retirer, so a second undo (whose
// undo_of names the redo) may revive it again.
//
// The identity validator (stage invariants, name "fabric.identity") enforces this on commit over
// every staged change, so the cost is bounded by the changed records (two trie reads for a write
// over a tombstone, none otherwise beyond the type lookup). Rules (severity error, the message
// starts with the error code name):
//   R-INV-ID-REUSE  a record is written under an id whose base leaf is a tombstone and the patch's
//                   meta.undo_of does not name the tombstone's retired_by (id_reuse)
//   R-INV-ID-CLASS  a record is written under an id whose class differs from its type's record
//                   class, by create or by retype (id_class_mismatch; ARCH1-R17)
// The undo_of list checked is the patch meta's full list; the tx record keeps only its first entry
// (WO-050), so a group undo that revives ids retired by several transactions is judged on the
// patch. The validator needs the base to be a snapshot (the transaction manager's); over any other
// state_reader it cannot see tombstones and checks the classes only.
//
// Stable ids (ARCH1-R07). Ids that must survive re-instantiation, re-import and re-cook are pure
// functions of what they stand for, never of position, time or a counter:
//   instance_id_for(parent, source)   derive(instance, parent instance or document id,
//                                            text of the source entity id)
//   derived_id_for(derivation key)    derive(derived, well_known(namespace, 1), 64 lowercase hex
//                                            digits of the key)
//   imported_id_for(class, system, external id)  imported(class, system, external id)
// so re-instantiating a prefab after its entities were reordered, re-importing a changed file and
// re-cooking unchanged inputs land on the ids already in the project, and any overrides that refer
// to them stay attached. An external reference becomes a record id only through an import record
// (generated fabric.identity.import_record) whose record is the imported id (ARCH1-R65).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/gen/fabric.identity.gen.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/provenance.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/validate.hpp"

namespace sl::fabric {

using tombstone = gen::fabric_identity::tombstone;
using import_record = gen::fabric_identity::import_record;

/// Where an id stands in one snapshot.
enum class id_lifetime : std::uint8_t {
  unborn = 0,     // minted or computed, held by no leaf
  live = 1,       // a record leaf
  tombstoned = 2, // a tombstone leaf: retired for good
};

/// "unborn", "live", "tombstoned"; "unknown" otherwise.
[[nodiscard]] std::string_view to_string(id_lifetime l) noexcept;

/// True for the classes whose ids may name a record: every assigned class except tx, event,
/// element and operation (which name transactions, events, oset elements and operations).
[[nodiscard]] bool names_record(id_class c) noexcept;

/// The lifetime of `id` in `s`; the store's errors otherwise.
[[nodiscard]] result<id_lifetime> lifetime_of(const snapshot &s, const record_id &id);

/// The tombstone of `id` in `s`, or nullopt when `id` is live or unborn. `object_corrupt` when the
/// tombstone object names another id; the store's and decoder's errors otherwise.
[[nodiscard]] result<std::optional<tombstone>> tombstone_of(const snapshot &s, const record_id &id);

/// Whether a patch with `meta` may revive an id retired by transaction `retired_by`: true exactly
/// when meta.undo_of names it.
[[nodiscard]] bool may_revive(const patch_meta &meta, const record_id &retired_by) noexcept;

/// The id of the instance of `source` (an entity, asset or instance id) placed under `parent` (an
/// instance id or the id of a document: class instance, entity or asset):
/// derive(instance, parent, source.to_string()). `invalid_argument` for a null id,
/// `id_class_mismatch` for a class outside those.
[[nodiscard]] result<instance_id> instance_id_for(const record_id &parent, const record_id &source);

/// The id of the derived resource of `key` (ARCH1-R44):
/// derive(derived, well_known(namespace, 1), key.to_hex()). `invalid_argument` for the all-zero
/// digest (the "no digest" sentinel) or an algorithm other than SHA-256.
[[nodiscard]] result<derived_id> derived_id_for(const digest &key);
/// derived_id_for(derivation_key(p)); `provenance_incomplete` for an incomplete record.
[[nodiscard]] result<derived_id> derived_id_for(const provenance &p);

/// imported(c, system, external_id). `id_class_mismatch` when `c` names no record or is not
/// assigned; `invalid_argument` for an empty system or external id.
[[nodiscard]] result<record_id> imported_id_for(id_class c, std::string_view system,
                                                std::string_view external_id);

/// The import record of an external object: its record is imported_id_for(c, system,
/// external_id); errors as imported_id_for, and `invalid_argument` for an empty locator or an
/// all-zero content digest.
[[nodiscard]] result<import_record> make_import_record(id_class c, std::string_view system,
                                                       std::string_view external_id,
                                                       std::string_view locator,
                                                       const digest &content);

/// Ok when `r` is a well-formed import record: system, external id and locator non-empty, content
/// not all zero, and record equal to imported(class of record, system, external id).
/// `invalid_argument` naming the first broken rule otherwise.
[[nodiscard]] result<void> check_import_record(const import_record &r);

/// The identity validator (file comment). Stateless.
class identity_validator final : public validator {
public:
  [[nodiscard]] std::string_view name() const noexcept override { return "fabric.identity"; }
  [[nodiscard]] validator_stage stage() const noexcept override {
    return validator_stage::invariants;
  }
  [[nodiscard]] result<void> run(const validation_context &ctx,
                                 std::vector<diagnostic> &out) const override;
};

} // namespace sl::fabric
