// archetype accessors (WO-088, SL-ARCH-003 ARCH3-R107, R108, R111, ADR-0025). Rows are dense: a
// global row r lives in chunk r / capacity at row r % capacity, every chunk but the last non-empty
// one is full, and chunks past the rows are empty until release_empty_chunks.

#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/memory.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

namespace sl::runtime::ecs {

namespace {

// The address `offset` bytes into a chunk, as void * (the caller casts it to the column's type;
// every column offset is 64-byte aligned, ARCH3-R108).
[[nodiscard]] void *at_offset(std::byte *base, std::size_t offset) noexcept {
  return base + offset;
}

} // namespace

archetype::archetype(mem::resource &memory, mem::memory_tag tag) noexcept
    : chunks_(memory, tag), transitions_(memory, tag) {}

std::span<const component_id> archetype::components() const noexcept {
  return {ids_.data(), count_};
}

std::span<const archetype::column_layout> archetype::layout() const noexcept {
  return {columns_.data(), count_};
}

std::uint32_t archetype::chunk_count() const noexcept {
  return static_cast<std::uint32_t>(chunks_.size());
}

std::uint32_t archetype::rows(std::uint32_t chunk) const noexcept {
  const std::uint64_t first = std::uint64_t{chunk} * capacity_;
  if (chunk >= chunk_count() || first >= rows_) {
    return 0;
  }
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(capacity_, rows_ - first));
}

std::byte *archetype::chunk_at(std::uint32_t chunk) const noexcept {
  return chunk < chunk_count() ? chunks_[chunk] : nullptr;
}

std::span<const entity> archetype::entities(std::uint32_t chunk) const noexcept {
  std::byte *base = chunk_at(chunk);
  if (base == nullptr) {
    return {};
  }
  return {static_cast<const entity *>(at_offset(base, 0)), rows(chunk)};
}

std::uint32_t archetype::position_of(component_id id) const noexcept {
  const std::span<const component_id> ids = components();
  const auto it = std::ranges::lower_bound(ids, id);
  if (it == ids.end() || *it != id) {
    return max_components_per_archetype;
  }
  return static_cast<std::uint32_t>(it - ids.begin());
}

bool archetype::contains(component_id id) const noexcept {
  return position_of(id) != max_components_per_archetype;
}

void *archetype::column(component_id id, std::uint32_t chunk) const noexcept {
  const std::uint32_t p = position_of(id);
  std::byte *base = chunk_at(chunk);
  if (p == max_components_per_archetype || base == nullptr) {
    return nullptr;
  }
  return at_offset(base, layout()[p].offset);
}

std::span<std::uint32_t> archetype::added_ticks(component_id id,
                                                std::uint32_t chunk) const noexcept {
  const std::uint32_t p = position_of(id);
  std::byte *base = chunk_at(chunk);
  if (p == max_components_per_archetype || base == nullptr) {
    return {};
  }
  return {static_cast<std::uint32_t *>(at_offset(base, layout()[p].added_offset)), rows(chunk)};
}

std::span<std::uint32_t> archetype::changed_ticks(component_id id,
                                                  std::uint32_t chunk) const noexcept {
  const std::uint32_t p = position_of(id);
  std::byte *base = chunk_at(chunk);
  if (p == max_components_per_archetype || base == nullptr) {
    return {};
  }
  return {static_cast<std::uint32_t *>(at_offset(base, layout()[p].changed_offset)), rows(chunk)};
}

std::byte *archetype::entity_at(std::uint32_t row) const noexcept {
  return chunks_[row / capacity_] + (std::size_t{row % capacity_} * sizeof(entity));
}

std::byte *archetype::value_at(std::uint32_t position, std::uint32_t row) const noexcept {
  const column_layout &c = layout()[position];
  return chunks_[row / capacity_] + c.offset + (std::size_t{row % capacity_} * c.size);
}

std::uint32_t *archetype::added_at(std::uint32_t position, std::uint32_t row) const noexcept {
  const std::size_t offset =
      layout()[position].added_offset + (std::size_t{row % capacity_} * sizeof(std::uint32_t));
  return static_cast<std::uint32_t *>(at_offset(chunks_[row / capacity_], offset));
}

std::uint32_t *archetype::changed_at(std::uint32_t position, std::uint32_t row) const noexcept {
  const std::size_t offset =
      layout()[position].changed_offset + (std::size_t{row % capacity_} * sizeof(std::uint32_t));
  return static_cast<std::uint32_t *>(at_offset(chunks_[row / capacity_], offset));
}

std::uint32_t archetype::cached(component_id id) const noexcept {
  const auto span = transitions_.span();
  const auto it = std::ranges::lower_bound(span, id, std::less<>{}, &transition::id);
  return it != span.end() && it->id == id ? it->target : no_archetype;
}

} // namespace sl::runtime::ecs
