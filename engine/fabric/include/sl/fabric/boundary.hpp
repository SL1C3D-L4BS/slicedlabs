#pragma once

// The runtime/document boundary (WO-061, SL-ARCH-001 ARCH1-R60 and ARCH1-R61, FAB-INV-BND-01 to
// FAB-INV-BND-03; master plan section 2.1 and section 3, F5).
//
// Document state is never mutated by the runtime. Volume 05 reads a document_projection: an
// immutable, thread-safe view of one snapshot restricted to the records whose type lives in one of
// the requested data domains (a record is in when its type's domain equals a requested domain or
// lies under it, by dotted prefix: "content" covers "content.scene", never "contentx"). The
// projection hands out record views only (const bodies) and has no mutator: no setter, no writable
// view, no way to reach the snapshot, the store or the transaction manager. Document ids, the
// membership of each document and the revision digest are readable so that Volume 05 can key its
// caches on them.
//
// What the runtime learns about the running world enters the document only as a proposed patch:
// propose_from_observation() turns observed changes (record, path, value) into a normalised patch
// based on the projection's revision, and the caller submits that patch through a
// transaction_manager transaction whose origin says epistemic_class::observed (ARCH1-R42:
// observed is immutable and never promoted without accept:<domain>). Nothing here commits or
// applies anything. The proposal is checked against the projection first: a record outside the
// projection's domains is `authority_denied` before a transaction exists, a record that is not
// live is `not_found`, and a patch that does not apply to the projected state fails with the patch
// layer's errors. The manager then runs every validation stage again (structural, schema,
// authority, references, invariants, policy): the early checks narrow what is proposed, they do
// not replace validation.
//
// Include rule R-BND-INCLUDE (tools/fabric/surface_check.py, `just fabric-surface-check`): files
// under engine/runtime and engine/render must not include sl/fabric/internal/**, store.hpp (which
// declares the mutation capability) or f5.hpp (which scopes it); this header and the other public
// read-side headers are what they include.
//
// The projection copies the records it shows once, in make() (O(records in scope)); copies of a
// projection share that data. The snapshot's store must outlive the projection and its copies
// (the schema registry is read only by make()).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/deps.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

/// A read-only view of the records of one snapshot that belong to a set of data domains.
class document_projection final : public state_reader {
public:
  using visitor = std::function<result<void>(const record_view &)>;

  /// The projection of `from` over `domains`: each a dotted domain name ("content.scene") or a
  /// reserved prefix ("content"). `invalid_argument` for an empty list or a name that is neither;
  /// `schema_unknown` when a live record has a type `schemas` does not know; the snapshot's read
  /// errors. A record whose type has no domain is outside every projection.
  [[nodiscard]] static result<document_projection>
  make(const snapshot &from, const schema_registry &schemas, std::span<const std::string> domains);

  document_projection(const document_projection &) = default;
  document_projection(document_projection &&) noexcept = default;
  document_projection &operator=(const document_projection &) = default;
  document_projection &operator=(document_projection &&) noexcept = default;
  ~document_projection() override = default;

  /// The record `id` as a view (its `body` points into this projection and lives as long as it, or
  /// any copy). `not_found` when `id` is not live in the snapshot, `authority_denied` when it is
  /// live but its type is outside the projection's domains.
  [[nodiscard]] result<record_view> get(const record_id &id) const;
  /// Ok when `id` is a record of the projection; the errors of get() otherwise.
  [[nodiscard]] result<void> check_scope(const record_id &id) const;
  /// Calls `fn` with every record of the projection in ascending id order; stops at the first
  /// error `fn` returns (returned unchanged).
  [[nodiscard]] result<void> for_each(const visitor &fn) const;

  /// The documents with at least one record in the projection, ascending.
  [[nodiscard]] std::span<const record_id> documents() const noexcept;
  /// The ids of the projection's records that belong to `document`, ascending (empty for a
  /// document with none).
  [[nodiscard]] std::vector<record_id> members(const record_id &document) const;
  /// Calls `fn` with every record of `document` in the projection in ascending id order.
  [[nodiscard]] result<void> for_each_in_document(const record_id &document,
                                                  const visitor &fn) const;
  /// The document a record of the projection belongs to (the null id for none); the errors of get()
  /// otherwise.
  [[nodiscard]] result<record_id> document_of(const record_id &id) const;

  /// Records in the projection.
  [[nodiscard]] std::size_t size() const noexcept;
  /// The requested domains, sorted and without duplicates.
  [[nodiscard]] std::span<const std::string> domains() const noexcept;
  /// The id of the snapshot's root object: equal for equal document state, so a cache key.
  [[nodiscard]] const digest &root_digest() const noexcept;
  /// The revision object the projection was taken at; nullopt for an empty or root-only snapshot.
  [[nodiscard]] const std::optional<digest> &revision_digest() const noexcept;
  [[nodiscard]] std::uint64_t generation() const noexcept;

  // state_reader: the record as {type, body} (what the patch layer reads, so a proposal can be
  // normalised over the projection); nullopt when it is not live, `authority_denied` outside the
  // projection's domains.
  [[nodiscard]] result<std::optional<record_state>> read(const record_id &id) const override;

private:
  struct data;
  explicit document_projection(std::shared_ptr<const data> d) noexcept : data_(std::move(d)) {}

  std::shared_ptr<const data> data_;
};

/// One thing the runtime observed: the value now at `path` of `record`.
struct observed_change {
  record_id record;
  fabric::path path;
  fabric::value value;

  friend bool operator==(const observed_change &, const observed_change &) noexcept = default;
};

/// The label propose_from_observation() puts on the patch meta.
inline constexpr std::string_view observation_label = "runtime.observation";

/// A patch that sets each observed value, in order, based on the projection's revision, with its
/// expectations filled from the projected state (normalised, ARCH2-R04). `origin` must say
/// epistemic_class::observed (`invalid_argument` otherwise); begin a transaction with the same
/// origin and `base` = the patch's base to commit it. Errors: `invalid_argument` for no changes
/// or a null record id, `authority_denied` for a record outside the projection's domains,
/// `not_found` for a record that is not live, the patch layer's errors for a path that does not
/// apply. Nothing is committed or applied.
[[nodiscard]] result<patch> propose_from_observation(const document_projection &projection,
                                                     std::span<const observed_change> changes,
                                                     const tx_origin &origin);

} // namespace sl::fabric
