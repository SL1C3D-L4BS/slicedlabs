// The tick-wrap sweep (WO-093, SL-ARCH-003 ARCH3-R131, ADR-0025). A tick is older than
// change_tick_max_age when now - tick (u32) is above it; the sweep sets it to
// now - change_tick_max_age. After a sweep every stamp and last-run tick is at most 2^30 - 1 old;
// a sweep at least every change_tick_sweep_every ticks keeps them below 2^31 old, where the signed
// difference of tick_after orders any two of them. Clamping keeps the order of every tick younger
// than the bound and merges the older ones into the bound, which no later since precedes.

#include "sl/runtime/ecs/change.hpp"
#include "sl/runtime/ecs/world.hpp"

#include <algorithm>
#include <cstdint>
#include <span>

namespace sl::runtime::ecs {

namespace {

/// `tick`, or now - change_tick_max_age when `tick` is older than that.
[[nodiscard]] std::uint32_t clamp_tick(std::uint32_t tick, std::uint32_t now) noexcept {
  return now - tick > change_tick_max_age ? now - change_tick_max_age : tick;
}

void clamp_all(std::span<std::uint32_t> ticks, std::uint32_t now) noexcept {
  std::ranges::transform(ticks, ticks.begin(),
                         [now](std::uint32_t t) noexcept { return clamp_tick(t, now); });
}

} // namespace

// The stamps are written through the archetype spans; the world is mutable on purpose.
// cppcheck-suppress constParameterReference
void sweep_ticks(world &w, std::uint32_t now) noexcept {
  for (const archetype *a : w.archetypes()) {
    for (std::uint32_t c = 0; c < a->chunk_count(); ++c) {
      if (a->rows(c) == 0) {
        break;
      }
      for (const component_id id : a->components()) {
        clamp_all(a->added_ticks(id, c), now);
        clamp_all(a->changed_ticks(id, c), now);
      }
    }
  }
}

void last_run_ticks::sweep(std::uint32_t now) noexcept {
  floor_ = clamp_tick(floor_, now);
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index): s < last_run_slots, the size
  // of both arrays.
  for (std::uint32_t s = 0; s < last_run_slots; ++s) {
    if (recorded_[s] != 0) {
      ticks_[s] = clamp_tick(ticks_[s], now);
    }
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)
}

} // namespace sl::runtime::ecs
