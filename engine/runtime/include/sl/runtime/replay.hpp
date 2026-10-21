#pragma once

// The determinism contract and the replay seams (WO-084, SL-ARCH-003 ARCH3-R084 to R093, episode
// 089, ADR-0026). Volume 05 claims class D1 (same binary, same host ISA). Per-job buffers merge
// through merge_ordered over ordered_key {tick, phase, system_index, chunk_index, local_seq},
// compared lexicographically; state hashes go through state_hasher (SHA-256 of sl::fabric digest
// over tagged little-endian values, -0.0 written as +0.0 and every NaN canonical); randomness comes
// from rng_stream over std::mt19937_64 with engine-defined mappings (no std distribution); snapshot
// participation goes through snapshot_contributor; external inputs are external_input records.
// Volume 05 defines no replay format (ARCH3-R093). Layer L2 (includes time.hpp).
//
// Thread safety (SL-ARCH-003 `thread_safety`): rng_stream is worker-only, a value with one owner;
// state_hasher, merge_ordered and ordered_buffer are any-thread, a buffer has one writer job;
// snapshot_contributor capture and restore are owner-thread, at end_frame only.

#include "sl/core/assert.hpp"
#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/time.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sl::runtime::det {

/// throughput or deterministic (ARCH3-R084); classes beyond D1 are the Volume 17 seam V17-1.
enum class determinism_mode : std::uint8_t { throughput, deterministic };

/// The determinism class Volume 05 claims (ARCH3-R085).
enum class determinism_class : std::uint8_t { d1 };

/// The merge key of every per-job buffer (ARCH3-R086, `determinism` ordered_key), lexicographic.
struct ordered_key {
  std::uint64_t tick = 0;
  std::uint8_t phase = 0;
  std::uint16_t system_index = 0;
  std::uint32_t chunk_index = 0;
  std::uint32_t local_seq = 0;

  friend constexpr auto operator<=>(const ordered_key &, const ordered_key &) noexcept = default;
  friend constexpr bool operator==(const ordered_key &, const ordered_key &) noexcept = default;
};

/// The ordered_key layout version (an input of runtime_abi_digest, ARCH3-R032).
inline constexpr std::uint32_t ordered_key_layout_version = 1;

/// A per-job buffer: one writer pushes with strictly increasing keys (Debug assertion).
template <class T> class ordered_buffer {
public:
  struct entry {
    ordered_key key;
    T value{};
  };

  void push(const ordered_key &key, T value) {
    SL_DEBUG_ASSERT(entries_.empty() || entries_.back().key < key,
                    "ordered_buffer::push: keys must be strictly increasing");
    entries_.push_back(entry{.key = key, .value = std::move(value)});
  }

  void reserve(std::size_t n) { entries_.reserve(n); }
  void clear() noexcept { entries_.clear(); }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  [[nodiscard]] std::span<const entry> entries() const noexcept { return entries_; }
  [[nodiscard]] std::span<entry> entries() noexcept { return entries_; }

private:
  std::vector<entry> entries_;
};

namespace detail {
/// The index of the smallest head key, or heads.size() when every head is null; a key that
/// repeats across heads is a Debug assertion (det_merge.cpp).
[[nodiscard]] std::size_t min_head(std::span<const ordered_key *const> heads) noexcept;
} // namespace detail

/// The k-way merge by key (ARCH3-R086): calls out(const ordered_key &, T &) once per entry of
/// every buffer in ascending key order, whatever the buffer order; null buffers are skipped. A key
/// that repeats across buffers is a Debug assertion. The buffers are left unchanged unless `out`
/// moves from the values.
template <class T, class F>
void merge_ordered(std::span<ordered_buffer<T> *const> buffers, F &&out) {
  std::vector<std::size_t> next(buffers.size(), 0);
  std::vector<const ordered_key *> heads(buffers.size(), nullptr);
  const auto head = [&](std::size_t i) -> const ordered_key * {
    const ordered_buffer<T> *b = buffers[i];
    if (b == nullptr || next[i] >= b->size()) {
      return nullptr;
    }
    return &b->entries()[next[i]].key;
  };
  for (std::size_t i = 0; i < buffers.size(); ++i) {
    heads[i] = head(i);
  }
  for (;;) {
    const std::size_t i = detail::min_head(heads);
    if (i == heads.size()) {
      return;
    }
    auto &e = buffers[i]->entries()[next[i]];
    std::invoke(out, std::as_const(e.key), e.value);
    ++next[i];
    heads[i] = head(i);
  }
}

/// The canonical state hash (ARCH3-R088): SHA-256 over one tag byte per value then its
/// little-endian bytes. Tags: u8 0x01, u16 0x02, u32 0x04, u64 0x08, i8 0x11, i16 0x12, i32 0x14,
/// i64 0x18, f32 0x24, f64 0x28 (bit pattern after -0.0 -> +0.0 and NaN -> 0x7FC00000 /
/// 0x7FF8000000000000), bytes 0x30 and str 0x31 (u64 length, then the bytes), bool 0x40 (one byte
/// 0 or 1). Any thread; one hasher per thread.
class state_hasher {
public:
  void u8(std::uint8_t v) noexcept;
  void u16(std::uint16_t v) noexcept;
  void u32(std::uint32_t v) noexcept;
  void u64(std::uint64_t v) noexcept;
  void i8(std::int8_t v) noexcept;
  void i16(std::int16_t v) noexcept;
  void i32(std::int32_t v) noexcept;
  void i64(std::int64_t v) noexcept;
  void boolean(bool v) noexcept;
  void f32(float v) noexcept;
  void f64(double v) noexcept;
  void bytes(std::span<const std::byte> v) noexcept;
  void str(std::string_view v) noexcept;

  /// The digest of everything hashed so far; hashing may continue.
  [[nodiscard]] fabric::digest finish() const noexcept;

private:
  void tagged(std::uint8_t tag, std::uint64_t v, std::size_t width) noexcept;
  void length_prefixed(std::uint8_t tag, std::span<const std::byte> v) noexcept;

  fabric::sha256 sha_;
};

/// A seeded random stream (ARCH3-R089): std::mt19937_64 seeded with the first 8 bytes
/// (little-endian) of SHA-256("slr-rng/1" || seed u64le || stream u64le). Worker-only: a value with
/// one owner (one per system or chunk key), never shared.
class rng_stream {
public:
  rng_stream(std::uint64_t seed, std::uint64_t stream) noexcept;

  [[nodiscard]] std::uint64_t next_u64() noexcept { return engine_(); }
  /// Uniform in [0, n): draws until x >= (0 - n) % n, returns x % n. n must be above 0 (Debug
  /// assertion; 0 returns 0).
  [[nodiscard]] std::uint64_t next_below(std::uint64_t n) noexcept;
  /// Uniform in [0, 1): (x >> 11) x 2^-53.
  [[nodiscard]] double next_unit() noexcept;

  /// The operator<< text of the engine (classic locale).
  [[nodiscard]] std::string capture() const;
  /// Replaces the state with a capture; snapshot_corrupt (state unchanged) for malformed text.
  [[nodiscard]] result<void> restore(std::string_view text);

  friend bool operator==(const rng_stream &, const rng_stream &) noexcept = default;

private:
  std::mt19937_64 engine_;
};

/// Snapshot participation (ARCH3-R091). Owner-thread, end_frame only.
class snapshot_contributor {
public:
  virtual ~snapshot_contributor() = default;

  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
  [[nodiscard]] virtual std::uint32_t version() const noexcept = 0;
  /// Appends the contributor's bytes to `out`.
  [[nodiscard]] virtual result<void> capture(std::vector<std::byte> &out) const = 0;
  /// Restores from bytes of capture (same name and version); snapshot_corrupt for bytes that do
  /// not parse, with the state unchanged.
  [[nodiscard]] virtual result<void> restore(std::span<const std::byte> in) = 0;
  /// Hashes the contributor's hashed state.
  virtual void hash(state_hasher &h) const = 0;

protected:
  snapshot_contributor() = default;
  snapshot_contributor(const snapshot_contributor &) = default;
  snapshot_contributor(snapshot_contributor &&) = default;
  snapshot_contributor &operator=(const snapshot_contributor &) = default;
  snapshot_contributor &operator=(snapshot_contributor &&) = default;
};

/// One recorded external input (the record_sink seam, ARCH3-R030, V17-5).
struct external_input {
  std::uint64_t frame = 0;
  time::tick tick = 0;
  std::uint32_t kind = 0;
  std::vector<std::byte> bytes;
  ordered_key key;
};

/// runtime.clock, version 1 (time_contributor.cpp): snapshots the logical tick, the stepper and the
/// frame clock (with the frame index); hashes the tick only (`determinism`). The clocks must
/// outlive it.
class clock_contributor final : public snapshot_contributor {
public:
  clock_contributor(time::fixed_stepper &stepper, time::logical_clock &logical,
                    time::frame_clock &frame) noexcept;

  [[nodiscard]] std::string_view name() const noexcept override { return "runtime.clock"; }
  [[nodiscard]] std::uint32_t version() const noexcept override { return 1; }
  [[nodiscard]] result<void> capture(std::vector<std::byte> &out) const override;
  [[nodiscard]] result<void> restore(std::span<const std::byte> in) override;
  void hash(state_hasher &h) const override;

private:
  time::fixed_stepper *stepper_;
  time::logical_clock *logical_;
  time::frame_clock *frame_;
};

/// runtime.rng, version 1 (rng_contributor.cpp): snapshots and hashes the states of the runtime's
/// rng streams in registration order. The streams must outlive it.
class rng_contributor final : public snapshot_contributor {
public:
  explicit rng_contributor(std::span<rng_stream *const> streams);

  [[nodiscard]] std::string_view name() const noexcept override { return "runtime.rng"; }
  [[nodiscard]] std::uint32_t version() const noexcept override { return 1; }
  [[nodiscard]] result<void> capture(std::vector<std::byte> &out) const override;
  [[nodiscard]] result<void> restore(std::span<const std::byte> in) override;
  void hash(state_hasher &h) const override;

private:
  std::vector<rng_stream *> streams_;
};

} // namespace sl::runtime::det
