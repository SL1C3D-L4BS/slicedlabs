#pragma once

// Archetype component storage (WO-088, SL-ARCH-003 ARCH3-R106 to R113, ADR-0025; episode 092). An
// archetype is the sorted set of at most 32 component ids, created on demand in playback order,
// kept in creation order and never destroyed. Its rows live in 16384-byte chunks aligned to 64
// from the world's chunk pool (2 MiB slabs aligned to 4096, tag ecs.chunks): the entity column at
// offset 0, then the component columns ordered by (alignment descending, id ascending), then the
// u32 added and changed tick columns of each component in the same order, every column at a
// 64-byte aligned offset; the capacity is the largest n <= 1024 whose layout fits. Rows are dense:
// every chunk but the last non-empty one is full, removal is swap-remove, and chunks left empty
// return to the pool at release_empty_chunks (a boundary). A structural change reserves every
// byte it needs with try_allocate (a staged archetype, the transition cache slot, the destination
// chunk, the location page, the entity page) before it moves a row, so out_of_memory leaves the
// world unchanged (ARCH3-R110). Archetype transitions (add or remove of one component) are cached
// per archetype. Layer L5: includes lower layers and ecs/entity.hpp, ecs/component.hpp.
// Thread safety (`thread_safety`): structural calls (create, destroy, add, remove, set),
// resources, restore_from and release_empty_chunks are owner-thread (wrong_thread otherwise) and
// outside phases (structural_change_forbidden); get, column access and archetype reads are
// worker-only inside systems with the declared access, owner-thread outside phases; a background
// job touching the world trips SL_DEBUG_ASSERT once a job system is attached.

#include "sl/core/assert.hpp"
#include "sl/core/error.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/memory.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace sl::runtime::jobs {
class job_system;
} // namespace sl::runtime::jobs

namespace sl::runtime::ecs {

// --- limits (SL-ARCH-003 `limits`) ---

/// The bytes of one chunk (`limits` chunk_bytes; runtime ABI).
inline constexpr std::uint32_t chunk_bytes = 16384;
/// The alignment of a chunk and of every column offset (`limits` chunk_alignment).
inline constexpr std::uint32_t chunk_alignment = 64;
/// The most rows of one chunk (`limits` max_rows_per_chunk).
inline constexpr std::uint32_t max_rows_per_chunk = 1024;
/// The most components of one archetype (`limits` max_components_per_archetype).
inline constexpr std::uint32_t max_components_per_archetype = 32;
/// The most archetypes of one world (`limits` max_archetypes).
inline constexpr std::uint32_t max_archetypes = 4096;
/// The bytes and alignment of one chunk pool slab (`limits` chunk_pool_slab_bytes).
inline constexpr std::size_t chunk_pool_slab_bytes = 2097152;
inline constexpr std::size_t chunk_pool_slab_alignment = 4096;
/// Chunks carved from one slab.
inline constexpr std::uint32_t chunks_per_slab =
    static_cast<std::uint32_t>(chunk_pool_slab_bytes / chunk_bytes);

/// The archetype index of no archetype.
inline constexpr std::uint32_t no_archetype = 0xFFFFFFFFU;

struct world_config {
  std::uint32_t max_entities = max_entities_default;
  std::uint32_t max_archetypes = ecs::max_archetypes;
};

/// The key of a world resource.
struct resource_key {
  std::uint32_t value = 0;

  friend constexpr auto operator<=>(resource_key, resource_key) noexcept = default;
};

/// Who writes a component through set: the runtime (stamps the changed tick) or a document
/// rebase (stamps no changed tick, ARCH3-R132).
enum class write_source : std::uint8_t { runtime, document };

namespace detail {

/// A growable array of trivially copyable values whose memory comes from a mem::resource through
/// try_allocate: reserve is the only fallible call, push_back and insert_at need reserved room.
/// Nobody keeps a pointer into it across a reserve.
template <class T> class buffer {
  static_assert(std::is_trivially_copyable_v<T>, "detail::buffer holds trivially copyable values");

public:
  buffer() noexcept = default;
  buffer(mem::resource &memory, mem::memory_tag tag) noexcept : memory_(&memory), tag_(tag) {}
  buffer(const buffer &) = delete;
  buffer &operator=(const buffer &) = delete;
  buffer(buffer &&other) noexcept
      : memory_(other.memory_), tag_(other.tag_), data_(std::exchange(other.data_, nullptr)),
        size_(std::exchange(other.size_, 0)), capacity_(std::exchange(other.capacity_, 0)) {}
  buffer &operator=(buffer &&other) noexcept {
    if (this != &other) {
      reset();
      memory_ = other.memory_;
      tag_ = other.tag_;
      data_ = std::exchange(other.data_, nullptr);
      size_ = std::exchange(other.size_, 0);
      capacity_ = std::exchange(other.capacity_, 0);
    }
    return *this;
  }
  ~buffer() { reset(); }

  /// Room for at least n values: out_of_memory (or the resource's error) changes nothing.
  [[nodiscard]] result<void> reserve(std::size_t n) noexcept {
    if (n <= capacity_) {
      return {};
    }
    const std::size_t cap = std::max({n, capacity_ * 2, std::size_t{4}});
    auto block = memory_->try_allocate(cap * sizeof(T), alignof(T), tag_);
    if (!block) {
      return std::unexpected(block.error());
    }
    T *fresh = static_cast<T *>(*block);
    if (size_ != 0) {
      std::memcpy(fresh, data_, size_ * sizeof(T));
    }
    release_block();
    data_ = fresh;
    capacity_ = cap;
    return {};
  }
  void push_back(const T &v) noexcept {
    SL_ASSERT(size_ < capacity_, "detail::buffer::push_back without reserved room");
    std::memcpy(data_ + size_, &v, sizeof(T));
    ++size_;
  }
  void insert_at(std::size_t at, const T &v) noexcept {
    SL_ASSERT(size_ < capacity_ && at <= size_, "detail::buffer::insert_at out of range");
    if (at < size_) {
      std::memmove(data_ + at + 1, data_ + at, (size_ - at) * sizeof(T));
    }
    std::memcpy(data_ + at, &v, sizeof(T));
    ++size_;
  }
  void pop_back() noexcept {
    SL_ASSERT(size_ != 0, "detail::buffer::pop_back on an empty buffer");
    --size_;
  }
  void clear() noexcept { size_ = 0; }
  /// Releases the memory.
  void reset() noexcept {
    release_block();
    data_ = nullptr;
    size_ = 0;
    capacity_ = 0;
  }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] T *data() const noexcept { return data_; }
  [[nodiscard]] T &operator[](std::size_t i) const noexcept { return data_[i]; }
  [[nodiscard]] std::span<T> span() const noexcept { return {data_, size_}; }

private:
  void release_block() noexcept {
    if (data_ != nullptr) {
      memory_->release(data_, capacity_ * sizeof(T), alignof(T));
    }
  }

  mem::resource *memory_ = nullptr;
  mem::memory_tag tag_;
  T *data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t capacity_ = 0;
};

/// One world resource: a value of size bytes at data, moved and destroyed through the functions.
struct resource_slot {
  resource_key key;
  const void *type_key = nullptr;
  void *data = nullptr;
  std::uint32_t size = 0;
  std::uint32_t align = 0;
  void (*move)(void *dst, void *src) noexcept = nullptr;
  void (*destroy)(void *p) noexcept = nullptr;
};

} // namespace detail

/// An archetype: its sorted components, its chunks and their columns. Owned by its world; read
/// through archetypes(). Column pointers and tick spans stay valid until the next structural
/// change, release_empty_chunks or restore_from.
class archetype {
public:
  /// The place of one component in a chunk: offsets are bytes from the chunk start.
  struct column_layout {
    component_id id;
    std::uint32_t size = 0;
    std::uint32_t align = 0;
    std::uint32_t offset = 0;
    std::uint32_t added_offset = 0;
    std::uint32_t changed_offset = 0;
  };

  archetype(const archetype &) = delete;
  archetype(archetype &&) = delete;
  archetype &operator=(const archetype &) = delete;
  archetype &operator=(archetype &&) = delete;
  ~archetype() = default;

  /// The component ids, ascending.
  [[nodiscard]] std::span<const component_id> components() const noexcept;
  /// The column layouts, in components() order.
  [[nodiscard]] std::span<const column_layout> layout() const noexcept;
  [[nodiscard]] std::uint32_t chunk_count() const noexcept;
  /// The rows of chunk `chunk` (0 for an empty or absent chunk).
  [[nodiscard]] std::uint32_t rows(std::uint32_t chunk) const noexcept;
  /// The rows of every chunk.
  [[nodiscard]] std::uint32_t row_count() const noexcept { return rows_; }
  /// Rows per chunk (the largest n <= 1024 whose layout fits).
  [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }
  /// The entity column of `chunk` (its first rows(chunk) values).
  [[nodiscard]] std::span<const entity> entities(std::uint32_t chunk) const noexcept;
  /// The column of `id` in `chunk`; nullptr when the archetype lacks `id` or the chunk is absent.
  [[nodiscard]] void *column(component_id id, std::uint32_t chunk) const noexcept;
  /// The added and changed ticks of `id` in `chunk` (rows(chunk) values; empty when absent).
  [[nodiscard]] std::span<std::uint32_t> added_ticks(component_id id,
                                                     std::uint32_t chunk) const noexcept;
  [[nodiscard]] std::span<std::uint32_t> changed_ticks(component_id id,
                                                       std::uint32_t chunk) const noexcept;
  /// The position in the world's creation order.
  [[nodiscard]] std::uint32_t creation_index() const noexcept { return creation_index_; }
  /// True when `id` is one of the components.
  [[nodiscard]] bool contains(component_id id) const noexcept;
  /// The position of `id` in components(), or max_components_per_archetype when absent.
  [[nodiscard]] std::uint32_t position_of(component_id id) const noexcept;

private:
  friend class world;
  struct transition {
    component_id id;
    std::uint32_t target = no_archetype;
  };

  archetype(mem::resource &memory, mem::memory_tag tag) noexcept;
  /// Sets the components (ascending, at most 32, registered) and computes the layout; false when
  /// not even one row fits a chunk.
  [[nodiscard]] bool compute_layout(std::span<const component_id> sorted,
                                    const component_registry &registry) noexcept;
  [[nodiscard]] std::byte *chunk_at(std::uint32_t chunk) const noexcept;
  /// The value of the component at `position` in global row `row`.
  [[nodiscard]] std::byte *value_at(std::uint32_t position, std::uint32_t row) const noexcept;
  [[nodiscard]] std::uint32_t *added_at(std::uint32_t position, std::uint32_t row) const noexcept;
  [[nodiscard]] std::uint32_t *changed_at(std::uint32_t position, std::uint32_t row) const noexcept;
  [[nodiscard]] std::byte *entity_at(std::uint32_t row) const noexcept;
  /// The cached transition target for `id`, or no_archetype.
  [[nodiscard]] std::uint32_t cached(component_id id) const noexcept;

  std::array<component_id, max_components_per_archetype> ids_{};
  std::array<column_layout, max_components_per_archetype> columns_{};
  std::uint32_t count_ = 0;
  std::uint32_t capacity_ = 0;
  std::uint32_t creation_index_ = 0;
  std::uint32_t rows_ = 0;
  detail::buffer<std::byte *> chunks_;
  detail::buffer<transition> transitions_; // sorted by id
};

/// The RAII token of begin_iteration: while one is held a structural call trips the Debug
/// iteration guard (ARCH3-R112). Move-only.
class iteration_guard {
public:
  iteration_guard() noexcept = default;
  explicit iteration_guard(std::atomic<std::uint32_t> &counter) noexcept : counter_(&counter) {
    counter_->fetch_add(1);
  }
  iteration_guard(const iteration_guard &) = delete;
  iteration_guard &operator=(const iteration_guard &) = delete;
  iteration_guard(iteration_guard &&other) noexcept
      : counter_(std::exchange(other.counter_, nullptr)) {}
  iteration_guard &operator=(iteration_guard &&other) noexcept {
    if (this != &other) {
      end();
      counter_ = std::exchange(other.counter_, nullptr);
    }
    return *this;
  }
  ~iteration_guard() { end(); }

private:
  void end() noexcept {
    if (counter_ != nullptr) {
      counter_->fetch_sub(1);
      counter_ = nullptr;
    }
  }

  std::atomic<std::uint32_t> *counter_ = nullptr;
};

/// The whole state of a world as views into it (capture_image) or into a snapshot's decoded
/// buffers (restore_from). Views stay valid until the next structural change of their owner.
struct world_image {
  struct column_entry {
    component_id id;
    /// rows values of the component, contiguous (stride = component size).
    void *data = nullptr;
    std::span<std::uint32_t> added;
    std::span<std::uint32_t> changed;
  };
  struct chunk_entry {
    std::span<const entity> entities;
    std::vector<column_entry> columns; // in the archetype's components() order
  };
  struct archetype_entry {
    std::vector<component_id> components; // ascending
    std::vector<chunk_entry> chunks;      // every chunk full but the last
  };

  entity_allocator::state entities;
  std::vector<archetype_entry> archetypes;      // creation order
  std::vector<detail::resource_slot> resources; // ascending key
  std::uint32_t change_tick = 0;
};

/// Performance counters (recorded, work-bound).
struct world_stats {
  std::uint64_t structural_changes = 0;
  // Rows relocated: an add or remove moves its own row (1) and the swap-remove fill (0 or 1); a
  // destroy moves only the fill; a create moves none.
  std::uint64_t rows_moved = 0;      // over every successful structural change
  std::uint64_t last_rows_moved = 0; // by the latest successful structural change
  std::uint32_t archetypes = 0;
  std::uint32_t chunks = 0;        // chunks held by archetypes
  std::uint32_t empty_chunks = 0;  // held chunks with no row (returned at release_empty_chunks)
  std::uint32_t pooled_chunks = 0; // free chunks in the pool
  std::uint32_t slabs = 0;
  std::uint64_t rows = 0;
  std::uint64_t row_capacity = 0; // rows the held chunks can hold (occupancy = rows / capacity)
};

/// A world: entities, archetypes and resources over one memory resource. Neither copyable nor
/// movable (archetypes and guards point into it).
class world {
public:
  /// A world over `frozen` (invalid_argument when not frozen) whose memory comes from
  /// `upstream`; invalid_argument for a max_entities of 0 or a max_archetypes of 0 or above 4096;
  /// out_of_memory when the location table cannot be allocated. Both must outlive the world; the
  /// calling thread becomes the owner thread.
  [[nodiscard]] static result<std::unique_ptr<world>> make(const component_registry &frozen,
                                                           mem::resource &upstream,
                                                           world_config config = {}) noexcept;

  world(const world &) = delete;
  world(world &&) = delete;
  world &operator=(const world &) = delete;
  world &operator=(world &&) = delete;
  ~world();

  // --- structural calls (owner thread, outside phases) ---

  /// A new entity with no component.
  [[nodiscard]] result<entity> create() noexcept { return create_raw({}, {}); }
  /// A new entity with the given components (invalid_argument for a type given twice).
  template <class... Cs> [[nodiscard]] result<entity> create_with(Cs &&...values) noexcept {
    constexpr std::size_t n = sizeof...(Cs);
    static_assert((component_type<std::remove_cvref_t<Cs>> && ...));
    const std::array<result<component_id>, n> found{registry_->id_of<std::remove_cvref_t<Cs>>()...};
    std::array<component_id, n> ids{};
    for (std::size_t i = 0; i < n; ++i) {
      if (!found[i]) {
        return std::unexpected(found[i].error());
      }
      ids[i] = *found[i];
    }
    std::tuple<std::remove_cvref_t<Cs>...> staged(std::forward<Cs>(values)...);
    const std::array<void *, n> sources = std::apply(
        [](auto &...v) noexcept { return std::array<void *, n>{std::addressof(v)...}; }, staged);
    return create_raw(ids, sources);
  }
  /// Ends `e` and every component value it holds; stale_entity for a dead handle.
  [[nodiscard]] result<void> destroy(entity e) noexcept;
  /// Adds component T: stale_entity, component_unknown, component_exists, archetype_limit,
  /// out_of_memory.
  template <class T> [[nodiscard]] result<void> add(entity e, T &&value) noexcept {
    using U = std::remove_cvref_t<T>;
    static_assert(component_type<U>);
    const auto id = registry_->id_of<U>();
    if (!id) {
      return std::unexpected(id.error());
    }
    U staged(std::forward<T>(value));
    return add_raw(e, *id, std::addressof(staged), write_source::runtime);
  }
  /// Removes component T: stale_entity, component_unknown, component_missing, archetype_limit,
  /// out_of_memory.
  template <class T> [[nodiscard]] result<void> remove(entity e) noexcept {
    const auto id = registry_->id_of<std::remove_cvref_t<T>>();
    if (!id) {
      return std::unexpected(id.error());
    }
    return remove_raw(e, *id);
  }
  /// Replaces component T (or adds it when missing); a document write stamps no changed tick.
  template <class T>
  [[nodiscard]] result<void> set(entity e, T &&value,
                                 write_source source = write_source::runtime) noexcept {
    using U = std::remove_cvref_t<T>;
    static_assert(component_type<U>);
    const auto id = registry_->id_of<U>();
    if (!id) {
      return std::unexpected(id.error());
    }
    U staged(std::forward<T>(value));
    return set_raw(e, *id, std::addressof(staged), source);
  }

  // --- access ---

  /// Component T of `e`: stale_entity, component_unknown, component_missing.
  template <class T> [[nodiscard]] result<T *> get(entity e) noexcept {
    const auto id = registry_->id_of<T>();
    if (!id) {
      return std::unexpected(id.error());
    }
    auto p = get_raw(e, *id);
    if (!p) {
      return std::unexpected(p.error());
    }
    return static_cast<T *>(*p);
  }
  template <class T> [[nodiscard]] result<const T *> get(entity e) const noexcept {
    const auto id = registry_->id_of<T>();
    if (!id) {
      return std::unexpected(id.error());
    }
    auto p = get_raw(e, *id);
    if (!p) {
      return std::unexpected(p.error());
    }
    return static_cast<const T *>(*p);
  }
  /// Component T of `e` without checks (SL_DEBUG_ASSERT that `e` is alive and has T).
  template <class T> [[nodiscard]] T &get_unchecked(entity e) noexcept {
    const auto id = registry_->id_of<T>();
    SL_DEBUG_ASSERT(id.has_value(), "get_unchecked of an unregistered component");
    return *static_cast<T *>(get_unchecked_raw(e, id.value_or(component_id{})));
  }
  [[nodiscard]] bool is_alive(entity e) const noexcept { return entities_.is_alive(e); }
  /// True when `e` is alive and has `id`.
  [[nodiscard]] bool has(entity e, component_id id) const noexcept;

  // --- by component id (the templates call these; opaque payload components, seam V02-1) ---

  /// As create_with: values[i] is moved into component ids[i] (the caller still destroys it).
  [[nodiscard]] result<entity> create_raw(std::span<const component_id> ids,
                                          std::span<void *const> values) noexcept;
  /// As add: the value at `value` is moved in (the caller still destroys it).
  [[nodiscard]] result<void> add_raw(entity e, component_id id, void *value,
                                     write_source source) noexcept;
  [[nodiscard]] result<void> remove_raw(entity e, component_id id) noexcept;
  [[nodiscard]] result<void> set_raw(entity e, component_id id, void *value,
                                     write_source source) noexcept;
  [[nodiscard]] result<void *> get_raw(entity e, component_id id) const noexcept;
  [[nodiscard]] void *get_unchecked_raw(entity e, component_id id) const noexcept;

  // --- versions, ticks, phases ---

  /// +1 per structural change (ARCH3-R130).
  [[nodiscard]] std::uint64_t version() const noexcept { return version_; }
  /// +1 per new archetype (ARCH3-R130).
  [[nodiscard]] std::uint64_t archetype_generation() const noexcept { return generation_; }
  void set_change_tick(std::uint32_t tick) noexcept { change_tick_ = tick; }
  [[nodiscard]] std::uint32_t change_tick() const noexcept { return change_tick_; }
  /// Set by the frame and system schedulers around phases: structural calls inside are
  /// structural_change_forbidden.
  void set_phase_active(bool active) noexcept { phase_active_ = active; }
  [[nodiscard]] bool phase_active() const noexcept { return phase_active_; }
  /// The Debug iteration guard: a structural call while the token lives trips SL_DEBUG_ASSERT
  /// (and is refused with structural_change_forbidden).
  [[nodiscard]] iteration_guard begin_iteration() const noexcept {
    return iteration_guard(iterating_);
  }
  /// Owner thread: background jobs of `jobs` may not touch the world (SL_DEBUG_ASSERT in the
  /// accessors); nullptr detaches.
  void set_job_system(const jobs::job_system *jobs) noexcept { jobs_ = jobs; }

  // --- resources ---

  /// Stores `value` under `key`: already_exists for a used key, wrong_thread off the owner.
  template <class T> [[nodiscard]] result<void> add_resource(resource_key key, T value) noexcept {
    static_assert(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_destructible_v<T>);
    static_assert(alignof(T) <= mem::max_alignment);
    const detail::resource_slot slot{.key = key,
                                     .type_key = detail::type_key_of<T>(),
                                     .data = nullptr,
                                     .size = static_cast<std::uint32_t>(sizeof(T)),
                                     .align = static_cast<std::uint32_t>(alignof(T)),
                                     .move = &detail::move_value<T>,
                                     .destroy = &detail::destroy_value<T>};
    return add_resource_raw(slot, std::addressof(value));
  }
  /// The resource under `key`: not_found, invalid_argument when it holds another type.
  template <class T> [[nodiscard]] result<T *> resource(resource_key key) noexcept {
    auto p = resource_raw(key, detail::type_key_of<T>());
    if (!p) {
      return std::unexpected(p.error());
    }
    return static_cast<T *>(*p);
  }
  [[nodiscard]] result<void> add_resource_raw(const detail::resource_slot &slot,
                                              void *value) noexcept;
  [[nodiscard]] result<void *> resource_raw(resource_key key, const void *type_key) const noexcept;

  // --- reading the storage ---

  /// The archetypes in creation order.
  [[nodiscard]] std::span<const archetype *const> archetypes() const noexcept {
    return {archetypes_.data(), archetypes_.size()};
  }
  [[nodiscard]] const entity_allocator &entities() const noexcept { return entities_; }
  [[nodiscard]] const component_registry &registry() const noexcept { return *registry_; }
  [[nodiscard]] world_stats stats() const noexcept;

  // --- boundaries and snapshots ---

  /// Owner thread, boundary: returns every chunk left without rows to the pool.
  void release_empty_chunks() noexcept;
  /// The state as views into the world (allocator, archetypes in creation order with their
  /// non-empty chunks, resources by key, the change tick).
  [[nodiscard]] world_image capture_image() const;
  /// Owner thread: replaces the whole state with `image`, moving every value out of its columns
  /// and resources (the image owner still destroys them). invalid_argument for an inconsistent
  /// image, archetype_limit, out_of_memory; on error nothing changes.
  [[nodiscard]] result<void> restore_from(const world_image &image) noexcept;

private:
  struct location {
    std::uint32_t archetype = no_archetype;
    std::uint32_t row = 0;
  };
  /// A destination archetype: an existing index, or a staged new archetype (not registered).
  struct destination {
    std::uint32_t index = no_archetype;
    archetype *staged = nullptr;
    bool cache = false;      // record the transition in the source archetype
    bool took_chunk = false; // a chunk was pushed onto the destination
  };

  world(const component_registry &frozen, mem::resource &upstream,
        const world_config &config) noexcept;
  [[nodiscard]] result<void> init() noexcept;

  [[nodiscard]] result<void> check_structural() const noexcept;
  void check_access() const noexcept;
  [[nodiscard]] result<void> check_entity(entity e) const noexcept;

  // locations (ecs_world.cpp)
  [[nodiscard]] result<void> reserve_location(std::uint32_t index) noexcept;
  [[nodiscard]] location &location_of(std::uint32_t index) const noexcept;

  // the chunk pool (ecs_world.cpp)
  [[nodiscard]] result<std::byte *> acquire_chunk() noexcept;
  void release_chunk(std::byte *chunk) noexcept;

  // archetypes (ecs_world_structural.cpp)
  [[nodiscard]] std::uint32_t find_archetype(std::span<const component_id> sorted) const noexcept;
  [[nodiscard]] result<archetype *> stage_archetype(std::span<const component_id> sorted) noexcept;
  void discard_archetype(archetype *a) noexcept;
  void register_archetype(archetype *a) noexcept;
  [[nodiscard]] result<destination> resolve(std::span<const component_id> sorted) noexcept;
  [[nodiscard]] result<destination> transition(std::uint32_t source, component_id id,
                                               bool adding) noexcept;
  [[nodiscard]] result<void> reserve_row(destination &d) noexcept;
  void unwind(destination &d) noexcept;
  [[nodiscard]] archetype &commit(destination &d, std::uint32_t source, component_id id) noexcept;
  [[nodiscard]] static std::uint32_t take_row(archetype &a, entity e) noexcept;
  void fill_hole(archetype &a, std::uint32_t row) noexcept;
  void destroy_rows(archetype &a) noexcept;
  void note_change(std::uint64_t rows_moved) noexcept;

  // resources (ecs_world_resources.cpp)
  void destroy_resources() noexcept;

  const component_registry *registry_;
  mem::resource *memory_;
  mem::memory_tag meta_tag_;
  mem::memory_tag chunk_tag_;
  world_config config_;
  entity_allocator entities_;
  detail::buffer<location *> location_pages_;
  detail::buffer<archetype *> archetypes_;
  detail::buffer<std::byte *> slabs_;
  detail::buffer<detail::resource_slot> resources_;
  std::byte *free_chunks_ = nullptr;
  std::uint32_t pooled_chunks_ = 0;
  std::uint64_t version_ = 0;
  std::uint64_t generation_ = 0;
  std::uint64_t structural_changes_ = 0;
  std::uint64_t rows_moved_ = 0;
  std::uint64_t last_rows_moved_ = 0;
  std::uint32_t change_tick_ = 0;
  bool phase_active_ = false;
  mutable std::atomic<std::uint32_t> iterating_{0};
  std::thread::id owner_;
  const jobs::job_system *jobs_ = nullptr;
};

} // namespace sl::runtime::ecs
