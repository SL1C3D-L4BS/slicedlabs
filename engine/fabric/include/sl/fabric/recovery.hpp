#pragma once

// Recovery of the commit log (WO-055, SL-ARCH-002 `recovery_steps` R1-R10, ARCH2-R35 to R40,
// `corruption` classes C1-C12, ADR-0015).
//
// recover_log() is what opening a workspace for write runs after it holds the writer lock (R1,
// workspace.hpp):
//   R2  FORMAT must read "slfabric 1.0" (C8: a newer major is `format_unsupported`, nothing is
//       touched; a newer minor is `read_only_newer`)
//   R3  the newest checkpoint whose checksum verifies and whose commit frame re-reads with the
//       recorded seq and payload digest; otherwise segment 1 offset 24. An invalid checkpoint is
//       a C12 finding and is moved to quarantine/ (never trusted, never deleted)
//   R4  frames are scanned (length bounds, CRC, type). In the last segment the first failure with
//       no valid frame after it (a resync tries every later offset) is a torn tail (C1); a failure
//       followed by a valid frame, any failure in an earlier segment, a damaged segment header
//       and an unknown frame type are mid-log damage (C2)
//   R5  every commit: seq + 1 and prev (C3), objects digest (C4), loose blobs present (C5) and
//       verifying (C4); ref updates must chain and intents must bracket (C3)
//   R6  object frames after the last valid commit are orphans: counted and truncated, never
//       visible
//   R7  intent_begin without intent_end: open intents, reported with the options continue or
//       compensate (roll-forward is never automatic, ARCH2-R37)
//   R8  torn and orphan bytes are copied to quarantine/ (synced) before the segment is truncated
//       at the failure offset (synced); a torn segment header is quarantined and rewritten; loose
//       `.tmp` files of interrupted commits are moved to quarantine/
//   R9  refs are rebuilt from the checkpoint plus the commit ref updates and must equal the
//       store's; every ref target and every link of every object scanned must be stored (C5):
//       checked in the scan when it started at segment 1, against the opened store otherwise
//   R10 the last segment is synced, a checkpoint of the last commit is written, older checkpoints
//       beyond keep_checkpoints are removed, and the report is returned
// Damage truncation cannot remove (C2-C6, C8) is reported and left untouched: the default policy
// refuses the write open (`journal_corrupt` from workspace::open); the read-only recovery view
// and repairs are WO-059. In inspect mode recovery only reads: it reports what repair would do
// and opens the store only when the log is clean.
//
// Starting at a checkpoint, recovery reads and hashes only the frames after it: its work is
// O(delta since the checkpoint) (frames_scanned, bytes_scanned). log_backend::open still hashes
// every object when it rebuilds its index (WO-053; recorded for WO-062).

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/checkpoint.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/log_backend.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/vfs.hpp"

namespace sl::fabric {

/// SL-ARCH-002 `corruption`; the number is the class number.
enum class corruption_class : std::uint8_t {
  c1 = 1,   ///< torn tail
  c2 = 2,   ///< mid-log CRC mismatch (any damage to frames or segment headers)
  c3 = 3,   ///< broken chain or seq gap
  c4 = 4,   ///< object hash mismatch
  c5 = 5,   ///< missing object
  c6 = 6,   ///< undecodable record (an intact frame whose payload does not parse)
  c7 = 7,   ///< schema-invalid record
  c8 = 8,   ///< unknown newer format
  c9 = 9,   ///< dangling strong reference at rest
  c10 = 10, ///< id-class confusion
  c11 = 11, ///< stale lock
  c12 = 12, ///< corrupt checkpoint
};

/// "C1" .. "C12"; "C?" otherwise.
[[nodiscard]] std::string_view to_string(corruption_class c) noexcept;
/// "torn tail", "mid-log CRC mismatch", ... as SL-ARCH-002 names them.
[[nodiscard]] std::string_view corruption_name(corruption_class c) noexcept;

/// One finding with its evidence (SL-ARCH-002 `corruption` evidence column).
struct recovery_finding {
  corruption_class kind = corruption_class::c1;
  /// journal_corrupt (C1-C6), checkpoint_invalid (C12), writer_locked (C11).
  errc code = errc::journal_corrupt;
  /// The file, relative to `.slfabric`: "log/seg-00000001.slg", "checkpoint/ckpt-...", "lock".
  std::string file;
  /// The file offset of the damage (frame or segment header); 0 when not applicable.
  std::uint64_t offset = 0;
  /// The last valid commit before the finding.
  std::uint64_t seq = 0;
  /// C2: intact commit frames found after the damage (in the damaged segment and later ones).
  std::uint64_t commits_after = 0;
  /// C3: the expected prev digest; C4 and C5: the object id.
  std::optional<digest> expected;
  /// C3: the prev digest found; C4: what the bytes hash to.
  std::optional<digest> found;
  /// True when the finding refuses a write open under the default policy.
  bool blocking = false;
  /// "C2 mid-log CRC mismatch: log/seg-00000001.slg offset 120: frame crc mismatch; 3 commits
  /// after".
  std::string message;

  friend bool operator==(const recovery_finding &, const recovery_finding &) = default;
};

/// An open intent (R7) and what may be done with it.
struct open_intent {
  record_id operation;
  std::string kind;
  /// Always {"continue", "compensate"} (ARCH2-R37: the default is to leave it to the caller).
  std::vector<std::string> options;

  friend bool operator==(const open_intent &, const open_intent &) = default;
};

/// ARCH2-R40: what recovery found and did.
struct recovery_report {
  /// Bytes cut from the log by R8 (torn and orphan bytes; a torn segment header).
  std::uint64_t lost_bytes = 0;
  /// Frames cut by R8; a partial frame counts as one.
  std::uint64_t truncated_frames = 0;
  /// Object frames after the last valid commit (R6).
  std::uint64_t orphan_objects = 0;
  std::vector<open_intent> open_intents;
  /// Per-ref recovered heads, sorted by name (R9).
  std::vector<ref_entry> heads;
  bool used_checkpoint = false;
  /// Files moved or copied into quarantine/ by this run, relative to `.slfabric`.
  std::vector<std::string> quarantined;
  /// Every finding, in the order found.
  std::vector<recovery_finding> findings;

  /// The last valid commit.
  std::uint64_t last_seq = 0;
  /// The seq of the checkpoint used (0 when none).
  std::uint64_t checkpoint_seq = 0;
  /// Frames read and checked by R4-R6 (the checkpoint's own commit frame not included).
  std::uint64_t frames_scanned = 0;
  /// Log bytes read by R4-R6 (segment headers and the checkpoint's commit frame not included).
  std::uint64_t bytes_scanned = 0;
  /// Valid commit frames replayed by R5.
  std::uint64_t commits_scanned = 0;
  /// The checkpoint written by R10 (0 when none).
  std::uint64_t checkpoint_written = 0;
  /// R1: the previous holder text of a stale lock that was taken over (C11).
  std::optional<std::string> stale_lock;

  /// True when any finding is blocking.
  [[nodiscard]] bool blocking() const noexcept;
  /// True when nothing was lost, truncated, orphaned, quarantined or found.
  [[nodiscard]] bool lossless() const noexcept;
  /// One line per finding plus the loss counts, for error messages and logs.
  [[nodiscard]] std::string summary() const;

  friend bool operator==(const recovery_report &, const recovery_report &) = default;
};

/// repair runs R8 and R10 (the caller holds the writer lock); inspect only reads.
enum class recovery_mode : std::uint8_t { repair = 1, inspect = 2 };

struct recovery_policy {
  /// R3: start at the newest valid checkpoint (false: always scan the whole log).
  bool use_checkpoint = true;
  /// R10: write a checkpoint after a repair that saw commits past the one used.
  bool write_checkpoint = true;
  /// R9: check that every link of every scanned object and every head is stored (C5).
  bool verify_links = true;
  /// R10: checkpoints kept; older ones are removed (at least 1).
  std::uint32_t keep_checkpoints = 2;
  /// false: stop after the repair without opening the store, for callers that want only the
  /// report (the heads come from the scan; links that a checkpoint scan defers to the store stay
  /// unchecked, and then no checkpoint is written).
  bool open_store = true;
};

/// A recovered log: the opened store, or null when recovery refused (a blocking finding) or, in
/// inspect mode, when the log needs a repair first.
struct recovered_log {
  std::unique_ptr<log_backend> store;
  recovery_report report;
};

/// R2-R10 over the `.slfabric` directory `store_dir` (see the file comment); `log` configures the
/// opened store and the syncs recovery issues. Errors: `not_found` when store_dir holds no FORMAT
/// (no store, or a creation that did not finish), `format_unsupported` and `read_only_newer`
/// (C8, nothing touched), `store_corrupt` for an unreadable FORMAT, the vfs errors and the
/// errors of log_backend::open. Findings are data in the report, never errors.
[[nodiscard]] result<recovered_log> recover_log(vfs &fs, const std::filesystem::path &store_dir,
                                                const recovery_policy &policy,
                                                const log_options &log = {},
                                                recovery_mode mode = recovery_mode::repair);

} // namespace sl::fabric
