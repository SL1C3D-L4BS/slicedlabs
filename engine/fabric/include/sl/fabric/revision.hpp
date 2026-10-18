#pragma once

// Revision objects and the revision builder (WO-046, SL-ARCH-001 ARCH1-R22, ARCH1-R50 to
// ARCH1-R52, SL-ARCH-002 ARCH2-R07 step C3, ADR-0014).
//
// Object layouts (SLB/1 payloads; none holds a time field):
//   record    (kind 2)  struct {1 id id, 2 type id, 3 document id (absent for none),
//                               4 type major uint, 5 type minor uint, 6 body}      (ARCH1-R52)
//   tombstone (kind 3)  fabric.identity.tombstone {1 id, 2 last_record, 3 retired_by} (ARCH1-R05)
//   tree      (kind 4)  tree.hpp
//   root      (kind 5)  struct {1 records trie digest, 2 membership map digest}     (ARCH1-R51)
//   tx        (kind 7)  fabric.history.tx_record
//   revision  (kind 6)  fabric.history.revision {1 parents, 2 root, 3 schema_set, 4 tx,
//                                                5 generation}                     (ARCH1-R50)
// The record object layout is not fixed by SL-ARCH-001 beyond its field set; this one is the
// WO-046 choice (recorded for WO-062).
//
// revision_builder turns a base snapshot plus staged record changes into the new objects (record
// and tombstone objects, the trie nodes on the changed paths, the member tries and membership map
// entries of the documents touched, the root, the tx record and the revision) and a commit_batch
// that moves no ref: moving a head is the transaction manager's (WO-050). The schema_set digest
// comes from the codec (WO-041). Objects the store already holds are left out of the batch,
// except the revision object, which is always written.
//
// Changing one record's body writes at most depth + 4 objects: the record object, one node per
// level of the records trie, the root, the tx record and the revision (FAB-INV-REV-05).

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/gen/fabric.history.gen.hpp"
#include "sl/fabric/gen/fabric.identity.gen.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

/// The fields of a record object (ARCH1-R52).
struct record_object {
  record_id id;
  type_id type;
  record_id document;
  type_version version;
  value body;

  friend bool operator==(const record_object &, const record_object &) noexcept = default;
};

/// The root object: the records trie and the membership map (ARCH1-R51).
struct root_object {
  digest records;
  digest membership;

  friend bool operator==(const root_object &, const root_object &) noexcept = default;
};

/// `invalid_argument` for a null id or type.
[[nodiscard]] result<object> encode_record_object(const record_object &r);
/// `object_corrupt` for another kind or a payload that is not exactly the record layout.
[[nodiscard]] result<record_object> decode_record_object(const object &o);

[[nodiscard]] result<object> encode_root_object(const root_object &r);
[[nodiscard]] result<root_object> decode_root_object(const object &o);

[[nodiscard]] result<object> encode_revision(const gen::fabric_history::revision &r);
[[nodiscard]] result<gen::fabric_history::revision> decode_revision(const object &o);

[[nodiscard]] result<object> encode_tx_record(const gen::fabric_history::tx_record &t);
[[nodiscard]] result<gen::fabric_history::tx_record> decode_tx_record(const object &o);

[[nodiscard]] result<object> encode_tombstone(const gen::fabric_identity::tombstone &t);
[[nodiscard]] result<gen::fabric_identity::tombstone> decode_tombstone(const object &o);

/// The links of revision-store objects, for store verify: revision -> parents, root and tx; root
/// -> both tries; tree -> children and leaf targets; tombstone -> last record; tx -> patch.
/// Records and blobs link to nothing here, and the empty trie is never a link.
class revision_links final : public link_reader {
public:
  [[nodiscard]] result<void> append_links(const object &obj,
                                          std::vector<digest> &out) const noexcept override;
};

/// A record to write: its type, owning document (null for none), body and the type version it
/// is written with (nullopt: the version registered for the type).
struct record_write {
  type_id type;
  record_id document;
  value body;
  std::optional<type_version> version;
};

/// What the builder needs besides the changes: the tx record it writes (kind 7), the tx id that
/// retires ids (tombstone retired_by), and parents beyond the base revision (a merge).
struct revision_inputs {
  gen::fabric_history::tx_record tx;
  record_id tx_id;
  std::vector<digest> extra_parents;
};

/// What a build produced: the new objects in `batch.objects` (records and tombstones, trie nodes
/// children first, the root, the tx record, the revision last) and no ref update.
struct built_revision {
  root_object root_value;
  digest root;
  digest tx;
  gen::fabric_history::revision revision_value;
  digest revision;
  commit_batch batch;
};

class revision_builder {
public:
  /// Maps a staged record to its owning document.
  using document_resolver =
      std::function<result<record_id>(const record_id &, const record_state &)>;

  /// Changes over `base`; `schemas` gives type versions and the schema_set digest. Both must
  /// outlive the builder.
  revision_builder(const snapshot &base, const schema_registry &schemas) noexcept
      : base_(base), schemas_(&schemas) {}

  /// Stages a write of `id` (replacing any earlier staged change of it). `invalid_argument` for
  /// a null id or type.
  [[nodiscard]] result<void> put(const record_id &id, record_write w);
  /// Stages the retirement of `id`: a tombstone that keeps its last record object. The id must
  /// be live in the base or staged (`not_found` at build otherwise).
  [[nodiscard]] result<void> retire(const record_id &id);
  /// Stages every change of a staged_state (patch.hpp): a removed record is retired, a written
  /// one keeps the document of its base record unless `document_of` is given.
  [[nodiscard]] result<void> stage(std::span<const staged_change> changes,
                                   const document_resolver &document_of = {});

  [[nodiscard]] std::size_t staged() const noexcept { return changes_.size(); }
  [[nodiscard]] const snapshot &base() const noexcept { return base_; }

  /// Builds the objects and the batch; the builder is unchanged. `invalid_argument` for a null
  /// tx id when an id is retired or for a repeated parent; `schema_unknown` for a write with no
  /// version whose type is not registered; `not_found` for retiring an id that is not live.
  [[nodiscard]] result<built_revision> build(const revision_inputs &in) const;

private:
  snapshot base_;
  const schema_registry *schemas_;
  std::map<record_id, std::optional<record_write>> changes_;
};

} // namespace sl::fabric
