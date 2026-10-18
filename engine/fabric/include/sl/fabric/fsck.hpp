#pragma once

// fsck: the read-only check of a durable store (WO-059, SL-ARCH-002 ARCH2-R41 and ARCH2-R44,
// `corruption` C1-C12, ADR-0015).
//
// fsck() never writes, never repairs and takes no lock (the C11 probe takes the writer lock for
// an instant and releases it without touching the file). It runs, collecting findings:
//   FORMAT       a newer major or unrecognised text is C8 and ends the check (nothing else read)
//   lock         with probe_lock: a holder text in `.slfabric/lock` that no live writer locks is
//                a stale lock (C11)
//   log          every segment header (C2 for damage, C3 for a wrong number or first seq, C8 for
//                a newer major) and every frame: a frame failure is followed by a resync to the
//                next intact frame; in the last segment a failure with nothing valid after it is
//                a torn tail (C1), anything else is mid-log damage (C2, with the intact commits
//                after it). Intact frames are indexed: an object frame whose payload is not an
//                SLFO frame or an undecodable commit or intent payload is C6. Each commit is
//                checked for seq and prev (C3) and objects digest (C4) unless damage since the
//                previous commit already explains it; listed loose blobs must be present (C5) and
//                hash to their name (C4). Ref updates of every intact commit are applied in log
//                order (the heads fsck walks). Whole frames after the last commit are orphans
//                (counted, as recovery counts them, not findings).
//   checkpoints  each must decode (checksum, layout) and name an intact commit frame with its
//                seq and payload digest (C12)
//   pins         `.slfabric/pins/<name>` must hold a 64-hex revision digest (C6 otherwise)
//   reachability from every head and pin through revision_links: an object not stored is C5, one
//                whose bytes do not hash to its id is C4, one whose links or record layout do not
//                decode is C6 (each object id reported once per class)
//   records      for every head and pin revision (check_records): a trie entry whose record
//                object names another id, or (with schemas) a record whose id class is not its
//                type's record class is C10; a body that fails its type under the schemas is C7;
//                a strong or owned reference to a record that is not live is C9 (with the path)
// Findings come in that order. The report also lists every intact object and commit frame, so
// that tools can name them (sl_fabric_fsck --objects) and a lenient open can build its view.
//
// open_salvage_store() is a read-only store over the intact objects of a report: get() reads the
// indexed bytes and verifies them, refs are the ones given, commit() is `permission_denied`.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/corruption.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/vfs.hpp"

namespace sl::fabric {

struct fsck_options {
  /// The registry C7, C9 and C10 are judged against (frozen); types it lacks are not judged.
  /// Null: no schema checks.
  const schema_registry *schemas = nullptr;
  /// Probe the writer lock for a stale holder (C11).
  bool probe_lock = true;
  /// Check the records of every head and pin revision (C6, C7, C9, C10).
  bool records = true;
};

/// An intact object: an object frame of a segment or a loose blob.
struct fsck_object {
  digest id;
  /// Relative to `.slfabric`.
  std::string file;
  /// The frame offset (0 for a loose blob).
  std::uint64_t frame = 0;
  /// Where the SLFO bytes start and how long they are.
  std::uint64_t offset = 0;
  std::uint64_t length = 0;
  bool loose = false;

  friend bool operator==(const fsck_object &, const fsck_object &) = default;
};

/// An intact commit frame.
struct fsck_commit {
  std::uint64_t seq = 0;
  std::uint32_t segment = 0;
  std::uint64_t offset = 0;
  /// commit_digest of its payload.
  digest payload;
  digest revision;
  std::vector<ref_update> refs;
  /// The commit passed every check (chain, objects digest, loose blobs).
  bool valid = false;
  /// It and every commit before it are valid and no frame damage precedes it.
  bool intact_prefix = false;

  friend bool operator==(const fsck_commit &a, const fsck_commit &b) noexcept {
    return a.seq == b.seq && a.segment == b.segment && a.offset == b.offset &&
           a.payload == b.payload && a.revision == b.revision && a.valid == b.valid &&
           a.intact_prefix == b.intact_prefix && a.refs.size() == b.refs.size();
  }
};

struct fsck_report {
  /// Every finding, in the order of the file comment.
  std::vector<finding> findings;
  /// The heads the log's intact commits set, sorted by name.
  std::vector<ref_entry> heads;
  /// The pins, sorted by name.
  std::vector<ref_entry> pins;
  /// Every intact object, in log order then loose blobs in commit order (each id once).
  std::vector<fsck_object> objects;
  /// Every intact commit frame, in log order.
  std::vector<fsck_commit> commits;
  std::uint32_t segments = 0;
  std::uint64_t frames = 0;
  std::uint64_t orphan_frames = 0;
  std::uint64_t objects_reached = 0;
  std::uint64_t records_checked = 0;
  /// Records whose type the registry lacks (not judged for C7, C9, C10).
  std::uint64_t records_unjudged = 0;
  /// FORMAT or a segment names a newer minor: readable, never written by this engine.
  bool read_only_newer = false;
  /// The lock probe found a live writer (findings may be transient).
  bool live_writer = false;

  /// True when there is no finding.
  [[nodiscard]] bool clean() const noexcept { return findings.empty(); }
};

/// Checks the store of the workspace directory `root` (or `root` itself when it is a `.slfabric`
/// directory holding FORMAT). `not_found` when there is no store; the vfs errors when a file
/// cannot be read. Findings are data, never errors.
[[nodiscard]] result<fsck_report> fsck(vfs &fs, const std::filesystem::path &root,
                                       fsck_options options = {});

/// The record checks of fsck for one revision (C6, C7, C9, C10; file comment). An unreadable
/// revision, root or trie node is skipped (the reachability walk reports it).
[[nodiscard]] result<std::vector<finding>>
check_records(const store_backend &store, const digest &revision, const schema_registry *schemas);

/// True when the state closure of `revision` verifies: the revision, its root and tx record, every
/// node of both tries and every record and tombstone object read, hash to their ids and decode.
[[nodiscard]] result<bool> state_closure_verifies(const store_backend &store,
                                                  const digest &revision);

/// The read-only store over the intact objects of `report` for the store directory `store_dir`
/// of `fs` (file comment), with the refs `refs` (sorted by name, unique; `invalid_argument`
/// otherwise).
[[nodiscard]] result<std::shared_ptr<store_backend>>
open_salvage_store(std::shared_ptr<vfs> fs, const std::filesystem::path &store_dir,
                   const fsck_report &report, std::vector<ref_entry> refs);

} // namespace sl::fabric
