#pragma once

// Corruption policies (WO-059, SL-ARCH-002 `corruption` C1-C12, ARCH2-R41 to ARCH2-R43,
// ADR-0015).
//
// The policy table is SL-ARCH-002 `corruption` word for word (corruption_policies()); the flags
// say what each policy does:
//   quarantines     the damaged bytes are copied (or moved) into `.slfabric/quarantine/`
//   read_only_view  a lenient open shows a read-only view instead of the head (C2-C7, C10)
//   repair          repair proposals may exist (C6, C7, C9)
//   refuses         the open is refused and nothing is touched (C8)
//   self_healing    recovery handles it and the store opens normally (C1, C11, C12)
//
// A finding names its class, where it is (file, segment, offset, object id, record id) and the
// evidence; finding::line() is the one-line form sl_fabric_fsck prints (`oid <hex>` or
// `offset <n>` appears whenever it is known).
//
// quarantine() copies or moves bytes into `.slfabric/quarantine/<seq or oid>-<what>` and never
// deletes anything: a name taken by other bytes gets a "-2", "-3" ... suffix, and the same bytes
// under the same name are reused (a rerun quarantines nothing twice).
//
// loss_report: what a recovered view lacks that the store's ref named: the revisions set after the
// recovered one (from the log's intact commit frames) and the records whose records-trie entry
// differs between the recovered revision and the newest lost revision whose records trie still
// reads, with their documents. `exact` is false when no lost revision's trie reads.
//
// Repairs (ARCH2-R42) are built only from verifiable sources: a record object reached by hash
// from a first-parent ancestor revision of the head, verified on read. A proposal restores that
// body (a whole-body set when the record is live, a create when it is absent); nothing is
// invented, so a record with no verifiable earlier version gets no proposal. apply_repairs()
// stages the proposals' ops in one transaction whose origin has epistemic class `inferred`, after
// re-reading every source object and checking that the op carries exactly its body.
//
// Strict and lenient opens (open_checked, src/workspace_corruption.cpp):
//   strict   fsck first (with the lock probe); any finding at all refuses with `store_corrupt`
//            naming the findings (C8 with `format_unsupported`) and nothing is touched; a clean
//            store opens for write through workspace::open
//   lenient  C8 refuses with `format_unsupported`. With only self-healing findings and C9 the
//            store opens for write through workspace::open (recovery repairs C1, C11, C12) and
//            the C9 records are flagged. Otherwise it opens a read-only view: the newest revision
//            of the ref that comes before any log damage (C2, C3, log C4-C6) and whose state
//            closure (revision, root, tries, records, tombstones, tx record) verifies, exposed as
//            the only ref `recovered/<seq>` of a store over the intact objects whose commits are
//            refused (`permission_denied`); the damaged bytes are copied into quarantine/, the
//            loss is reported and the view's records with C7, C9 or C10 findings are flagged.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/log_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/recovery.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/vfs.hpp"
#include "sl/fabric/workspace.hpp"

namespace sl::fabric {

/// One row of SL-ARCH-002 `corruption` and what its policy does.
struct corruption_policy {
  corruption_class cls = corruption_class::c1;
  std::string_view name;
  std::string_view detect;
  std::string_view policy;
  std::string_view evidence;
  bool quarantines = false;
  bool read_only_view = false;
  bool repair = false;
  bool refuses = false;
  bool self_healing = false;
};

/// The twelve rows in class order.
[[nodiscard]] std::span<const corruption_policy> corruption_policies() noexcept;
/// The row of `c` (the C1 row for a value outside 1-12).
[[nodiscard]] const corruption_policy &policy_of(corruption_class c) noexcept;

/// Where a finding is.
struct finding_location {
  /// Relative to `.slfabric`: "log/seg-00000001.slg", "loose/ab/<hex>", "FORMAT", "lock", ...
  std::string file;
  /// The segment number when the finding is in a segment (0 otherwise).
  std::uint32_t segment = 0;
  /// The file offset of the damage (frame or header); 0 when not applicable.
  std::uint64_t offset = 0;
  std::optional<digest> oid;
  std::optional<record_id> record;

  friend bool operator==(const finding_location &, const finding_location &) = default;
};

/// One corruption finding (SL-ARCH-002 `corruption` evidence column).
struct finding {
  corruption_class cls = corruption_class::c1;
  /// journal_corrupt, object_corrupt, object_missing, schema_invalid, format_unsupported,
  /// checkpoint_invalid, writer_locked, id_class_mismatch, ...
  errc code = errc::store_corrupt;
  finding_location where;
  /// The evidence in words.
  std::string detail;
  /// C9: the record the dangling reference names (where.record is the referring record).
  std::optional<record_id> target;
  /// The last valid commit before the finding (0 when none or unknown).
  std::uint64_t seq = 0;
  /// C2: intact commit frames after the damage.
  std::uint64_t commits_after = 0;

  /// "C5 missing object: oid <hex>: <detail>" or "C2 mid-log CRC mismatch:
  /// log/seg-00000001.slg offset 120: <detail>": the file, offset, oid and record when known.
  [[nodiscard]] std::string line() const;

  friend bool operator==(const finding &, const finding &) = default;
};

/// The finding of a recovery_finding (C4 and C5 name `expected` as the object id).
[[nodiscard]] finding from_recovery(const recovery_finding &f);

// ---- quarantine ------------------------------------------------------------------------------

enum class quarantine_method : std::uint8_t { copy = 1, move = 2 };

/// What to quarantine: the file `source` (relative to `store_dir`), whole or the byte range
/// [offset, offset + length) (copy only), under quarantine/<name>.
struct quarantine_item {
  std::filesystem::path store_dir;
  std::filesystem::path source;
  std::string name;
  quarantine_method method = quarantine_method::copy;
  std::optional<std::uint64_t> offset;
  std::optional<std::uint64_t> length;
};

/// Quarantines `what` (file comment) and returns the path written relative to the store
/// directory ("quarantine/<name>" or "quarantine/<name>-2"). `invalid_argument` for an empty name,
/// a name with a '/', or a range with method move; the vfs errors (`not_found` for an absent
/// source, `decode_truncated` for a range past its end). Never removes a file.
[[nodiscard]] result<std::string> quarantine(vfs &fs, const quarantine_item &what,
                                             durability sync = durability::fsync);

// ---- loss ------------------------------------------------------------------------------------

struct loss_report {
  /// Sorted, unique.
  std::vector<record_id> records;
  /// The documents of those records (the null document left out), sorted, unique.
  std::vector<record_id> documents;
  /// The revisions lost, sorted, unique.
  std::vector<digest> revisions;
  /// False when the records could not be enumerated (no lost revision's trie reads).
  bool exact = true;

  friend bool operator==(const loss_report &, const loss_report &) = default;
};

/// The loss of a view at `recovered` (nullopt: nothing recovered, the empty state) against
/// `lost`, the revisions set after it, newest first (file comment). Store errors other than
/// `object_missing` and `object_corrupt` are returned.
[[nodiscard]] result<loss_report> compute_loss(const store_backend &store,
                                               const std::optional<digest> &recovered,
                                               std::span<const digest> lost);

// ---- repair ----------------------------------------------------------------------------------

enum class repair_source : std::uint8_t { ancestor_revision = 1 };

/// A repair from a verifiable source (ARCH2-R42).
struct repair_proposal {
  corruption_class cls = corruption_class::c7;
  /// The record restored.
  record_id record;
  repair_source source = repair_source::ancestor_revision;
  /// The ancestor revision the body comes from and the record object holding it.
  digest source_revision;
  digest source_object;
  /// set(record, whole body) or create(record, type, body).
  patch_op op;
  std::string summary;

  friend bool operator==(const repair_proposal &, const repair_proposal &) = default;
};

/// Proposals for the C6, C7 and C9 findings in `findings` (the classes whose policy has a repair
/// proposal) at revision `head` of `store`: for C6 and C7 the record itself, for C9 the dangling
/// target, restored from the newest first-parent ancestor of `head` where it is live with the type
/// it has at the head (when live there), its record object reads and verifies and (with
/// `schemas`) its body checks under its type. One proposal per record; none when no such ancestor
/// exists. `schemas` may be null (no body checks).
[[nodiscard]] result<std::vector<repair_proposal>>
propose_repairs(const store_backend &store, const digest &head, std::span<const finding> findings,
                const schema_registry *schemas);

/// Who applies repairs.
struct repair_origin {
  actor_id actor;
  std::string subsystem;
};

/// Applies `proposals` as one transaction on `ref` with epistemic class inferred (file comment).
/// `invalid_argument` for no proposals; `object_corrupt` when a source object no longer verifies
/// or an op does not carry exactly its body; the transaction's errors otherwise (diagnostics
/// appended to the message).
[[nodiscard]] result<commit_receipt> apply_repairs(transaction_manager &manager,
                                                   std::span<const repair_proposal> proposals,
                                                   const repair_origin &origin,
                                                   std::string_view ref = "main");

// ---- strict and lenient open (src/workspace_corruption.cpp) ---------------------------------

/// How open_checked() treats findings (ARCH2-R43).
enum class corruption_mode : std::uint8_t { strict = 1, lenient = 2 };

struct checked_open_options {
  /// The ref a lenient view recovers.
  std::string ref = "main";
};

/// What open_checked() opened.
struct checked_open {
  std::unique_ptr<workspace> ws;
  /// Every fsck finding, then the findings of the write open's recovery.
  std::vector<finding> findings;
  /// True for a lenient read-only view.
  bool recovered_view = false;
  /// "recovered/<seq>" for a view.
  std::string recovered_ref;
  std::optional<digest> recovered_revision;
  /// Records of the view (or of the head) with C7, C9 or C10 findings, sorted, unique.
  std::vector<record_id> flagged;
  loss_report loss;
  /// Files quarantined by this open, relative to `.slfabric`.
  std::vector<std::string> quarantined;
};

/// Opens the durable workspace in `root` under `mode` (file comment). `not_found` without a
/// store, `format_unsupported` for C8, `store_corrupt` for a strict refusal or a lenient open
/// where no revision verifies; fsck's and workspace::open's errors otherwise.
[[nodiscard]] result<checked_open> open_checked(const std::filesystem::path &root,
                                                corruption_mode mode, const recovery_policy &policy,
                                                workspace_options options,
                                                const checked_open_options &how = {});

} // namespace sl::fabric
