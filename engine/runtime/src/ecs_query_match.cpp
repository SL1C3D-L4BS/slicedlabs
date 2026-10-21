// The query match cache (WO-090, SL-ARCH-003 ARCH3-R115, ADR-0025). The cache keeps the creation
// indices of the matched archetypes (ascending, so iteration stays in creation order), how many
// of the world's archetypes it has scanned and the archetype generation it saw then. Archetypes
// are only ever appended (ARCH3-R107), each new one adding 1 to the generation, so when the
// generation moved by exactly the number of new archetypes only those are scanned. Any other
// change (another world, restore_from, which replaces every archetype and adds one more to the
// generation) drops the cache and scans every archetype again.

#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/query.hpp"
#include "sl/runtime/ecs/world.hpp"

#include <algorithm>
#include <cstdint>
#include <span>

namespace sl::runtime::ecs::detail {

bool query_core::matches(const archetype &a) const noexcept {
  const std::span<const component_id> have = a.components();
  const std::span<const component_id> need{required_.data(), required_count_};
  if (!std::ranges::includes(have, need)) {
    return false;
  }
  const std::span<const component_id> refuse{excluded_.data(), excluded_count_};
  return std::ranges::none_of(refuse, [&a](component_id id) noexcept { return a.contains(id); });
}

void query_core::refresh(const world &w) noexcept {
  const std::span<const archetype *const> all = w.archetypes();
  const auto count = static_cast<std::uint32_t>(all.size());
  const std::uint64_t generation = w.archetype_generation();
  const bool appended = seen_ == &w && count >= scanned_ && generation >= generation_ &&
                        generation - generation_ == count - scanned_;
  if (!appended) {
    matched_.clear();
    scanned_ = 0;
    seen_ = &w;
  }
  for (std::uint32_t index = scanned_; index < count; ++index) {
    ++archetypes_visited_;
    if (matches(*all[index])) {
      matched_.push_back(index);
    }
  }
  scanned_ = count;
  generation_ = generation;
}

} // namespace sl::runtime::ecs::detail
