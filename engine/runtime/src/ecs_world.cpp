// world: construction, checks, the location table, the chunk pool and access (WO-088, SL-ARCH-003
// ARCH3-R108, R109, R111, R112, R175, `limits` chunk_pool_slab_bytes, ADR-0025). The location
// table maps an entity index to (archetype, global row) in pages of entity_page_records records,
// allocated when an index is first handed out and never moved. The chunk pool carves 2 MiB slabs
// (aligned to 4096, tag ecs.chunks when the upstream's registry has it) into 128 chunks on an
// intrusive free list; a free chunk is filled 0xDD and ASan-poisoned except while its link is read
// or written. Slabs go back to the upstream only when the world is destroyed.

#include "sl/runtime/ecs/world.hpp"

#include "sl/core/assert.hpp"
#include "sl/core/error.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/memory.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <thread>

namespace sl::runtime::ecs {

namespace {

constexpr std::size_t location_page_alignment = 64;

[[nodiscard]] mem::memory_tag chunk_tag_of(const mem::resource &upstream) noexcept {
  if (upstream.tags() != nullptr) {
    auto tag = upstream.tags()->find("ecs.chunks");
    if (tag) {
      return *tag;
    }
  }
  return upstream.default_tag();
}

} // namespace

world::world(const component_registry &frozen, mem::resource &upstream,
             const world_config &config) noexcept
    : registry_(&frozen), memory_(&upstream), meta_tag_(upstream.default_tag()),
      chunk_tag_(chunk_tag_of(upstream)), config_(config), entities_(config.max_entities, upstream),
      location_pages_(upstream, meta_tag_), archetypes_(upstream, meta_tag_),
      slabs_(upstream, meta_tag_), resources_(upstream, meta_tag_),
      owner_(std::this_thread::get_id()) {}

result<std::unique_ptr<world>> world::make(const component_registry &frozen,
                                           mem::resource &upstream, world_config config) noexcept {
  if (!frozen.frozen()) {
    return make_error(errc::invalid_argument, "world::make: the component registry is not frozen");
  }
  if (config.max_entities == 0) {
    return make_error(errc::invalid_argument, "world::make: max_entities is 0");
  }
  if (config.max_archetypes == 0 || config.max_archetypes > max_archetypes) {
    return make_error(errc::invalid_argument, "world::make: max_archetypes outside 1..4096");
  }
  std::unique_ptr<world> w(new (std::nothrow) world(frozen, upstream, config));
  if (w == nullptr) {
    return make_error(errc::out_of_memory, "world::make: no memory for the world");
  }
  if (auto ok = w->init(); !ok) {
    return std::unexpected(ok.error());
  }
  return w;
}

result<void> world::init() noexcept {
  const std::size_t pages =
      (std::size_t{config_.max_entities} + entity_page_records - 1) / entity_page_records;
  if (auto ok = location_pages_.reserve(pages); !ok) {
    return ok;
  }
  for (std::size_t i = 0; i < pages; ++i) {
    location_pages_.push_back(nullptr);
  }
  return {};
}

world::~world() {
  for (archetype *a : archetypes_.span()) {
    destroy_rows(*a);
    std::destroy_at(a);
    memory_->release(a, sizeof(archetype), alignof(archetype));
  }
  archetypes_.reset();
  destroy_resources();
  for (std::byte *slab : slabs_.span()) {
    mem::unpoison(slab, chunk_pool_slab_bytes);
    memory_->release(slab, chunk_pool_slab_bytes, chunk_pool_slab_alignment);
  }
  slabs_.reset();
  free_chunks_ = nullptr;
  for (location *page : location_pages_.span()) {
    if (page != nullptr) {
      memory_->release(page, sizeof(location) * entity_page_records, location_page_alignment);
    }
  }
  location_pages_.reset();
}

// --- checks ---

void world::check_access() const noexcept {
#ifndef NDEBUG
  constexpr std::optional background_prio{jobs::priority::background};
  const bool background = jobs_ != nullptr && jobs_->current_job_priority() == background_prio;
  SL_DEBUG_ASSERT(!background, "a background job touched the world");
#endif
}

result<void> world::check_structural() const noexcept {
  if (std::this_thread::get_id() != owner_) {
    return make_error(errc::wrong_thread, "world: structural call off the owner thread");
  }
  const std::uint32_t iterating = iterating_.load();
  SL_DEBUG_ASSERT(iterating == 0, "structural call while a query iterates (iteration guard)");
  if (iterating != 0) {
    return make_error(errc::structural_change_forbidden,
                      "world: structural call while a query iterates");
  }
  if (phase_active_) {
    return make_error(errc::structural_change_forbidden,
                      "world: structural call inside a phase (use a command buffer)");
  }
  check_access();
  return {};
}

result<void> world::check_entity(entity e) const noexcept { return entities_.check(e); }

// --- locations ---

result<void> world::reserve_location(std::uint32_t index) noexcept {
  const std::uint32_t page = index / entity_page_records;
  if (page >= location_pages_.size() || location_pages_[page] != nullptr) {
    return {};
  }
  auto block = memory_->try_allocate(sizeof(location) * entity_page_records,
                                     location_page_alignment, meta_tag_);
  if (!block) {
    return std::unexpected(block.error());
  }
  auto *fresh = static_cast<location *>(*block);
  for (std::uint32_t i = 0; i < entity_page_records; ++i) {
    std::construct_at(fresh + i);
  }
  location_pages_[page] = fresh;
  return {};
}

world::location &world::location_of(std::uint32_t index) const noexcept {
  return location_pages_[index / entity_page_records][index % entity_page_records];
}

// --- the chunk pool ---

result<std::byte *> world::acquire_chunk() noexcept {
  if (free_chunks_ == nullptr) {
    if (auto ok = slabs_.reserve(slabs_.size() + 1); !ok) {
      return std::unexpected(ok.error());
    }
    auto block =
        memory_->try_allocate(chunk_pool_slab_bytes, chunk_pool_slab_alignment, chunk_tag_);
    if (!block) {
      return std::unexpected(block.error());
    }
    auto *slab = static_cast<std::byte *>(*block);
    slabs_.push_back(slab);
    for (std::uint32_t i = chunks_per_slab; i-- > 0;) {
      release_chunk(slab + (std::size_t{i} * chunk_bytes));
    }
  }
  std::byte *chunk = free_chunks_;
  mem::unpoison(chunk, sizeof(free_chunks_));
  std::memcpy(static_cast<void *>(&free_chunks_), chunk, sizeof(free_chunks_)); // the link
  mem::unpoison(chunk, chunk_bytes);
  mem::fill_fresh(chunk, chunk_bytes);
  --pooled_chunks_;
  return chunk;
}

void world::release_chunk(std::byte *chunk) noexcept {
  mem::fill_freed(chunk, chunk_bytes);
  std::memcpy(chunk, static_cast<const void *>(&free_chunks_), sizeof(std::byte *));
  mem::poison(chunk, chunk_bytes);
  free_chunks_ = chunk;
  ++pooled_chunks_;
}

void world::release_empty_chunks() noexcept {
  SL_DEBUG_ASSERT(std::this_thread::get_id() == owner_,
                  "world::release_empty_chunks off the owner thread");
  SL_DEBUG_ASSERT(!phase_active_, "world::release_empty_chunks inside a phase");
  SL_DEBUG_ASSERT(iterating_.load() == 0, "world::release_empty_chunks while a query iterates");
  for (archetype *a : archetypes_.span()) {
    const std::uint32_t needed = (a->rows_ + a->capacity_ - 1) / a->capacity_;
    while (a->chunks_.size() > needed) {
      release_chunk(a->chunks_[a->chunks_.size() - 1]);
      a->chunks_.pop_back();
    }
  }
}

// --- access ---

bool world::has(entity e, component_id id) const noexcept {
  if (!entities_.is_alive(e)) {
    return false;
  }
  return archetypes_[location_of(e.index).archetype]->contains(id);
}

result<void *> world::get_raw(entity e, component_id id) const noexcept {
  check_access();
  if (auto alive = entities_.check(e); !alive) {
    return std::unexpected(alive.error());
  }
  if (!registry_->contains(id)) {
    return make_error(errc::component_unknown, "world::get of an unregistered component id");
  }
  const location &at = location_of(e.index);
  const archetype &a = *archetypes_[at.archetype];
  const std::uint32_t p = a.position_of(id);
  if (p == max_components_per_archetype) {
    return make_error(errc::component_missing,
                      "world::get: the entity lacks " + std::string(registry_->info(id).name));
  }
  return static_cast<void *>(a.value_at(p, at.row));
}

void *world::get_unchecked_raw(entity e, component_id id) const noexcept {
  check_access();
  entities_.assume_alive(e);
  const location &at = location_of(e.index);
  const archetype &a = *archetypes_[at.archetype];
  const std::uint32_t p = a.position_of(id);
  SL_DEBUG_ASSERT(p != max_components_per_archetype,
                  "get_unchecked of a component the entity lacks");
  return a.value_at(p, at.row);
}

world_stats world::stats() const noexcept {
  world_stats s;
  s.structural_changes = structural_changes_;
  s.rows_moved = rows_moved_;
  s.last_rows_moved = last_rows_moved_;
  s.archetypes = static_cast<std::uint32_t>(archetypes_.size());
  s.pooled_chunks = pooled_chunks_;
  s.slabs = static_cast<std::uint32_t>(slabs_.size());
  for (const archetype *a : archetypes_.span()) {
    const std::uint32_t chunks = a->chunk_count();
    const std::uint32_t used = (a->rows_ + a->capacity_ - 1) / a->capacity_;
    s.chunks += chunks;
    s.empty_chunks += chunks - used;
    s.rows += a->rows_;
    s.row_capacity += std::uint64_t{chunks} * a->capacity_;
  }
  return s;
}

} // namespace sl::runtime::ecs
