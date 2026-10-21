#pragma once

// Runtime snapshots and the state hash (WO-095, SL-ARCH-003 ARCH3-R143 to R147, episode 098,
// ADR-0026). A snapshot is an immutable SLRS/1 byte string taken at end_frame: the entity
// allocator, the archetypes in creation order with their row order and change ticks, the
// snapshot-flagged fields of every component value (reflect::encode), the resources whose type is
// a registered component, the contributors (sorted by name), the schedule digest and the
// runtime_abi_digest; never the job graph. SLRS/1, little-endian throughout:
//
//   "SLRS" | major u8 1 | minor u8 0 | frame u64 | tick u64 | runtime_abi_digest [32] |
//   schedule digest [32] | type count u32, per component in id order (name, type digest [32]) |
//   contributor count u32, per contributor by name (name, version u32, length u64, bytes) |
//   allocator (next_index u32, generations, free FIFO, retired: each a u32 count then u32s) |
//   change tick u32 | archetype count u32, per archetype in creation order (component count u32,
//   ids u16, chunk count u32, per chunk (rows u32, rows x (index u32, generation u32), per column
//   (length u32, the encoded snapshot fields of each row), per column (rows x added u32,
//   rows x changed u32))) | resource count u32, per resource by key (key u32, component id u16,
//   length u32, the encoded snapshot fields) | state hash [32] | SHA-256 of all previous bytes.
//
// A name is a u32 length then its bytes. A type digest is SHA-256 (det::state_hasher) of the
// component's name, size, alignment, relocatability and reflected layout (type name, then per
// field its name, offset, size, kind and flags). Restoring builds each value from zero bytes and
// decodes the snapshot fields into it, so every component or resource with values in a snapshot
// must be trivially copyable; fields without the snapshot flag come back zero. state_hash is
// det::state_hasher over the parts of hash_breakdown (the hashed fields only) and the contributors.
// Layer L8 (includes ecs/world.hpp, reflect.hpp, replay.hpp and platform.hpp).
//
// Thread safety (`thread_safety`): take, restore, save, load, state_hash and breakdown are
// owner-thread, at end_frame (snapshot_mid_tick inside a phase); a snapshot value is immutable
// and may be read from any thread.

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/platform.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/replay.hpp"
#include "sl/runtime/time.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace sl::runtime {

/// The SLRS/1 version this build writes and reads (ARCH3-R144).
inline constexpr std::uint8_t snapshot_major = 1;
inline constexpr std::uint8_t snapshot_minor = 0;

/// What take records besides the world and the contributors.
struct snapshot_meta {
  std::uint64_t frame = 0;
  time::tick tick = 0;
  sl::fabric::digest schedule_digest;
  sl::fabric::digest runtime_abi_digest;
};

/// One SLRS/1 snapshot: an immutable value whose bytes passed the trailer and structure checks.
class snapshot {
public:
  /// Adopts `bytes`: snapshot_corrupt for a trailer, magic or structure mismatch (every bit flip
  /// fails the trailer), snapshot_version for a major other than 1 under a valid trailer.
  [[nodiscard]] static result<snapshot> from_bytes(std::vector<std::byte> bytes);

  [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::uint64_t frame() const noexcept { return frame_; }
  [[nodiscard]] time::tick tick() const noexcept { return tick_; }
  [[nodiscard]] const sl::fabric::digest &state_hash() const noexcept { return state_hash_; }
  [[nodiscard]] const sl::fabric::digest &runtime_abi_digest() const noexcept { return abi_; }
  [[nodiscard]] const sl::fabric::digest &schedule_digest() const noexcept { return schedule_; }

private:
  snapshot() = default;

  std::vector<std::byte> bytes_;
  std::uint64_t frame_ = 0;
  time::tick tick_ = 0;
  sl::fabric::digest state_hash_;
  sl::fabric::digest abi_;
  sl::fabric::digest schedule_;
};

/// The state hash split into parts (ARCH3-R146). Parts, in order: {no_archetype, no_component}
/// the entity allocator; per archetype i in creation order {i, no_component} its signature and
/// entity column in row order, then {i, c} per component c (ascending id) the hashed fields of
/// its column in row order; per hashed resource by key {no_archetype, its component} the key and
/// its hashed fields. `contributors` hashes every contributor (name, version, hash) by name.
struct hash_breakdown {
  static constexpr std::uint32_t no_archetype = 0xFFFF'FFFFU;
  static constexpr ecs::component_id no_component{0xFFFF};

  struct part {
    std::uint32_t archetype = no_archetype;
    ecs::component_id component = no_component;
    sl::fabric::digest digest;

    friend bool operator==(const part &, const part &) noexcept = default;
  };

  std::vector<part> parts;
  sl::fabric::digest contributors;
};

/// Takes a snapshot of `w` (owner thread, end_frame): snapshot_mid_tick inside a phase;
/// invalid_argument when `registry` is not the world's, `types` is not frozen, a component's
/// reflected type is not in `types`, a contributor is null or its name repeats, or a component
/// or resource with values in the snapshot is not trivially copyable; a contributor's capture
/// error as returned.
[[nodiscard]] result<snapshot> take(const ecs::world &w, const ecs::component_registry &registry,
                                    const type_registry &types,
                                    std::span<const det::snapshot_contributor *const> contributors,
                                    const snapshot_meta &meta);

/// Restores `s` into `w` (owner thread, end_frame). Every check comes before any change:
/// snapshot_mid_tick inside a phase; invalid_argument as take; snapshot_corrupt or
/// snapshot_version as snapshot::from_bytes, and snapshot_corrupt for values that do not decode
/// or an image the world refuses; snapshot_abi_mismatch when the recorded runtime_abi_digest is
/// not `expected_abi`; snapshot_schema_mismatch when the type count, a name or a type digest
/// differs from `registry`; snapshot_contributor_unknown when a recorded (name, version) is not
/// among `contributors`. Then the contributors restore (rolled back from their own captures when
/// one fails) and the world restores (the contributors rolled back when it fails). Resources
/// whose type is not a registered component stay as they are; a contributor the snapshot does
/// not record is left unchanged.
[[nodiscard]] result<void> restore(ecs::world &w, const ecs::component_registry &registry,
                                   const type_registry &types,
                                   std::span<det::snapshot_contributor *const> contributors,
                                   const snapshot &s, const sl::fabric::digest &expected_abi);

/// The state hash (ARCH3-R146): the errors of take before the snapshot bytes.
[[nodiscard]] result<sl::fabric::digest>
state_hash(const ecs::world &w, const ecs::component_registry &registry, const type_registry &types,
           std::span<const det::snapshot_contributor *const> contributors);

/// The parts behind state_hash.
[[nodiscard]] result<hash_breakdown>
breakdown(const ecs::world &w, const ecs::component_registry &registry, const type_registry &types,
          std::span<const det::snapshot_contributor *const> contributors);

/// The first part of `a` that differs from `b` at the same position (the part of the longer one
/// past the end of the shorter); nullopt when every part agrees (compare `contributors` apart).
[[nodiscard]] std::optional<hash_breakdown::part> first_difference(const hash_breakdown &a,
                                                                   const hash_breakdown &b);

/// Writes the snapshot bytes to `path` through file_access::atomic_write (ARCH3-R147). RFP8 is
/// visited once before the write: when it fires the save stops there (a simulated crash before
/// the first primitive) and returns io_error; the crash after each storage primitive is the
/// faulty vfs's. The file_access errors otherwise.
[[nodiscard]] result<void> save(const file_access &files, std::string_view path, const snapshot &s);

/// Reads and checks a snapshot: the file_access errors, then as snapshot::from_bytes.
[[nodiscard]] result<snapshot> load(const file_access &files, std::string_view path);

namespace detail {

/// Little-endian SLRS/1 writing.
class snapshot_writer {
public:
  void u8(std::uint8_t v);
  void u16(std::uint16_t v);
  void u32(std::uint32_t v);
  void u64(std::uint64_t v);
  void raw(std::span<const std::byte> v);
  void digest(const sl::fabric::digest &d);
  /// A u32 length, then the bytes.
  void name(std::string_view v);
  [[nodiscard]] std::vector<std::byte> &bytes() noexcept { return out_; }

private:
  std::vector<std::byte> out_;
};

/// Bounds-checked little-endian SLRS/1 reading; every read fails (false) past the end.
class snapshot_reader {
public:
  explicit snapshot_reader(std::span<const std::byte> in) noexcept : in_(in) {}

  [[nodiscard]] bool u8(std::uint8_t &v) noexcept;
  [[nodiscard]] bool u16(std::uint16_t &v) noexcept;
  [[nodiscard]] bool u32(std::uint32_t &v) noexcept;
  [[nodiscard]] bool u64(std::uint64_t &v) noexcept;
  [[nodiscard]] bool raw(std::size_t n, std::span<const std::byte> &v) noexcept;
  [[nodiscard]] bool digest(sl::fabric::digest &d) noexcept;
  [[nodiscard]] bool name(std::string_view &v) noexcept;
  [[nodiscard]] std::size_t remaining() const noexcept { return in_.size() - at_; }

private:
  std::span<const std::byte> in_;
  std::size_t at_ = 0;
};

/// The decoded structure of SLRS/1 bytes: views into them.
struct parsed_snapshot {
  struct type_entry {
    std::string_view name;
    sl::fabric::digest digest;
  };
  struct contributor_entry {
    std::string_view name;
    std::uint32_t version = 0;
    std::span<const std::byte> bytes;
  };
  struct chunk_entry {
    std::uint32_t rows = 0;
    std::span<const std::byte> entities;             // rows x 8 bytes
    std::vector<std::span<const std::byte>> values;  // per column
    std::vector<std::span<const std::byte>> added;   // per column, rows x 4 bytes
    std::vector<std::span<const std::byte>> changed; // per column, rows x 4 bytes
  };
  struct archetype_entry {
    std::vector<ecs::component_id> components;
    std::vector<chunk_entry> chunks;
  };
  struct resource_entry {
    ecs::resource_key key;
    ecs::component_id type;
    std::span<const std::byte> values;
  };

  std::uint8_t major = 0;
  std::uint8_t minor = 0;
  std::uint64_t frame = 0;
  time::tick tick = 0;
  sl::fabric::digest abi;
  sl::fabric::digest schedule;
  std::vector<type_entry> types;
  std::vector<contributor_entry> contributors;
  ecs::entity_allocator::state entities;
  std::uint32_t change_tick = 0;
  std::vector<archetype_entry> archetypes;
  std::vector<resource_entry> resources;
  sl::fabric::digest state_hash;
};

/// Checks the trailer, the magic, the major and the whole structure of `bytes`.
[[nodiscard]] result<parsed_snapshot> parse_snapshot(std::span<const std::byte> bytes);

/// SHA-256 of raw bytes.
[[nodiscard]] sl::fabric::digest sha256_of(std::span<const std::byte> bytes) noexcept;

/// The type digest of one component (the SLRS/1 type section).
[[nodiscard]] sl::fabric::digest type_digest(const ecs::component_info &info) noexcept;

/// The shared input checks of take, restore, state_hash and breakdown.
[[nodiscard]] result<void> check_inputs(const ecs::world &w,
                                        const ecs::component_registry &registry,
                                        const type_registry &types) noexcept;

/// The contributors sorted by name: invalid_argument for a null one or a repeated name.
template <class C>
[[nodiscard]] result<std::vector<C *>> sorted_contributors(std::span<C *const> contributors);

/// The breakdown of an image of a world over `registry`, the contributors already sorted.
[[nodiscard]] hash_breakdown breakdown_of(const ecs::world_image &image,
                                          const ecs::component_registry &registry,
                                          std::span<const det::snapshot_contributor *const> sorted);

/// The state hash of a breakdown.
[[nodiscard]] sl::fabric::digest hash_of(const hash_breakdown &b) noexcept;

/// The component registered for a resource's type, or nullopt.
[[nodiscard]] std::optional<ecs::component_id>
resource_component(const ecs::component_registry &registry,
                   const ecs::detail::resource_slot &slot) noexcept;

} // namespace detail

} // namespace sl::runtime
