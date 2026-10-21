// Structural changes (WO-088, SL-ARCH-003 ARCH3-R107, R109 to R112, ADR-0025, ADR-0023). Every
// call runs in two halves. The first half reserves: it resolves the destination archetype (the
// cached transition, else an existing archetype with the signature, else a staged new archetype
// that is not registered yet), reserves the transition cache slot, the archetype list slot, the
// destination row (a chunk from the pool, which may take a slab), the location page and, for
// create, the entity. Any failure there unwinds what the first half took and returns with the
// world unchanged (ARCH3-R110). The second half cannot fail: it registers the staged archetype,
// records the transition, moves the row and swap-removes the source row.

#include "sl/runtime/ecs/world.hpp"

#include "sl/core/assert.hpp"
#include "sl/core/error.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/memory.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <span>
#include <string>

namespace sl::runtime::ecs {

namespace {

// Constructs the value at dst from the value at src; src stays alive (its owner destroys it).
void place(const component_info &info, void *dst, void *src) noexcept {
  if (info.trivially_relocatable) {
    std::memcpy(dst, src, info.size);
  } else {
    info.move(dst, src);
  }
}

// Moves the value at src to dst and ends it at src.
void relocate(const component_info &info, void *dst, void *src) noexcept {
  if (info.trivially_relocatable) {
    std::memcpy(dst, src, info.size);
  } else {
    info.move(dst, src);
    info.destroy(src);
  }
}

void end_value(const component_info &info, void *p) noexcept {
  if (!info.trivially_relocatable) {
    info.destroy(p);
  }
}

using signature = std::array<component_id, max_components_per_archetype>;

} // namespace

// --- archetypes ---

std::uint32_t world::find_archetype(std::span<const component_id> sorted) const noexcept {
  for (std::size_t i = 0; i < archetypes_.size(); ++i) {
    const auto have = archetypes_[i]->components();
    if (std::ranges::equal(have, sorted)) {
      return static_cast<std::uint32_t>(i);
    }
  }
  return no_archetype;
}

result<archetype *> world::stage_archetype(std::span<const component_id> sorted) noexcept {
  if (sorted.size() > max_components_per_archetype) {
    return make_error(errc::archetype_limit, "world: an archetype holds at most 32 components");
  }
  auto block = memory_->try_allocate(sizeof(archetype), alignof(archetype), meta_tag_);
  if (!block) {
    return std::unexpected(block.error());
  }
  auto *a = ::new (*block) archetype(*memory_, meta_tag_);
  if (!a->compute_layout(sorted, *registry_)) {
    discard_archetype(a);
    return make_error(errc::archetype_limit,
                      "world: the components of the archetype do not fit one row in a chunk");
  }
  return a;
}

void world::discard_archetype(archetype *a) noexcept {
  for (std::byte *chunk : a->chunks_.span()) {
    release_chunk(chunk);
  }
  std::destroy_at(a);
  memory_->release(a, sizeof(archetype), alignof(archetype));
}

void world::register_archetype(archetype *a) noexcept {
  a->creation_index_ = static_cast<std::uint32_t>(archetypes_.size());
  archetypes_.push_back(a);
  ++generation_;
}

result<world::destination> world::resolve(std::span<const component_id> sorted) noexcept {
  const std::uint32_t found = find_archetype(sorted);
  if (found != no_archetype) {
    return destination{.index = found};
  }
  if (archetypes_.size() >= config_.max_archetypes) {
    return make_error(errc::archetype_limit,
                      "world: archetype limit " + std::to_string(config_.max_archetypes));
  }
  if (auto ok = archetypes_.reserve(archetypes_.size() + 1); !ok) {
    return std::unexpected(ok.error());
  }
  auto staged = stage_archetype(sorted);
  if (!staged) {
    return std::unexpected(staged.error());
  }
  return destination{.index = static_cast<std::uint32_t>(archetypes_.size()), .staged = *staged};
}

result<world::destination> world::transition(std::uint32_t source, component_id id,
                                             bool adding) noexcept {
  archetype &src = *archetypes_[source];
  const std::uint32_t cached = src.cached(id);
  if (cached != no_archetype) {
    return destination{.index = cached};
  }
  signature sig{};
  const std::span<component_id> out(sig);
  std::size_t n = 0;
  if (adding) {
    if (src.count_ >= max_components_per_archetype) {
      return make_error(errc::archetype_limit, "world: an archetype holds at most 32 components");
    }
    bool placed = false;
    for (const component_id have : src.components()) {
      if (!placed && id < have) {
        out[n++] = id;
        placed = true;
      }
      out[n++] = have;
    }
    if (!placed) {
      out[n++] = id;
    }
  } else {
    for (const component_id have : src.components()) {
      if (have != id) {
        out[n++] = have;
      }
    }
  }
  auto d = resolve(std::span<const component_id>(sig.data(), n));
  if (!d) {
    return d;
  }
  if (auto ok = src.transitions_.reserve(src.transitions_.size() + 1); !ok) {
    unwind(*d);
    return std::unexpected(ok.error());
  }
  d->cache = true;
  return d;
}

result<void> world::reserve_row(destination &d) noexcept {
  archetype &a = d.staged != nullptr ? *d.staged : *archetypes_[d.index];
  if (std::uint64_t{a.rows_} < std::uint64_t{a.chunk_count()} * a.capacity_) {
    return {};
  }
  if (auto ok = a.chunks_.reserve(a.chunks_.size() + 1); !ok) {
    return ok;
  }
  auto chunk = acquire_chunk();
  if (!chunk) {
    return std::unexpected(chunk.error());
  }
  a.chunks_.push_back(*chunk);
  d.took_chunk = true;
  return {};
}

void world::unwind(destination &d) noexcept {
  if (d.staged != nullptr) {
    discard_archetype(d.staged);
    d.staged = nullptr;
  } else if (d.took_chunk) {
    archetype &a = *archetypes_[d.index];
    release_chunk(a.chunks_[a.chunks_.size() - 1]);
    a.chunks_.pop_back();
  }
  d.took_chunk = false;
}

archetype &world::commit(destination &d, std::uint32_t source, component_id id) noexcept {
  if (d.staged != nullptr) {
    register_archetype(d.staged);
    d.index = d.staged->creation_index_;
    d.staged = nullptr;
  }
  if (d.cache) {
    auto &cache = archetypes_[source]->transitions_;
    const auto span = cache.span();
    const auto it = std::ranges::lower_bound(span, id, std::less<>{}, &archetype::transition::id);
    cache.insert_at(static_cast<std::size_t>(it - span.begin()),
                    archetype::transition{.id = id, .target = d.index});
  }
  return *archetypes_[d.index];
}

// --- rows ---

std::uint32_t world::take_row(archetype &a, entity e) noexcept {
  const std::uint32_t row = a.rows_++;
  std::memcpy(a.entity_at(row), &e, sizeof(entity));
  return row;
}

void world::fill_hole(archetype &a, std::uint32_t row) noexcept {
  const std::uint32_t last = a.rows_ - 1;
  if (row != last) {
    entity moved;
    std::memcpy(&moved, a.entity_at(last), sizeof(entity));
    std::memcpy(a.entity_at(row), &moved, sizeof(entity));
    for (std::uint32_t p = 0; p < a.count_; ++p) {
      relocate(registry_->info(a.components()[p]), a.value_at(p, row), a.value_at(p, last));
      *a.added_at(p, row) = *a.added_at(p, last);
      *a.changed_at(p, row) = *a.changed_at(p, last);
    }
    location_of(moved.index).row = row;
    ++last_rows_moved_;
  }
  --a.rows_;
}

void world::destroy_rows(archetype &a) noexcept {
  for (std::uint32_t p = 0; p < a.count_; ++p) {
    const component_info &info = registry_->info(a.components()[p]);
    if (info.trivially_relocatable) {
      continue;
    }
    for (std::uint32_t row = 0; row < a.rows_; ++row) {
      info.destroy(a.value_at(p, row));
    }
  }
  a.rows_ = 0;
}

void world::note_change(std::uint64_t rows_moved) noexcept {
  ++version_;
  ++structural_changes_;
  last_rows_moved_ += rows_moved;
  rows_moved_ += last_rows_moved_;
}

// --- the structural calls ---

result<entity> world::create_raw(std::span<const component_id> ids,
                                 std::span<void *const> values) noexcept {
  if (auto ok = check_structural(); !ok) {
    return std::unexpected(ok.error());
  }
  if (ids.size() != values.size()) {
    return make_error(errc::invalid_argument, "world::create: ids and values differ in count");
  }
  if (ids.size() > max_components_per_archetype) {
    return make_error(errc::archetype_limit, "world: an archetype holds at most 32 components");
  }
  if (!std::ranges::all_of(ids, [this](component_id id) { return registry_->contains(id); })) {
    return make_error(errc::component_unknown, "world::create of an unregistered component id");
  }
  signature sig{};
  std::ranges::copy(ids, sig.begin());
  const auto sorted = std::span<component_id>(sig.data(), ids.size());
  std::ranges::sort(sorted);
  if (std::ranges::adjacent_find(sorted) != sorted.end()) {
    return make_error(errc::invalid_argument, "world::create: a component is given twice");
  }
  auto d = resolve(sorted);
  if (!d) {
    return std::unexpected(d.error());
  }
  if (auto ok = reserve_row(*d); !ok) {
    unwind(*d);
    return std::unexpected(ok.error());
  }
  if (entities_.next_index() < config_.max_entities) {
    if (auto ok = reserve_location(entities_.next_index()); !ok) {
      unwind(*d);
      return std::unexpected(ok.error());
    }
  }
  auto e = entities_.create();
  if (!e) {
    unwind(*d);
    return e;
  }
  last_rows_moved_ = 0;
  archetype &a = commit(*d, no_archetype, component_id{});
  const std::uint32_t row = take_row(a, *e);
  for (std::uint32_t p = 0; p < a.count_; ++p) {
    const component_id cid = a.components()[p];
    const auto k = static_cast<std::size_t>(std::ranges::find(ids, cid) - ids.begin());
    place(registry_->info(cid), a.value_at(p, row), values[k]);
    *a.added_at(p, row) = change_tick_;
    *a.changed_at(p, row) = change_tick_;
  }
  location_of(e->index) = location{.archetype = d->index, .row = row};
  note_change(0);
  return e;
}

result<void> world::destroy(entity e) noexcept {
  if (auto ok = check_structural(); !ok) {
    return ok;
  }
  if (auto ok = check_entity(e); !ok) {
    return ok;
  }
  const location at = location_of(e.index);
  archetype &a = *archetypes_[at.archetype];
  for (std::uint32_t p = 0; p < a.count_; ++p) {
    end_value(registry_->info(a.components()[p]), a.value_at(p, at.row));
  }
  last_rows_moved_ = 0;
  fill_hole(a, at.row);
  const auto gone = entities_.destroy(e);
  SL_ASSERT(gone.has_value(), "world::destroy: the allocator refused a live handle");
  location_of(e.index) = location{};
  note_change(0);
  return {};
}

result<void> world::add_raw(entity e, component_id id, void *value, write_source source) noexcept {
  if (auto ok = check_structural(); !ok) {
    return ok;
  }
  if (auto ok = check_entity(e); !ok) {
    return ok;
  }
  if (!registry_->contains(id)) {
    return make_error(errc::component_unknown, "world::add of an unregistered component id");
  }
  const location at = location_of(e.index);
  if (archetypes_[at.archetype]->contains(id)) {
    return make_error(errc::component_exists, "world::add: the entity already has " +
                                                  std::string(registry_->info(id).name));
  }
  auto d = transition(at.archetype, id, true);
  if (!d) {
    return std::unexpected(d.error());
  }
  if (auto ok = reserve_row(*d); !ok) {
    unwind(*d);
    return ok;
  }
  last_rows_moved_ = 0;
  archetype &dst = commit(*d, at.archetype, id);
  archetype &src = *archetypes_[at.archetype];
  const std::uint32_t row = take_row(dst, e);
  for (std::uint32_t p = 0; p < dst.count_; ++p) {
    const component_id cid = dst.components()[p];
    const component_info &info = registry_->info(cid);
    if (cid == id) {
      place(info, dst.value_at(p, row), value);
      *dst.added_at(p, row) = change_tick_;
      *dst.changed_at(p, row) = source == write_source::document ? 0 : change_tick_;
    } else {
      const std::uint32_t q = src.position_of(cid);
      relocate(info, dst.value_at(p, row), src.value_at(q, at.row));
      *dst.added_at(p, row) = *src.added_at(q, at.row);
      *dst.changed_at(p, row) = *src.changed_at(q, at.row);
    }
  }
  fill_hole(src, at.row);
  location_of(e.index) = location{.archetype = d->index, .row = row};
  note_change(1);
  return {};
}

result<void> world::remove_raw(entity e, component_id id) noexcept {
  if (auto ok = check_structural(); !ok) {
    return ok;
  }
  if (auto ok = check_entity(e); !ok) {
    return ok;
  }
  if (!registry_->contains(id)) {
    return make_error(errc::component_unknown, "world::remove of an unregistered component id");
  }
  const location at = location_of(e.index);
  const std::uint32_t gone = archetypes_[at.archetype]->position_of(id);
  if (gone == max_components_per_archetype) {
    return make_error(errc::component_missing,
                      "world::remove: the entity lacks " + std::string(registry_->info(id).name));
  }
  auto d = transition(at.archetype, id, false);
  if (!d) {
    return std::unexpected(d.error());
  }
  if (auto ok = reserve_row(*d); !ok) {
    unwind(*d);
    return ok;
  }
  last_rows_moved_ = 0;
  archetype &dst = commit(*d, at.archetype, id);
  archetype &src = *archetypes_[at.archetype];
  const std::uint32_t row = take_row(dst, e);
  for (std::uint32_t p = 0; p < dst.count_; ++p) {
    const component_id cid = dst.components()[p];
    const std::uint32_t q = src.position_of(cid);
    relocate(registry_->info(cid), dst.value_at(p, row), src.value_at(q, at.row));
    *dst.added_at(p, row) = *src.added_at(q, at.row);
    *dst.changed_at(p, row) = *src.changed_at(q, at.row);
  }
  end_value(registry_->info(id), src.value_at(gone, at.row));
  fill_hole(src, at.row);
  location_of(e.index) = location{.archetype = d->index, .row = row};
  note_change(1);
  return {};
}

result<void> world::set_raw(entity e, component_id id, void *value, write_source source) noexcept {
  if (auto ok = check_structural(); !ok) {
    return ok;
  }
  if (auto ok = check_entity(e); !ok) {
    return ok;
  }
  if (!registry_->contains(id)) {
    return make_error(errc::component_unknown, "world::set of an unregistered component id");
  }
  const location at = location_of(e.index);
  const archetype &a = *archetypes_[at.archetype];
  const std::uint32_t p = a.position_of(id);
  if (p == max_components_per_archetype) {
    return add_raw(e, id, value, source);
  }
  const component_info &info = registry_->info(id);
  void *slot = a.value_at(p, at.row);
  end_value(info, slot);
  place(info, slot, value);
  if (source == write_source::runtime) {
    *a.changed_at(p, at.row) = change_tick_;
  }
  return {};
}

} // namespace sl::runtime::ecs
