#pragma once

// Immutable snapshots of project state (WO-046, SL-ARCH-001 ARCH1-R51 to ARCH1-R53, SL-ARCH-002
// ARCH2-R02 and ARCH2-R12, ADR-0014).
//
// A snapshot is the state named by one root object (revision.hpp): the records trie (record id
// -> record or tombstone object) and the membership map (document id -> member trie of record
// ids). It holds a few digests and a pointer to the store, so a copy is cheap; it never changes,
// and every read goes to the store (which verifies object ids, ARCH1-R11) without a lock, so any
// number of threads may read one snapshot at once when the store is thread-safe (ARCH1-R53). The
// store must outlive the snapshot and its copies.
//
// Reads see live records only: an id whose leaf is a tombstone reads as absent (entry() shows the
// tombstone). Snapshots implement state_reader (patch.hpp, WO-043) and record_enumerator
// (deps.hpp, WO-044). deps.hpp already names its record view `record_view`, so a snapshot read is
// a `snapshot_entry`.
//
// diff() compares the records tries and skips equal subtrees unread (ARCH1-R53).

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/deps.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/tree.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

/// A live record as stored: the fields of its record object (ARCH1-R52) and the object id.
struct snapshot_entry {
  record_id id;
  type_id type;
  record_id document; // the null id for a record that belongs to no document
  type_version version;
  value body;
  digest object;

  friend bool operator==(const snapshot_entry &, const snapshot_entry &) noexcept = default;
};

/// One changed record between two snapshots: its records-trie entry before and after.
using record_delta = tree_delta;

class snapshot final : public state_reader, public record_enumerator {
public:
  using entry_visitor = std::function<result<void>(const snapshot_entry &)>;

  /// The empty state: no revision, both tries empty. Nothing is read or written.
  [[nodiscard]] static result<snapshot> empty(const store_backend &store);
  /// The state of revision object `revision` (and its root object), verified.
  [[nodiscard]] static result<snapshot> open(const store_backend &store, const digest &revision);
  /// The state of root object `root` with no revision.
  [[nodiscard]] static result<snapshot> open_root(const store_backend &store, const digest &root);

  snapshot(const snapshot &) = default;
  snapshot(snapshot &&) noexcept = default;
  snapshot &operator=(const snapshot &) = default;
  snapshot &operator=(snapshot &&) noexcept = default;
  ~snapshot() override = default;

  /// The live record `id`: `not_found` when absent or retired, `object_corrupt` when the record
  /// object is not a well-formed record object for `id`, the store's errors otherwise.
  [[nodiscard]] result<snapshot_entry> get(const record_id &id) const;
  /// The records-trie entry of `id` (record or tombstone), or nullopt.
  [[nodiscard]] result<std::optional<tree_entry>> entry(const record_id &id) const;
  /// Whether `id` is live.
  [[nodiscard]] result<bool> contains(const record_id &id) const;
  /// Live records plus tombstones.
  [[nodiscard]] result<std::uint64_t> entry_count() const;

  /// The member ids of document `doc`, ascending (empty for an unknown document).
  [[nodiscard]] result<std::vector<record_id>> members(const record_id &doc) const;
  /// Every document with at least one member, ascending.
  [[nodiscard]] result<std::vector<record_id>> documents() const;
  /// Calls `fn` with every live record of document `doc` in ascending id order, through the
  /// membership map (O(document size) reads); stops at the first error.
  [[nodiscard]] result<void> for_each_in_document(const record_id &doc,
                                                  const entry_visitor &fn) const;
  /// Calls `fn` with every live record in ascending id order; stops at the first error.
  [[nodiscard]] result<void> for_each_entry(const entry_visitor &fn) const;

  /// The records-trie differences from this snapshot to `other`, ascending by id. Equal subtrees
  /// are skipped unread; `stats` (optional) counts what was read.
  [[nodiscard]] result<std::vector<record_delta>> diff(const snapshot &other,
                                                       tree_diff_stats *stats = nullptr) const;

  /// The id of the root object (computed for the empty snapshot, which is not stored).
  [[nodiscard]] const digest &root_digest() const noexcept { return root_; }
  /// The revision object id; nullopt for a snapshot opened from a root or empty.
  [[nodiscard]] const std::optional<digest> &revision_digest() const noexcept { return revision_; }
  /// The generation of the revision (0 when there is none).
  [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
  [[nodiscard]] const digest &records_root() const noexcept { return records_; }
  [[nodiscard]] const digest &membership_root() const noexcept { return membership_; }
  [[nodiscard]] const store_backend &store() const noexcept { return *store_; }

  // state_reader: the live record as {type, body}, nullopt when absent or retired.
  [[nodiscard]] result<std::optional<record_state>> read(const record_id &id) const override;
  // record_enumerator: every live record in ascending id order; provenance_inputs is empty
  // (provenance rides on tx records, ARCH1-R45).
  [[nodiscard]] result<void> for_each(const visitor &fn) const override;

private:
  snapshot(const store_backend &store, const digest &root, const digest &records,
           const digest &membership) noexcept
      : store_(&store), root_(root), records_(records), membership_(membership) {}

  [[nodiscard]] result<snapshot_entry> load_record(const tree_entry &e) const;

  const store_backend *store_;
  digest root_;
  digest records_;
  digest membership_;
  std::optional<digest> revision_;
  std::uint64_t generation_ = 0;
};

} // namespace sl::fabric
