// SLRS/1 bytes (WO-095, SL-ARCH-003 ARCH3-R144, ADR-0026): the little-endian writer and the
// bounds-checked reader, the raw SHA-256 of the trailer, the type digest, the structure parse and
// snapshot::from_bytes. The trailer is checked first, so any changed bit is snapshot_corrupt; a
// major other than 1 under a valid trailer is snapshot_version; every count is bounded by the
// `limits` table or by the bytes left before anything is reserved.

#include "sl/runtime/snapshot.hpp"

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/replay.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace sl::runtime {

namespace detail {

namespace {

constexpr std::size_t digest_bytes = sl::fabric::sha256::digest_size;
constexpr std::array<std::byte, 4> magic{std::byte{'S'}, std::byte{'L'}, std::byte{'R'},
                                         std::byte{'S'}};

[[nodiscard]] std::unexpected<error> corrupt(std::string_view what) {
  return make_error(errc::snapshot_corrupt, std::string("snapshot: ") + std::string(what));
}

/// Reads a u32 count, then `count` u32 values (refused when they cannot fit).
[[nodiscard]] bool u32_array(snapshot_reader &r, std::vector<std::uint32_t> &out) {
  std::uint32_t count = 0;
  if (!r.u32(count) || std::size_t{count} > r.remaining() / 4) {
    return false;
  }
  out.resize(count);
  return std::ranges::all_of(out, [&r](std::uint32_t &v) noexcept { return r.u32(v); });
}

[[nodiscard]] bool parse_chunk(snapshot_reader &r, std::size_t columns,
                               parsed_snapshot::chunk_entry &c) {
  if (!r.u32(c.rows) || c.rows == 0 || c.rows > ecs::max_rows_per_chunk) {
    return false;
  }
  const std::size_t rows = c.rows;
  if (!r.raw(rows * 8, c.entities)) {
    return false;
  }
  c.values.resize(columns);
  c.added.resize(columns);
  c.changed.resize(columns);
  for (std::span<const std::byte> &v : c.values) {
    std::uint32_t length = 0;
    if (!r.u32(length) || !r.raw(length, v)) {
      return false;
    }
  }
  for (std::size_t k = 0; k < columns; ++k) {
    if (!r.raw(rows * 4, c.added[k]) || !r.raw(rows * 4, c.changed[k])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool parse_archetype(snapshot_reader &r, parsed_snapshot::archetype_entry &a) {
  std::uint32_t count = 0;
  if (!r.u32(count) || count > ecs::max_components_per_archetype) {
    return false;
  }
  a.components.resize(count);
  for (ecs::component_id &id : a.components) {
    if (!r.u16(id.value)) {
      return false;
    }
  }
  std::uint32_t chunks = 0;
  // a chunk takes at least 12 bytes (rows, one entity)
  if (!r.u32(chunks) || std::size_t{chunks} > r.remaining() / 12) {
    return false;
  }
  a.chunks.resize(chunks);
  return std::ranges::all_of(
      a.chunks, [&r, count](parsed_snapshot::chunk_entry &c) { return parse_chunk(r, count, c); });
}

[[nodiscard]] bool parse_world(snapshot_reader &r, parsed_snapshot &p) {
  if (!r.u32(p.entities.next_index) || !u32_array(r, p.entities.generations) ||
      !u32_array(r, p.entities.free_fifo) || !u32_array(r, p.entities.retired) ||
      !r.u32(p.change_tick)) {
    return false;
  }
  std::uint32_t archetypes = 0;
  if (!r.u32(archetypes) || archetypes > ecs::max_archetypes) {
    return false;
  }
  p.archetypes.resize(archetypes);
  if (!std::ranges::all_of(p.archetypes, [&r](parsed_snapshot::archetype_entry &a) {
        return parse_archetype(r, a);
      })) {
    return false;
  }
  std::uint32_t resources = 0;
  // a resource takes at least 10 bytes (key, component id, length)
  if (!r.u32(resources) || std::size_t{resources} > r.remaining() / 10) {
    return false;
  }
  p.resources.resize(resources);
  for (parsed_snapshot::resource_entry &e : p.resources) {
    std::uint32_t length = 0;
    if (!r.u32(e.key.value) || !r.u16(e.type.value) || !r.u32(length) || !r.raw(length, e.values)) {
      return false;
    }
  }
  return true;
}

} // namespace

// --- writer ---

void snapshot_writer::u8(std::uint8_t v) { out_.push_back(std::byte{v}); }

void snapshot_writer::u16(std::uint16_t v) {
  u8(static_cast<std::uint8_t>(v & 0xFFU));
  u8(static_cast<std::uint8_t>(v >> 8U));
}

void snapshot_writer::u32(std::uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) {
    u8(static_cast<std::uint8_t>((v >> (8U * i)) & 0xFFU));
  }
}

void snapshot_writer::u64(std::uint64_t v) {
  for (unsigned i = 0; i < 8; ++i) {
    u8(static_cast<std::uint8_t>((v >> (8U * i)) & 0xFFU));
  }
}

void snapshot_writer::raw(std::span<const std::byte> v) {
  out_.insert(out_.end(), v.begin(), v.end());
}

void snapshot_writer::digest(const sl::fabric::digest &d) {
  for (const std::uint8_t b : d.bytes) {
    u8(b);
  }
}

void snapshot_writer::name(std::string_view v) {
  u32(static_cast<std::uint32_t>(v.size()));
  for (const char c : v) {
    u8(static_cast<std::uint8_t>(c));
  }
}

// --- reader ---

bool snapshot_reader::u8(std::uint8_t &v) noexcept {
  if (remaining() < 1) {
    return false;
  }
  v = std::to_integer<std::uint8_t>(in_[at_]);
  ++at_;
  return true;
}

bool snapshot_reader::u16(std::uint16_t &v) noexcept {
  std::uint64_t x = 0;
  if (remaining() < 2) {
    return false;
  }
  for (unsigned i = 0; i < 2; ++i) {
    x |= std::uint64_t{std::to_integer<std::uint8_t>(in_[at_ + i])} << (8U * i);
  }
  at_ += 2;
  v = static_cast<std::uint16_t>(x);
  return true;
}

bool snapshot_reader::u32(std::uint32_t &v) noexcept {
  std::uint64_t x = 0;
  if (remaining() < 4) {
    return false;
  }
  for (unsigned i = 0; i < 4; ++i) {
    x |= std::uint64_t{std::to_integer<std::uint8_t>(in_[at_ + i])} << (8U * i);
  }
  at_ += 4;
  v = static_cast<std::uint32_t>(x);
  return true;
}

bool snapshot_reader::u64(std::uint64_t &v) noexcept {
  std::uint64_t x = 0;
  if (remaining() < 8) {
    return false;
  }
  for (unsigned i = 0; i < 8; ++i) {
    x |= std::uint64_t{std::to_integer<std::uint8_t>(in_[at_ + i])} << (8U * i);
  }
  at_ += 8;
  v = x;
  return true;
}

bool snapshot_reader::raw(std::size_t n, std::span<const std::byte> &v) noexcept {
  if (remaining() < n) {
    return false;
  }
  v = in_.subspan(at_, n);
  at_ += n;
  return true;
}

bool snapshot_reader::digest(sl::fabric::digest &d) noexcept {
  std::span<const std::byte> v;
  if (!raw(digest_bytes, v)) {
    return false;
  }
  d.algo = sl::fabric::hash_algo::sha256;
  std::ranges::transform(v, d.bytes.begin(),
                         [](std::byte b) noexcept { return std::to_integer<std::uint8_t>(b); });
  return true;
}

bool snapshot_reader::name(std::string_view &v) noexcept {
  std::uint32_t length = 0;
  std::span<const std::byte> bytes;
  if (!u32(length) || !raw(length, bytes)) {
    return false;
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the name's bytes viewed as chars
  v = std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  return true;
}

// --- digests ---

sl::fabric::digest sha256_of(std::span<const std::byte> bytes) noexcept {
  sl::fabric::sha256 sha;
  std::array<std::uint8_t, sl::fabric::sha256::block_size> storage{};
  const std::span<std::uint8_t> chunk(storage);
  while (!bytes.empty()) {
    const std::size_t n = std::min(bytes.size(), chunk.size());
    std::ranges::transform(bytes.first(n), chunk.begin(),
                           [](std::byte b) noexcept { return std::to_integer<std::uint8_t>(b); });
    sha.update(chunk.first(n));
    bytes = bytes.subspan(n);
  }
  sl::fabric::digest d;
  d.algo = sl::fabric::hash_algo::sha256;
  d.bytes = sha.finish();
  return d;
}

sl::fabric::digest type_digest(const ecs::component_info &info) noexcept {
  det::state_hasher h;
  h.str("sl-rt-type/1");
  h.str(info.name);
  h.u32(info.size);
  h.u32(info.align);
  h.boolean(info.trivially_relocatable);
  h.boolean(info.reflect != nullptr);
  if (info.reflect != nullptr) {
    h.str(info.reflect->name);
    h.u32(static_cast<std::uint32_t>(info.reflect->fields.size()));
    for (const field_info &f : info.reflect->fields) {
      h.str(f.name);
      h.u32(f.offset);
      h.u32(f.size);
      h.u8(static_cast<std::uint8_t>(f.kind));
      h.u8(static_cast<std::uint8_t>(f.flags));
    }
  }
  return h.finish();
}

// --- parse ---

result<parsed_snapshot> parse_snapshot(std::span<const std::byte> bytes) {
  if (bytes.size() < magic.size() + 2 + digest_bytes) {
    return corrupt("too short");
  }
  const std::span<const std::byte> body = bytes.first(bytes.size() - digest_bytes);
  const sl::fabric::digest trailer = sha256_of(body);
  const std::span<const std::byte> stored = bytes.last(digest_bytes);
  if (!std::ranges::equal(trailer.bytes, stored, [](std::uint8_t a, std::byte b) noexcept {
        return a == std::to_integer<std::uint8_t>(b);
      })) {
    return corrupt("the SHA-256 trailer does not match");
  }
  snapshot_reader r(body);
  std::span<const std::byte> head;
  if (!r.raw(magic.size(), head) || !std::ranges::equal(head, magic)) {
    return corrupt("no SLRS magic");
  }
  parsed_snapshot p;
  if (!r.u8(p.major) || !r.u8(p.minor)) {
    return corrupt("no version");
  }
  if (p.major != snapshot_major) {
    return make_error(errc::snapshot_version,
                      "snapshot: SLRS major " + std::to_string(p.major) + " is not 1");
  }
  std::uint32_t types = 0;
  if (!r.u64(p.frame) || !r.u64(p.tick) || !r.digest(p.abi) || !r.digest(p.schedule) ||
      !r.u32(types) || types > ecs::max_component_types) {
    return corrupt("malformed header");
  }
  p.types.resize(types);
  for (parsed_snapshot::type_entry &t : p.types) {
    if (!r.name(t.name) || !r.digest(t.digest)) {
      return corrupt("malformed type section");
    }
  }
  std::uint32_t contributors = 0;
  // a contributor takes at least 16 bytes (name length, version, length)
  if (!r.u32(contributors) || std::size_t{contributors} > r.remaining() / 16) {
    return corrupt("malformed contributor section");
  }
  p.contributors.resize(contributors);
  for (parsed_snapshot::contributor_entry &c : p.contributors) {
    std::uint64_t length = 0;
    if (!r.name(c.name) || !r.u32(c.version) || !r.u64(length) || length > r.remaining() ||
        !r.raw(static_cast<std::size_t>(length), c.bytes)) {
      return corrupt("malformed contributor section");
    }
  }
  if (!parse_world(r, p)) {
    return corrupt("malformed world image");
  }
  if (!r.digest(p.state_hash) || r.remaining() != 0) {
    return corrupt("malformed state hash or trailing bytes");
  }
  return p;
}

} // namespace detail

result<snapshot> snapshot::from_bytes(std::vector<std::byte> bytes) {
  auto parsed = detail::parse_snapshot(bytes);
  if (!parsed) {
    return std::unexpected(parsed.error());
  }
  snapshot s;
  s.frame_ = parsed->frame;
  s.tick_ = parsed->tick;
  s.state_hash_ = parsed->state_hash;
  s.abi_ = parsed->abi;
  s.schedule_ = parsed->schedule;
  s.bytes_ = std::move(bytes);
  return s;
}

} // namespace sl::runtime
