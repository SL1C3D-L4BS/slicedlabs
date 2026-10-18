#pragma once

// Checkpoints of the commit log (WO-055, SL-ARCH-002 `journal` checkpoint, ARCH2-R32 and R39,
// recovery step R3 and R10, ADR-0015).
//
// A checkpoint is a derived accelerator: recovery may start scanning the log where the checkpoint
// says instead of at segment 1, and the result must equal a scan of the whole log (ARCH2-R39).
// The file `.slfabric/checkpoint/ckpt-<16 hex seq>.slc` is
//   'SLCK' | major u8 = 1 | minor u8 = 0 | SLB/1 struct | SHA-256 of every preceding byte
// with the struct
//   {1 seq uint, 2 segment uint, 3 offset after the commit frame uint,
//    4 last commit payload digest, 5 refs list {1 name string, 2 target digest},
//    6 schema_set digest (absent when unknown), 7 open intents list {1 operation id, 2 kind
//    string}, 8 offset of the last commit frame uint}
// Field 8 is a WO-055 addition (recorded for WO-062): it lets step R3 re-read the commit frame the
// checkpoint names and check its seq and payload digest. A newer minor may add fields; they are
// ignored. Anything else that does not verify is `checkpoint_invalid`: the checkpoint is ignored,
// reported (corruption class C12) and quarantined, never trusted.
//
// write_checkpoint is atomic: checkpoint/ckpt.tmp, sync, rename, directory sync.

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/log_backend.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/vfs.hpp"

namespace sl::fabric {

inline constexpr std::array<std::uint8_t, 4> checkpoint_magic = {'S', 'L', 'C', 'K'};
inline constexpr std::uint8_t checkpoint_major = 1;
inline constexpr std::uint8_t checkpoint_minor = 0;
/// The name of the checkpoint directory inside `.slfabric`.
inline constexpr std::string_view checkpoint_dir_name = "checkpoint";

/// An operation whose intent_begin has no intent_end (ARCH2-R36).
struct checkpoint_intent {
  record_id operation;
  std::string kind;

  friend bool operator==(const checkpoint_intent &, const checkpoint_intent &) = default;
};

/// The state of the log after commit `seq`.
struct checkpoint {
  std::uint64_t seq = 0;            ///< 1, the last commit covered
  std::uint32_t segment = 1;        ///< 2, the segment holding that commit frame
  std::uint64_t offset = 0;         ///< 3, the file offset just after the commit frame
  digest last_commit;               ///< 4, commit_digest of that commit payload
  std::vector<ref_entry> refs;      ///< 5, every ref, sorted by name
  std::optional<digest> schema_set; ///< 6, of the newest revision committed (nullopt: unknown)
  std::vector<checkpoint_intent> open_intents; ///< 7, sorted by operation
  std::uint64_t frame_offset = 0;              ///< 8, the file offset of the commit frame

  friend bool operator==(const checkpoint &, const checkpoint &) = default;
};

/// "ckpt-000000000000002a.slc" for seq 42.
[[nodiscard]] std::string checkpoint_file_name(std::uint64_t seq);
/// The seq of a checkpoint file name; nullopt for any other name.
[[nodiscard]] std::optional<std::uint64_t>
parse_checkpoint_file_name(std::string_view name) noexcept;

/// The file bytes of `c`. `invalid_argument` for refs that are not sorted and unique, open
/// intents that are not sorted and unique, or a frame offset not below the offset.
[[nodiscard]] result<std::vector<std::uint8_t>> encode_checkpoint(const checkpoint &c);
/// Parses and verifies checkpoint file bytes: `checkpoint_invalid` naming the reason for a bad
/// magic, another major, a checksum mismatch, a payload that is not the struct above, refs or
/// intents out of order, or offsets out of order.
[[nodiscard]] result<checkpoint> decode_checkpoint(byte_span bytes);

/// Writes `c` as store_dir/checkpoint/<checkpoint_file_name(c.seq)> atomically, creating the
/// directory when needed; `sync` as the log's (none skips every sync).
[[nodiscard]] result<void> write_checkpoint(vfs &fs, const std::filesystem::path &store_dir,
                                            const checkpoint &c,
                                            durability sync = durability::fsync) noexcept;
/// The checkpoint file names of store_dir, newest (highest seq) first; other names are skipped.
/// An absent directory has none.
[[nodiscard]] result<std::vector<std::string>>
list_checkpoints(vfs &fs, const std::filesystem::path &store_dir) noexcept;

} // namespace sl::fabric
