// mark_changed and last_run_ticks (WO-093, SL-ARCH-003 ARCH3-R127, ARCH3-R128, ADR-0025).
// mark_changed_raw finds the row of `e` through world::get_raw (which checks the handle and the
// component) and the archetype views: the chunk of an archetype holding the component whose
// column contains the value is the row's chunk, and the offset gives the row. It writes only that
// row's changed tick, so distinct rows may be marked from distinct threads.

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/change.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/error.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace sl::runtime::ecs {

result<void> mark_changed_raw(world &w, entity e, component_id id, std::uint32_t tick) noexcept {
  auto value = w.get_raw(e, id);
  if (!value) {
    return std::unexpected(value.error());
  }
  const auto *target = static_cast<const std::byte *>(*value);
  const std::uint32_t size = w.registry().info(id).size;
  for (const archetype *a : w.archetypes()) {
    if (!a->contains(id)) {
      continue;
    }
    for (std::uint32_t c = 0; c < a->chunk_count(); ++c) {
      const std::uint32_t rows = a->rows(c);
      if (rows == 0) {
        break;
      }
      const auto *first = static_cast<const std::byte *>(a->column(id, c));
      const std::byte *end = first + (std::size_t{rows} * size);
      if (std::less_equal<const std::byte *>{}(first, target) &&
          std::less<const std::byte *>{}(target, end)) {
        const auto row = static_cast<std::size_t>(target - first) / size;
        a->changed_ticks(id, c)[row] = tick;
        return {};
      }
    }
  }
  return make_error(errc::component_missing, "mark_changed: the row of the entity was not found");
}

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index): every index is checked below
// last_run_slots, the size of both arrays.
std::uint32_t last_run_ticks::since(std::uint32_t system) const noexcept {
  if (system >= last_run_slots || recorded_[system] == 0) {
    return floor_;
  }
  return ticks_[system];
}

void last_run_ticks::record(std::uint32_t system, std::uint32_t tick) noexcept {
  if (system >= last_run_slots) {
    return;
  }
  ticks_[system] = tick;
  recorded_[system] = 1;
}

void last_run_ticks::restore(std::span<const entry> saved, std::uint32_t saved_floor) noexcept {
  ticks_.fill(0);
  recorded_.fill(0);
  floor_ = saved_floor;
  for (const entry &e : saved) {
    record(e.system, e.tick);
  }
}

std::vector<last_run_ticks::entry> last_run_ticks::entries() const noexcept {
  std::vector<entry> out;
  for (std::uint32_t s = 0; s < last_run_slots; ++s) {
    if (recorded_[s] != 0) {
      out.push_back(entry{.system = s, .tick = ticks_[s]});
    }
  }
  return out;
}
// NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)

} // namespace sl::runtime::ecs
