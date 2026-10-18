#pragma once

// Content identity (WO-037, SL-ARCH-001 ARCH1-R09 and ARCH1-R10, ADR-0011).
//
// An object id is {algo, 32 bytes}; algo 1 is SHA-256 over
//   "slf-obj/1" 0x00 || major || minor || kind || flags || uvarint(payload length) || payload
// so every byte of the stored frame header except the magic `SLFO` is covered. SHA-256 is
// implemented in house (FIPS 180-4; src/sha256.cpp), incremental and allocation-free.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "sl/fabric/bytes.hpp"

namespace sl::fabric {

/// Incremental SHA-256 (FIPS 180-4).
class sha256 {
public:
  static constexpr std::size_t digest_size = 32;
  static constexpr std::size_t block_size = 64;
  using value_type = std::array<std::uint8_t, digest_size>;

  sha256() noexcept { reset(); }

  void reset() noexcept;
  void update(byte_span data) noexcept;
  void update(std::string_view text) noexcept;
  /// The digest of everything absorbed so far; the state is left unchanged, so absorbing may
  /// continue and a copied state hashes a shared prefix once.
  [[nodiscard]] value_type finish() const noexcept;

  /// One-shot digest.
  [[nodiscard]] static value_type hash(byte_span data) noexcept;
  [[nodiscard]] static value_type hash(std::string_view text) noexcept;

private:
  void compress(const std::uint8_t *block) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, block_size> buffer_{};
  std::size_t buffered_ = 0;
  std::uint64_t total_ = 0; // bytes absorbed
};

/// Hash algorithms of object ids (SL-ARCH-001 hash_algos).
enum class hash_algo : std::uint8_t {
  sha256 = 1,
  blake3 = 2, // reserved: needs its own ADR (ADR-0011)
};

/// Kinds of stored objects (SL-ARCH-001 object_kinds).
enum class object_kind : std::uint8_t {
  blob = 1,
  record = 2,
  tombstone = 3,
  tree = 4,
  root = 5,
  revision = 6,
  tx = 7,
  patch = 8,
  schema = 9,
  schema_set = 10,
};

[[nodiscard]] constexpr std::string_view to_string(object_kind k) noexcept {
  switch (k) {
  case object_kind::blob:
    return "blob";
  case object_kind::record:
    return "record";
  case object_kind::tombstone:
    return "tombstone";
  case object_kind::tree:
    return "tree";
  case object_kind::root:
    return "root";
  case object_kind::revision:
    return "revision";
  case object_kind::tx:
    return "tx";
  case object_kind::patch:
    return "patch";
  case object_kind::schema:
    return "schema";
  case object_kind::schema_set:
    return "schema_set";
  }
  return "unknown";
}

/// The stored-object frame (SL-ARCH-001 object_frame): magic, major, minor, kind, flags, then the
/// LEB128 payload length and the payload.
inline constexpr std::array<std::uint8_t, 4> frame_magic = {'S', 'L', 'F', 'O'};
inline constexpr std::uint8_t frame_major = 1;
inline constexpr std::uint8_t frame_minor = 0;

/// The header fields an object id covers; the payload length is taken from the payload itself.
struct frame_header {
  std::uint8_t major = frame_major;
  std::uint8_t minor = frame_minor;
  object_kind kind = object_kind::blob;
  std::uint8_t flags = 0; // bit 0 compressed (reserved, 0 in 1.0)

  friend constexpr bool operator==(const frame_header &, const frame_header &) noexcept = default;
};

/// A content digest: the object id of a stored object.
struct digest {
  hash_algo algo = hash_algo::sha256;
  std::array<std::uint8_t, sha256::digest_size> bytes{};

  /// 64 lowercase hex digits of `bytes`.
  [[nodiscard]] std::string to_hex() const;

  friend constexpr auto operator<=>(const digest &, const digest &) noexcept = default;
};

/// The object id of a frame (ARCH1-R09).
[[nodiscard]] digest hash_object(const frame_header &header, byte_span payload) noexcept;

} // namespace sl::fabric
