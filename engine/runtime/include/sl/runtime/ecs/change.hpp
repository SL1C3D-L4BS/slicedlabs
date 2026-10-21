#pragma once

// Change tracking (WO-093, SL-ARCH-003 ARCH3-R127 to R131, ADR-0025, ADR-0026; episode 095).
// Creation, add and set-that-adds stamp the added and changed ticks of the row (world, WO-088);
// a write<T> term stamps the changed tick of every visited row (query, WO-090); mark_changed<T>
// stamps it explicitly. changed<T>(since) and added<T>(since) keep the rows whose tick is later
// than since under the wrap rule tick_after (a signed 32-bit difference above zero), which stays
// exact while every live tick (stamps and last-run ticks) is younger than 2^31. last_run_ticks
// keeps the change tick of each system's last run: the system scheduler passes since(system) as
// iter_args::since and records the run's tick after it (ARCH3-R128). structural_log turns each
// flush_report into change signals in playback order: the archetypes created since the previous
// append (creation order), then one signal per applied spawn, destroy, add and remove; a set is a
// write and gives none (ARCH3-R129). The world version and archetype generation (world.hpp) are
// the cache invalidation values (ARCH3-R130). sweep_ticks clamps every stamp older than
// change_tick_max_age to now - change_tick_max_age; called at a boundary at least every
// change_tick_sweep_every ticks, together with last_run_ticks::sweep, it keeps every live tick
// younger than 2^31 across the u32 wrap (ARCH3-R131). Sources: src/ecs_change.cpp (mark_changed,
// last_run_ticks), src/ecs_change_log.cpp (structural_log), src/ecs_change_sweep.cpp (sweeps).
// Layer L7: includes ecs/world.hpp and ecs/command_buffer.hpp.
// Thread safety (`thread_safety`): mark_changed is worker-only (inside a system with write access
// to T; the owner thread outside phases), distinct rows from distinct threads; last_run_ticks is
// owner-thread at boundaries (distinct systems' slots never share memory); structural_log and
// the sweeps are owner-thread at boundaries.

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/command_buffer.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/error.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace sl::runtime::ecs {

// --- limits (SL-ARCH-003 `limits` change_tick_bits, change_tick_sweep_every) ---

/// The sweep period in change ticks (`limits` change_tick_sweep_every, 2^30).
inline constexpr std::uint32_t change_tick_sweep_every = 1073741824U;
/// The age a sweep clamps to: 2^30 - 1, so every live tick stays younger than 2^31 until the
/// next sweep, at most change_tick_sweep_every ticks later.
inline constexpr std::uint32_t change_tick_max_age = change_tick_sweep_every - 1U;
/// The slots of last_run_ticks: 8 phases x 256 systems per phase (`limits`
/// max_systems_per_phase).
inline constexpr std::uint32_t last_run_slots = 2048;

/// True when `tick` is later than `since` under the wrap rule (the compare of WO-090's
/// changed<T> and added<T> filters, query.hpp tick_later).
[[nodiscard]] constexpr bool tick_after(std::uint32_t tick, std::uint32_t since) noexcept {
  return static_cast<std::int32_t>(tick - since) > 0;
}

/// True when a sweep is due: `now` is change_tick_sweep_every or more ticks after `last_sweep`.
[[nodiscard]] constexpr bool sweep_due(std::uint32_t last_sweep, std::uint32_t now) noexcept {
  return now - last_sweep >= change_tick_sweep_every;
}

/// The last_run_ticks slot of a system: phase index x 256 + system_index.
[[nodiscard]] constexpr std::uint32_t last_run_key(std::uint8_t phase,
                                                   std::uint16_t system_index) noexcept {
  return (std::uint32_t{phase} * 256U) + std::uint32_t{system_index};
}

// --- mark_changed ---

/// Stamps the changed tick of component `id` of `e` with `tick`: stale_entity, component_unknown,
/// component_missing (world::get_raw's codes).
[[nodiscard]] result<void> mark_changed_raw(world &w, entity e, component_id id,
                                            std::uint32_t tick) noexcept;

/// Stamps the changed tick of component T of `e` with `tick` (worker-only, inside a system with
/// write access to T).
template <class T>
[[nodiscard]] result<void> mark_changed(world &w, entity e, std::uint32_t tick) noexcept {
  static_assert(component_type<std::remove_cvref_t<T>>);
  const auto id = w.registry().id_of<std::remove_cvref_t<T>>();
  if (!id) {
    return std::unexpected(id.error());
  }
  return mark_changed_raw(w, e, *id, tick);
}

// --- last-run ticks ---

/// The change tick of each system's last run (ARCH3-R128). A system never recorded (or a key at
/// or above last_run_slots) reads floor(): initially 0xFFFFFFFF, one before tick 0, so a first run
/// sees every stamp. Owner thread at boundaries.
class last_run_ticks {
public:
  /// One recorded system.
  struct entry {
    std::uint32_t system = 0;
    std::uint32_t tick = 0;

    friend constexpr bool operator==(const entry &, const entry &) noexcept = default;
  };

  /// The tick of `system`'s last run, or floor() when it never ran.
  [[nodiscard]] std::uint32_t since(std::uint32_t system) const noexcept;
  /// Records that `system` ran at change tick `tick`; a key at or above last_run_slots is
  /// ignored.
  void record(std::uint32_t system, std::uint32_t tick) noexcept;
  /// Replaces every slot: the systems of `saved` (keys below last_run_slots) get their ticks,
  /// every other system reads `saved_floor`.
  void restore(std::span<const entry> saved, std::uint32_t saved_floor) noexcept;
  /// The recorded systems in ascending key order (for snapshots; restore takes them back).
  [[nodiscard]] std::vector<entry> entries() const noexcept;
  /// The tick a never-run system reads.
  [[nodiscard]] std::uint32_t floor() const noexcept { return floor_; }
  /// Clamps every recorded tick and the floor older than change_tick_max_age to
  /// now - change_tick_max_age (with sweep_ticks, at the same boundary).
  void sweep(std::uint32_t now) noexcept;

private:
  std::array<std::uint32_t, last_run_slots> ticks_{};
  std::array<std::uint8_t, last_run_slots> recorded_{};
  std::uint32_t floor_ = 0xFFFFFFFFU;
};

// --- structural signals ---

enum class change_signal_kind : std::uint8_t {
  archetype_created,
  entity_created,
  entity_destroyed,
  component_added,
  component_removed
};

/// One structural change. archetype_created: target null, component_id{}, archetype = its
/// creation index. The others: the entity, the component (component_id{} for entity_created and
/// entity_destroyed) and the creation index of the entity's archetype at the boundary, or
/// no_archetype when it is no longer alive then.
struct change_signal {
  change_signal_kind kind = change_signal_kind::archetype_created;
  entity target;
  component_id component;
  std::uint32_t archetype = no_archetype;

  friend constexpr bool operator==(const change_signal &, const change_signal &) noexcept = default;
};

/// The structural signals of one boundary, built from each flush_report in playback order
/// (ARCH3-R129). Owner thread at boundaries; the signals live until clear().
class structural_log {
public:
  /// Appends the signals of `report`, the flush of `w` that just played: first archetype_created
  /// for every archetype of `w` not yet announced (creation order; every archetype again for
  /// another world or after restore_from), then one signal per applied spawn, destroy, add and
  /// remove in report order (a set gives none).
  void append(const flush_report &report, const world &w) noexcept;
  /// The signals appended since the last clear(), in playback order.
  [[nodiscard]] std::span<const change_signal> signals() const noexcept {
    return {signals_.data(), signals_.size()};
  }
  /// Drops the signals (the next boundary); the announced archetypes stay announced.
  void clear() noexcept { signals_.clear(); }

private:
  /// Fills where_ with the archetype of every live row of `w`.
  void locate(const world &w) noexcept;

  std::vector<change_signal> signals_;
  std::vector<std::uint32_t> where_; // entity index -> archetype creation index
  const world *seen_ = nullptr;      // compared, never dereferenced
  std::uint64_t generation_ = 0;
  std::uint32_t announced_ = 0;
};

// --- the tick-wrap sweep ---

/// Owner thread, boundary: clamps every added and changed tick of `w` older than
/// change_tick_max_age to now - change_tick_max_age (ARCH3-R131).
void sweep_ticks(world &w, std::uint32_t now) noexcept;

} // namespace sl::runtime::ecs
