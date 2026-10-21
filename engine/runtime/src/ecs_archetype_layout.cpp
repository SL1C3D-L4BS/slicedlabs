// The chunk layout (WO-088, SL-ARCH-003 ARCH3-R108, `limits` chunk_bytes, chunk_alignment,
// max_rows_per_chunk, ADR-0025; runtime ABI). For n rows: the entity column at offset 0 (8n bytes),
// then each component column in (alignment descending, id ascending) order at the next 64-byte
// aligned offset (size x n bytes), then for each component in the same order its u32 added and
// changed tick columns, each at the next 64-byte aligned offset. The capacity is the largest
// n <= 1024 whose end does not pass 16384; the layout grows with n, so a binary search finds it.

#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>

namespace sl::runtime::ecs {

namespace {

constexpr std::uint64_t align_up(std::uint64_t v) noexcept {
  return (v + chunk_alignment - 1) & ~std::uint64_t{chunk_alignment - 1};
}

// The positions in column order: (alignment descending, id ascending).
using order = std::array<std::uint32_t, max_components_per_archetype>;

// The bytes the layout of n rows takes; writes the offsets into `columns` when `write`.
std::uint64_t layout_bytes(std::span<archetype::column_layout> columns,
                           std::span<const std::uint32_t> by, std::uint32_t n,
                           bool write) noexcept {
  std::uint64_t at = std::uint64_t{n} * sizeof(entity);
  for (const std::uint32_t k : by) {
    archetype::column_layout &c = columns[k];
    at = align_up(at);
    if (write) {
      c.offset = static_cast<std::uint32_t>(at);
    }
    at += std::uint64_t{c.size} * n;
  }
  for (const std::uint32_t k : by) {
    archetype::column_layout &c = columns[k];
    at = align_up(at);
    if (write) {
      c.added_offset = static_cast<std::uint32_t>(at);
    }
    at += std::uint64_t{n} * sizeof(std::uint32_t);
    at = align_up(at);
    if (write) {
      c.changed_offset = static_cast<std::uint32_t>(at);
    }
    at += std::uint64_t{n} * sizeof(std::uint32_t);
  }
  return at;
}

} // namespace

bool archetype::compute_layout(std::span<const component_id> sorted,
                               const component_registry &registry) noexcept {
  count_ = static_cast<std::uint32_t>(sorted.size());
  const std::span<component_id> ids(ids_.data(), count_);
  const std::span<column_layout> cols(columns_.data(), count_);
  for (std::uint32_t i = 0; i < count_; ++i) {
    const component_info &info = registry.info(sorted[i]);
    ids[i] = sorted[i];
    cols[i] = column_layout{.id = sorted[i], .size = info.size, .align = info.align};
  }
  order positions{};
  const std::span<std::uint32_t> by(positions.data(), count_);
  std::ranges::iota(by, 0U);
  std::ranges::sort(by, [cols](std::uint32_t a, std::uint32_t b) {
    if (cols[a].align != cols[b].align) {
      return cols[a].align > cols[b].align;
    }
    return cols[a].id < cols[b].id;
  });
  if (layout_bytes(cols, by, 1, false) > chunk_bytes) {
    capacity_ = 0;
    return false;
  }
  std::uint32_t lo = 1;
  std::uint32_t hi = max_rows_per_chunk;
  while (lo < hi) {
    const std::uint32_t mid = lo + ((hi - lo + 1) / 2);
    if (layout_bytes(cols, by, mid, false) <= chunk_bytes) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  capacity_ = lo;
  static_cast<void>(layout_bytes(cols, by, lo, true));
  return true;
}

} // namespace sl::runtime::ecs
