// capture_image and restore_from (WO-088, SL-ARCH-003 ARCH3-R107, R110, R111, ADR-0025; the
// snapshot order WO-095 uses only this API). capture_image lists views: the allocator state, every
// archetype in creation order with its non-empty chunks (entity column, per component the raw
// column and the tick columns) and the resources by key. restore_from validates the image, stages
// every archetype with its chunks, the location pages and the resource blocks, restores the entity
// allocator last (its own all-or-nothing restore) and only then moves the values in and tears the
// old state down; any failure before that point leaves the world unchanged.

#include "sl/runtime/ecs/world.hpp"

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/error.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace sl::runtime::ecs {

namespace {

constexpr std::uint8_t slot_dead = 0;
constexpr std::uint8_t slot_alive = 1;
constexpr std::uint8_t slot_placed = 2;

[[nodiscard]] std::unexpected<error> bad_image(const char *why) {
  return make_error(errc::invalid_argument, std::string("world::restore_from: ") + why);
}

} // namespace

world_image world::capture_image() const {
  world_image image;
  image.entities = entities_.capture();
  image.archetypes.reserve(archetypes_.size());
  for (const archetype *a : archetypes_.span()) {
    world_image::archetype_entry entry;
    entry.components.assign(a->components().begin(), a->components().end());
    const std::uint32_t used = (a->rows_ + a->capacity_ - 1) / a->capacity_;
    entry.chunks.reserve(used);
    for (std::uint32_t c = 0; c < used; ++c) {
      world_image::chunk_entry chunk;
      chunk.entities = a->entities(c);
      chunk.columns.reserve(a->count_);
      for (const component_id id : a->components()) {
        chunk.columns.push_back(world_image::column_entry{.id = id,
                                                          .data = a->column(id, c),
                                                          .added = a->added_ticks(id, c),
                                                          .changed = a->changed_ticks(id, c)});
      }
      entry.chunks.push_back(std::move(chunk));
    }
    image.archetypes.push_back(std::move(entry));
  }
  image.resources.assign(resources_.span().begin(), resources_.span().end());
  image.change_tick = change_tick_;
  return image;
}

result<void> world::restore_from(const world_image &image) noexcept {
  if (auto ok = check_structural(); !ok) {
    return ok;
  }
  const entity_allocator::state &st = image.entities;
  if (image.archetypes.size() > config_.max_archetypes) {
    return make_error(errc::archetype_limit, "world::restore_from: too many archetypes");
  }
  if (st.next_index > config_.max_entities || st.generations.size() != st.next_index) {
    return bad_image("inconsistent entity allocator state");
  }
  // Which indices the image's allocator holds alive (the allocator validates the rest).
  std::vector<std::uint8_t> slots(st.next_index, slot_alive);
  for (const std::uint32_t i : st.free_fifo) {
    if (i < st.next_index) {
      slots[i] = slot_dead;
    }
  }
  for (const std::uint32_t i : st.retired) {
    if (i < st.next_index) {
      slots[i] = slot_dead;
    }
  }

  detail::buffer<archetype *> staged(*memory_, meta_tag_);
  detail::buffer<detail::resource_slot> fresh(*memory_, meta_tag_);
  const auto abandon = [&] noexcept {
    for (archetype *a : staged.span()) {
      discard_archetype(a);
    }
    staged.clear();
    for (const detail::resource_slot &s : fresh.span()) {
      memory_->release(s.data, s.size, s.align);
    }
    fresh.clear();
  };
  if (auto ok = staged.reserve(image.archetypes.size()); !ok) {
    return ok;
  }
  if (auto ok = archetypes_.reserve(image.archetypes.size()); !ok) {
    return ok;
  }

  for (std::size_t k = 0; k < image.archetypes.size(); ++k) {
    const world_image::archetype_entry &entry = image.archetypes[k];
    const std::span<const component_id> comps(entry.components);
    if (comps.size() > max_components_per_archetype) {
      abandon();
      return make_error(errc::archetype_limit, "world::restore_from: more than 32 components");
    }
    for (std::size_t i = 0; i < comps.size(); ++i) {
      if (!registry_->contains(comps[i]) || (i != 0 && !(comps[i - 1] < comps[i]))) {
        abandon();
        return bad_image("components unregistered or not strictly ascending");
      }
    }
    for (std::size_t j = 0; j < k; ++j) {
      if (std::ranges::equal(image.archetypes[j].components, entry.components)) {
        abandon();
        return bad_image("two archetypes with one signature");
      }
    }
    auto made = stage_archetype(comps);
    if (!made) {
      abandon();
      return std::unexpected(made.error());
    }
    archetype &a = **made;
    staged.push_back(&a);
    std::uint64_t rows = 0;
    bool short_seen = false;
    for (const world_image::chunk_entry &chunk : entry.chunks) {
      const std::size_t n = chunk.entities.size();
      if (n > a.capacity_ || (short_seen && n != 0) || chunk.columns.size() != comps.size()) {
        abandon();
        return bad_image("a chunk is not dense or its columns do not match the archetype");
      }
      short_seen = short_seen || n < a.capacity_;
      for (std::size_t p = 0; p < comps.size(); ++p) {
        const world_image::column_entry &col = chunk.columns[p];
        if (col.id != comps[p] || col.added.size() != n || col.changed.size() != n ||
            (n != 0 && col.data == nullptr)) {
          abandon();
          return bad_image("a column does not match its chunk");
        }
      }
      for (const entity e : chunk.entities) {
        if (e.index >= st.next_index || slots[e.index] != slot_alive ||
            st.generations[e.index] != e.generation) {
          abandon();
          return bad_image("an entity is not alive in the image or appears twice");
        }
        slots[e.index] = slot_placed;
      }
      rows += n;
    }
    const auto needed = static_cast<std::uint32_t>((rows + a.capacity_ - 1) / a.capacity_);
    if (auto ok = a.chunks_.reserve(needed); !ok) {
      abandon();
      return ok;
    }
    for (std::uint32_t c = 0; c < needed; ++c) {
      auto chunk = acquire_chunk();
      if (!chunk) {
        abandon();
        return std::unexpected(chunk.error());
      }
      a.chunks_.push_back(*chunk);
    }
  }
  if (std::ranges::find(slots, slot_alive) != slots.end()) {
    abandon();
    return bad_image("an entity alive in the image has no row");
  }
  for (std::uint32_t i = 0; i < st.next_index; i += entity_page_records) {
    if (auto ok = reserve_location(i); !ok) {
      abandon();
      return ok;
    }
  }
  if (auto ok = fresh.reserve(image.resources.size()); !ok) {
    abandon();
    return ok;
  }
  for (std::size_t k = 0; k < image.resources.size(); ++k) {
    const detail::resource_slot &s = image.resources[k];
    if ((k != 0 && !(image.resources[k - 1].key < s.key)) || s.data == nullptr ||
        s.move == nullptr || s.destroy == nullptr || s.size == 0 ||
        !mem::is_valid_alignment(s.align)) {
      abandon();
      return bad_image("resources malformed or not strictly ascending by key");
    }
    auto block = memory_->try_allocate(s.size, s.align, meta_tag_);
    if (!block) {
      abandon();
      return std::unexpected(block.error());
    }
    detail::resource_slot slot = s;
    slot.data = *block;
    fresh.push_back(slot);
  }
  if (auto ok = entities_.restore(st); !ok) {
    abandon();
    return ok;
  }

  // Nothing fails from here on: move the values in, then tear the old state down.
  for (std::size_t k = 0; k < staged.size(); ++k) {
    archetype &a = *staged[k];
    for (const world_image::chunk_entry &chunk : image.archetypes[k].chunks) {
      for (std::size_t r = 0; r < chunk.entities.size(); ++r) {
        const std::uint32_t row = take_row(a, chunk.entities[r]);
        for (std::uint32_t p = 0; p < a.count_; ++p) {
          const component_info &info = registry_->info(a.components()[p]);
          const world_image::column_entry &col = chunk.columns[p];
          void *src = static_cast<std::byte *>(col.data) + (r * info.size);
          if (info.trivially_relocatable) {
            std::memcpy(a.value_at(p, row), src, info.size);
          } else {
            info.move(a.value_at(p, row), src);
          }
          *a.added_at(p, row) = col.added[r];
          *a.changed_at(p, row) = col.changed[r];
        }
      }
    }
  }
  for (std::size_t k = 0; k < fresh.size(); ++k) {
    fresh[k].move(fresh[k].data, image.resources[k].data);
  }
  for (archetype *a : archetypes_.span()) {
    destroy_rows(*a);
    discard_archetype(a);
  }
  archetypes_.clear();
  destroy_resources();
  resources_ = std::move(fresh);
  for (archetype *a : staged.span()) {
    register_archetype(a);
    for (std::uint32_t row = 0; row < a->rows_; ++row) {
      entity e;
      std::memcpy(&e, a->entity_at(row), sizeof(entity));
      location_of(e.index) = location{.archetype = a->creation_index_, .row = row};
    }
  }
  staged.clear();
  ++generation_;
  change_tick_ = image.change_tick;
  last_rows_moved_ = 0;
  note_change(0);
  return {};
}

} // namespace sl::runtime::ecs
