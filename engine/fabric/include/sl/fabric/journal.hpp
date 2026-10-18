#pragma once

// The commit log byte format (WO-053, SL-ARCH-002 `journal`, ARCH2-R29, R30 and R31, ADR-0015).
//
// A log is a sequence of segment files `.slfabric/log/seg-NNNNNNNN.slg`. Each starts with a
// 24-byte header
//   'SLGF' | major u8 = 1 | minor u8 = 0 | reserved u16 = 0 | segment number u32le |
//   first seq u64le | crc32c u32le over bytes 0-19
// followed by frames
//   len u32le (= 1 + payload bytes, 1..64 MiB) | crc32c u32le over (type || payload) | type u8 |
//   payload
// Frame types: 1 object (one SLFO frame), 2 commit (an SLB/1 struct, fields 1-10 below),
// 3 intent_begin, 4 intent_end, 5 note (ignored). Types 6-127 (and 0) are `journal_corrupt`;
// 128-255 are notes of a newer minor and are skipped. CRC-32C is the Castagnoli polynomial,
// reflected, init 0xFFFFFFFF, final xor 0xFFFFFFFF; all integers are little-endian.
//
// A commit is the object frames since the previous commit, then the commit frame. It is valid iff
// its CRC is intact, its seq is the previous seq + 1, its `prev` is the SHA-256 of the previous
// commit payload (absent for seq 1), and the object frames since the previous commit hash to
// `objects_digest` (SHA-256 over their 33-byte object ids in log order) with `object_count`
// frames (ARCH2-R30).

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/vfs.hpp"

namespace sl::fabric {

// ---- CRC-32C ----

/// Extends a CRC-32C: crc32c_extend(crc32c(a), b) == crc32c(a || b).
[[nodiscard]] std::uint32_t crc32c_extend(std::uint32_t crc, byte_span data) noexcept;
/// The CRC-32C of `data` (crc32c("123456789") == 0xE3069283).
[[nodiscard]] inline std::uint32_t crc32c(byte_span data) noexcept {
  return crc32c_extend(0, data);
}

// ---- layout constants ----

inline constexpr std::array<std::uint8_t, 4> segment_magic = {'S', 'L', 'G', 'F'};
inline constexpr std::uint8_t journal_major = 1;
inline constexpr std::uint8_t journal_minor = 0;
inline constexpr std::size_t segment_header_size = 24;
/// len u32le, crc u32le, type u8.
inline constexpr std::size_t frame_prefix_size = 9;
/// The largest `len` of a frame: 64 MiB.
inline constexpr std::uint32_t max_frame_len = std::uint32_t{1} << 26U;
/// A segment rotates once it would grow past 256 MiB.
inline constexpr std::uint64_t segment_rotate_size = std::uint64_t{1} << 28U;
/// Objects whose SLFO frame is larger than 1 MiB are stored as loose files.
inline constexpr std::size_t loose_object_threshold = std::size_t{1} << 20U;
/// The text of `.slfabric/FORMAT`.
inline constexpr std::string_view format_text = "slfabric 1.0\n";

enum class frame_type : std::uint8_t {
  object = 1,
  commit = 2,
  intent_begin = 3,
  intent_end = 4,
  note = 5,
};

/// How a frame type byte is treated (ARCH2-R31).
enum class frame_type_class : std::uint8_t { known, corrupt, skipped };
[[nodiscard]] constexpr frame_type_class classify_frame_type(std::uint8_t type) noexcept {
  if (type >= 1 && type <= 5) {
    return frame_type_class::known;
  }
  return type >= 128 ? frame_type_class::skipped : frame_type_class::corrupt;
}

/// The file name of segment `number`: "seg-00000001.slg" (8 decimal digits).
[[nodiscard]] std::string segment_file_name(std::uint32_t number);
/// The segment number of a segment file name; nullopt for any other name.
[[nodiscard]] std::optional<std::uint32_t> parse_segment_file_name(std::string_view name) noexcept;

// ---- segment header ----

struct segment_header {
  std::uint8_t major = journal_major;
  std::uint8_t minor = journal_minor;
  std::uint32_t segment = 1;
  std::uint64_t first_seq = 1;

  friend constexpr bool operator==(const segment_header &, const segment_header &) = default;
};

[[nodiscard]] std::array<std::uint8_t, segment_header_size>
encode_segment_header(const segment_header &h) noexcept;
/// `decode_truncated` under 24 bytes; `journal_corrupt` for a bad magic, CRC or a nonzero reserved
/// field; `format_unsupported` for a major other than 1. A newer minor parses.
[[nodiscard]] result<segment_header> parse_segment_header(byte_span bytes) noexcept;

// ---- frames ----

/// One frame: `encode_invalid` when 1 + payload exceeds max_frame_len.
[[nodiscard]] result<std::vector<std::uint8_t>> encode_frame(std::uint8_t type, byte_span payload);
[[nodiscard]] inline result<std::vector<std::uint8_t>> encode_frame(frame_type type,
                                                                    byte_span payload) {
  return encode_frame(static_cast<std::uint8_t>(type), payload);
}

/// Why a frame scan stopped.
enum class frame_fault : std::uint8_t {
  none = 0,          ///< the input ended exactly after a frame
  truncated_prefix,  ///< fewer than 9 bytes left
  bad_length,        ///< len 0 or above max_frame_len
  truncated_payload, ///< the payload runs past the end
  crc_mismatch,      ///< the CRC does not cover (type || payload)
  unknown_type,      ///< a type in 0 or 6-127 under an intact CRC (journal_corrupt)
};
[[nodiscard]] std::string_view to_string(frame_fault f) noexcept;

/// A frame of type 1-5: its file offset (of the len field), type and payload.
struct frame_view {
  std::uint64_t offset = 0;
  frame_type type = frame_type::note;
  byte_span payload;
};

/// Where and why a scan stopped, and how far it got.
struct frame_stop {
  frame_fault fault = frame_fault::none;
  std::uint64_t offset = 0;  ///< offset of the failing frame (or of the end when clean)
  std::uint64_t end = 0;     ///< offset just after the last good frame
  std::uint64_t skipped = 0; ///< frames of type 128-255 skipped
  std::string message;       ///< "<reason> at offset N" when not clean

  [[nodiscard]] bool clean() const noexcept { return fault == frame_fault::none; }
};

/// Reads the frames of an in-memory segment body. It returns frames of types 1-5 in order, skips
/// 128-255 and stops at the first invalid frame; stop() reports the offset and the reason.
class frame_reader {
public:
  /// `data` holds file bytes starting at file offset `base`.
  explicit frame_reader(byte_span data, std::uint64_t base = 0) noexcept;

  /// The next frame, or nullopt at the end of the input or at the first invalid frame.
  [[nodiscard]] std::optional<frame_view> next();
  [[nodiscard]] const frame_stop &stop() const noexcept { return stop_; }

private:
  byte_span data_;
  std::uint64_t base_ = 0;
  std::size_t pos_ = 0;
  bool done_ = false;
  frame_stop stop_;
};

/// Streams the frames of an open segment file from `start` to its end through `visit` (types 1-5
/// only; 128-255 skipped). Returns where and why the scan stopped, or the first error of the
/// visitor or of the file.
using frame_visitor = std::function<result<void>(const frame_view &)>;
[[nodiscard]] result<frame_stop> scan_segment(const vfs_file &file, std::uint64_t start,
                                              const frame_visitor &visit);

// ---- commit and intent payloads ----

/// The commit frame payload (SL-ARCH-002 commit_fields), an SLB/1 struct of fields 1-10.
struct commit_record {
  std::uint64_t seq = 0;           ///< 1
  std::optional<digest> prev;      ///< 2, absent for seq 1
  digest tx_record;                ///< 3
  digest revision;                 ///< 4
  std::vector<ref_update> refs;    ///< 5 {1 name, 2 expected optional, 3 new}
  std::uint64_t object_count = 0;  ///< 6
  digest objects_digest;           ///< 7
  std::vector<digest> loose;       ///< 8
  std::int64_t commit_time_us = 0; ///< 9 (unhashed sidecar)
  record_id actor;                 ///< 10
};

[[nodiscard]] result<std::vector<std::uint8_t>> encode_commit(const commit_record &c);
/// `journal_corrupt` (naming the field) for anything but a struct of the fields above; unknown
/// fields of a newer minor are ignored.
[[nodiscard]] result<commit_record> decode_commit(byte_span payload);

/// intent_begin: struct {1 operation id, 2 kind string}; intent_end: struct {1 operation id,
/// 2 outcome uint (1 completed, 2 compensated, 3 abandoned)}.
[[nodiscard]] result<std::vector<std::uint8_t>> encode_intent(const intent_record &in);
/// `journal_corrupt` for a malformed payload; an end's kind is empty.
[[nodiscard]] result<intent_record> decode_intent(frame_type type, byte_span payload);

// ---- the chain ----

/// SHA-256 of a commit payload: the `prev` of the next commit.
[[nodiscard]] digest commit_digest(byte_span payload) noexcept;

/// SHA-256 over the 33-byte ids (algo byte, 32 hash bytes) of object frames in log order.
class objects_hasher {
public:
  void add(const digest &id) noexcept;
  [[nodiscard]] digest finish() const noexcept;
  [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
  void reset() noexcept;

private:
  sha256 hash_;
  std::uint64_t count_ = 0;
};

/// What the next commit must chain to: the last valid seq and its payload digest.
struct chain_state {
  std::uint64_t seq = 0;
  std::optional<digest> last;
};

/// ARCH2-R30 without the CRC (checked by the frame scan) and loose blobs (checked by the store):
/// `journal_corrupt` naming the rule for a seq that is not chain.seq + 1, a prev that is not
/// chain.last (or present at seq 1), an object_count or objects_digest that does not match the
/// object frames seen since the previous commit.
[[nodiscard]] result<void> check_commit(const commit_record &c, const chain_state &chain,
                                        const objects_hasher &objects) noexcept;

} // namespace sl::fabric
