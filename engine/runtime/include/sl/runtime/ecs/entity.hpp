#pragma once

// ECS entity lifecycle and id mapping (WO-083, SL-ARCH-003 ARCH3-R101 to R105, ADR-0025; episode
// 091). An entity is the 64-bit handle {index u32, generation u32}: generations start at 1, the
// all-zero handle is null, and a slot whose generation reaches 0xFFFFFFFF is retired forever. The
// handle layout is runtime ABI (an input of runtime_abi_digest, ARCH3-R032). entity_allocator keeps
// one generation per index in pages of 4096 records that never move; freed indices are reused in
// FIFO order of destruction only while at least 1024 are free, so handle assignment is a pure
// function of the operation history (ADR-0026). RFP10 is visited in create. entity_map<Key> maps an
// external key (a fabric record_id in the link) to one entity and back over two ordered maps; it
// iterates in ascending key order and holds no unordered container (R-RT-DIST).
// Layer L5: includes only lower runtime layers.
// Thread safety (`thread_safety`): entity_allocator create, destroy, restore and entity_map
// mutation are owner-thread at sync points; is_alive, check, assume_alive and entity_map lookups
// are any-thread during phases (read-only: nothing mutates while they run).

#include "sl/core/error.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/memory.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory_resource>
#include <utility>
#include <vector>

namespace sl::runtime::ecs {

// --- limits (SL-ARCH-003 `limits`) ---

/// Bits of the index and of the generation (`limits` entity_index_bits, entity_generation_bits).
inline constexpr std::uint32_t entity_index_bits = 32;
inline constexpr std::uint32_t entity_generation_bits = 32;
/// The generation of a slot that was never destroyed (`limits` entity_generation_first; 0 is null).
inline constexpr std::uint32_t entity_generation_first = 1;
/// The generation at which a slot is retired (`limits` entity_generation_retire).
inline constexpr std::uint32_t entity_generation_retire = 0xFFFFFFFFU;
/// Freed indices are reused only while at least this many are free (`limits`
/// entity_reuse_fifo_threshold).
inline constexpr std::uint32_t entity_reuse_fifo_threshold = 1024;
/// Records per page of the entity table (`limits` entity_page_records; pages never move).
inline constexpr std::uint32_t entity_page_records = 4096;
/// The default of runtime_config.max_entities (`limits` max_entities_default).
inline constexpr std::uint32_t max_entities_default = 4194304;

// --- the handle (ARCH3-R101) ---

/// A generational handle. Ordered by (index, generation), which is also the order of bits().
struct entity {
  std::uint32_t index = 0;
  std::uint32_t generation = 0;

  /// True for the all-zero handle.
  [[nodiscard]] constexpr bool is_null() const noexcept { return index == 0 && generation == 0; }
  /// The 64-bit form: the index in the high word, the generation in the low word.
  [[nodiscard]] constexpr std::uint64_t bits() const noexcept {
    return (std::uint64_t{index} << entity_generation_bits) | std::uint64_t{generation};
  }
  /// The inverse of bits().
  [[nodiscard]] static constexpr entity from_bits(std::uint64_t value) noexcept {
    return entity{static_cast<std::uint32_t>(value >> entity_generation_bits),
                  static_cast<std::uint32_t>(value & 0xFFFFFFFFU)};
  }

  friend constexpr auto operator<=>(entity, entity) = default;
};

static_assert(sizeof(entity) == 8, "the handle layout is runtime ABI (ADR-0025)");

/// The null handle; never alive.
inline constexpr entity null_entity{};

// --- the allocator (ARCH3-R101 to R104) ---

/// Hands out entity handles. Owner-thread for create, destroy and restore (at sync points);
/// is_alive, check and assume_alive are any-thread while nothing mutates. Neither copyable nor
/// movable: readers may hold the page addresses.
class entity_allocator {
public:
  /// The hashed and snapshotted state (`determinism` entity allocator): the generation of every
  /// index below next_index, the free indices oldest first, and the retired indices ascending.
  struct state {
    std::vector<std::uint32_t> generations;
    std::vector<std::uint32_t> free_fifo;
    std::uint32_t next_index = 0;
    std::vector<std::uint32_t> retired;

    friend bool operator==(const state &, const state &) = default;
  };

  /// At most `max_entities` indices (0 .. max_entities - 1); pages come from `memory` with its
  /// default tag. The page table (one pointer per possible page) is allocated here through the
  /// pmr path of `memory`.
  entity_allocator(std::uint32_t max_entities, mem::resource &memory);
  entity_allocator(const entity_allocator &) = delete;
  entity_allocator(entity_allocator &&) = delete;
  entity_allocator &operator=(const entity_allocator &) = delete;
  entity_allocator &operator=(entity_allocator &&) = delete;
  ~entity_allocator();

  /// A new handle: the oldest freed index while at least entity_reuse_fifo_threshold are free,
  /// else the next fresh index, else (max_entities reached) the oldest freed index, else
  /// entity_capacity. out_of_memory when a new page cannot be allocated. On error nothing
  /// changes. RFP10: param 1 fast-forwards the chosen slot's generation to 0xFFFFFFFE (its next
  /// destroy retires it); any other param forces entity_capacity.
  [[nodiscard]] result<entity> create() noexcept;
  /// Ends a live handle: its generation increases by one, and the index joins the free FIFO
  /// unless the generation reached entity_generation_retire (the slot is retired forever).
  /// stale_entity for a dead or null handle (nothing changes).
  [[nodiscard]] result<void> destroy(entity e) noexcept;
  /// True when `e` was returned by create and not destroyed since.
  [[nodiscard]] bool is_alive(entity e) const noexcept;
  /// The checked accessor: stale_entity unless is_alive(e).
  [[nodiscard]] result<void> check(entity e) const noexcept;
  /// The unchecked accessor: SL_DEBUG_ASSERT(is_alive(e)); nothing in Release.
  void assume_alive(entity e) const noexcept;

  /// Live handles.
  [[nodiscard]] std::uint32_t alive() const noexcept { return alive_; }
  /// Indices in the free FIFO.
  [[nodiscard]] std::uint32_t free_count() const noexcept { return free_count_; }
  /// Retired indices.
  [[nodiscard]] std::uint32_t retired_count() const noexcept { return retired_count_; }
  /// The next fresh index (every index below it was handed out at least once).
  [[nodiscard]] std::uint32_t next_index() const noexcept { return next_index_; }
  [[nodiscard]] std::uint32_t max_entities() const noexcept { return max_entities_; }
  /// Allocated pages; they are pages 0 .. page_count() - 1 and never move until destruction.
  [[nodiscard]] std::uint32_t page_count() const noexcept { return page_count_; }
  /// The generation array of page `page` (entity_page_records values), nullptr when the page is
  /// not allocated.
  [[nodiscard]] const std::uint32_t *page_address(std::uint32_t page) const noexcept;

  /// The state for hashing and snapshots.
  [[nodiscard]] state capture() const;
  /// Replaces the state with `s` (a capture of an allocator with the same or a larger
  /// max_entities): invalid_argument when `s` is inconsistent (sizes, an index out of range or
  /// listed twice, a generation of 0, a retired index whose generation is not 0xFFFFFFFF or the
  /// reverse), out_of_memory when a page cannot be allocated; nothing changes on error. Pages
  /// already allocated are reused in place.
  [[nodiscard]] result<void> restore(const state &s);

private:
  struct page;
  /// The end of the free FIFO (no index reaches it: indices are below max_entities).
  static constexpr std::uint32_t no_index = 0xFFFFFFFFU;

  [[nodiscard]] page *page_of(std::uint32_t index) const noexcept;
  [[nodiscard]] result<page *> allocate_page() noexcept;
  void release_page(page *p) noexcept;
  void push_free(std::uint32_t index) noexcept;
  [[nodiscard]] std::uint32_t pop_free() noexcept;

  mem::resource *memory_;
  std::uint32_t max_entities_;
  std::uint32_t next_index_ = 0;
  std::uint32_t alive_ = 0;
  std::uint32_t free_count_ = 0;
  std::uint32_t retired_count_ = 0;
  std::uint32_t free_head_ = no_index; // the oldest free index
  std::uint32_t free_tail_ = no_index; // the newest free index
  std::uint32_t page_count_ = 0;
  std::pmr::vector<page *> pages_; // sized once in the constructor; never grows
};

// --- the id mapping (ARCH3-R105) ---

namespace detail {
// The errors of entity_map, out of line so each instantiation does not carry the strings.
[[nodiscard]] std::unexpected<error> entity_map_null_entity();
[[nodiscard]] std::unexpected<error> entity_map_duplicate_key();
[[nodiscard]] std::unexpected<error> entity_map_duplicate_entity();
[[nodiscard]] std::unexpected<error> entity_map_key_not_found();
[[nodiscard]] std::unexpected<error> entity_map_entity_not_found();
} // namespace detail

/// A one-to-one map between an external key and an entity, over two std::pmr::map with
/// allocators from the given resource (node allocation takes the pmr path of mem::resource).
/// Key must be totally ordered by operator<. Mutation is owner-thread at sync points; find,
/// key_of, for_each and size are any-thread while nothing mutates.
template <class Key> class entity_map {
public:
  explicit entity_map(mem::resource &memory) : by_key_(&memory), by_entity_(&memory) {}
  entity_map(const entity_map &) = delete;
  entity_map(entity_map &&) = delete;
  entity_map &operator=(const entity_map &) = delete;
  entity_map &operator=(entity_map &&) = delete;
  ~entity_map() = default;

  /// Maps `key` to `e` and back: already_exists when the key or the entity is already mapped,
  /// invalid_argument for the null entity; nothing changes on error.
  [[nodiscard]] result<void> insert(const Key &key, entity e) {
    if (e.is_null()) {
      return detail::entity_map_null_entity();
    }
    if (by_key_.contains(key)) {
      return detail::entity_map_duplicate_key();
    }
    if (by_entity_.contains(e)) {
      return detail::entity_map_duplicate_entity();
    }
    by_key_.emplace(key, e);
    by_entity_.emplace(e, key);
    return {};
  }

  /// The entity of `key`; not_found when it is not mapped.
  [[nodiscard]] result<entity> find(const Key &key) const {
    const auto it = by_key_.find(key);
    if (it == by_key_.end()) {
      return detail::entity_map_key_not_found();
    }
    return it->second;
  }

  /// The key of `e`; not_found when it is not mapped.
  [[nodiscard]] result<Key> key_of(entity e) const {
    const auto it = by_entity_.find(e);
    if (it == by_entity_.end()) {
      return detail::entity_map_entity_not_found();
    }
    return it->second;
  }

  /// Removes `key` and its entity; not_found when it is not mapped.
  [[nodiscard]] result<void> erase(const Key &key) {
    const auto it = by_key_.find(key);
    if (it == by_key_.end()) {
      return detail::entity_map_key_not_found();
    }
    by_entity_.erase(it->second);
    by_key_.erase(it);
    return {};
  }

  /// Calls fn(const Key &, entity) for every pair in ascending key order.
  template <class F> void for_each(F fn) const {
    for (const auto &[key, e] : by_key_) {
      std::invoke(fn, key, e);
    }
  }

  [[nodiscard]] std::size_t size() const noexcept { return by_key_.size(); }

private:
  std::pmr::map<Key, entity> by_key_;
  std::pmr::map<entity, Key> by_entity_;
};

} // namespace sl::runtime::ecs
