// Inline par_for_each batches (WO-094, SL-ARCH-003 ARCH3-R090, R116): batch_cursor gives every
// for_each_chunk run the batch index query::par_for_each would give its chunk (whole non-empty
// chunks of the matched archetypes in creation order, a batch closing at par_batch_min_rows rows
// or more), so serial_canonical and non-chunked systems key their work like the parallel policy.

#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/query.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/ecs/world.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace sl::runtime::ecs::detail {

bool batch_cursor::matches(const archetype &a) const noexcept {
  return std::ranges::all_of(required_, [&a](component_id c) noexcept { return a.contains(c); }) &&
         std::ranges::none_of(excluded_, [&a](component_id c) noexcept { return a.contains(c); });
}

std::uint32_t batch_cursor::locate(const entity *first) noexcept {
  const std::span<const archetype *const> all = world_->archetypes();
  while (slot_ < all.size()) {
    const archetype &a = *all[slot_];
    if (!matches(a) || chunk_ >= a.chunk_count() || a.rows(chunk_) == 0) {
      ++slot_; // rows are dense: the archetype's later chunks are empty too
      chunk_ = 0;
      continue;
    }
    const std::span<const entity> column = a.entities(chunk_);
    const std::less<> before;
    if (!before(first, column.data()) && before(first, std::to_address(column.end()))) {
      return batch_;
    }
    rows_in_ += static_cast<std::uint32_t>(column.size());
    if (rows_in_ >= par_batch_min_rows) {
      ++batch_;
      rows_in_ = 0;
    }
    ++chunk_;
  }
  return batch_;
}

} // namespace sl::runtime::ecs::detail
