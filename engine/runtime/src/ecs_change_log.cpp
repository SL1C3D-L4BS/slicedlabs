// The structural log (WO-093, SL-ARCH-003 ARCH3-R129, ADR-0026; `determinism` merge point
// "structural signals (from flush_report order)"). append announces the archetypes the log has
// not announced for this world, in creation order: it remembers the world's address (compared,
// never dereferenced), the archetype generation and the count announced, and announces every
// archetype again for another world or when the generation moved by more than the archetypes
// added (restore_from). Then it maps each applied entry of the report, in report order, to one
// signal: spawn -> entity_created, destroy -> entity_destroyed, add -> component_added,
// remove -> component_removed; a set gives component_added when it added the missing component
// (op::added, WO-109) and none otherwise. The archetype of an entity signal is the one the
// entity is in at the boundary (located by one pass over every live row), no_archetype when the
// entity is dead then: flush_report does not say which archetype a row passed through.

#include "sl/runtime/ecs/change.hpp"
#include "sl/runtime/ecs/command_buffer.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"

#include <cstdint>
#include <optional>

namespace sl::runtime::ecs {

namespace {

using op_kind = flush_report::op_kind;

/// The signal kind of an applied entry; none for a set that wrote an existing component.
[[nodiscard]] std::optional<change_signal_kind> kind_of(const flush_report::op &op) noexcept {
  switch (op.kind) {
  case op_kind::spawn:
    return change_signal_kind::entity_created;
  case op_kind::destroy:
    return change_signal_kind::entity_destroyed;
  case op_kind::add:
    return change_signal_kind::component_added;
  case op_kind::remove:
    return change_signal_kind::component_removed;
  case op_kind::set:
    if (op.added) {
      return change_signal_kind::component_added;
    }
    break;
  }
  return std::nullopt;
}

} // namespace

void structural_log::append(const flush_report &report, const world &w) noexcept {
  const auto count = static_cast<std::uint32_t>(w.archetypes().size());
  const std::uint64_t generation = w.archetype_generation();
  const bool appended = seen_ == &w && count >= announced_ && generation >= generation_ &&
                        generation - generation_ == count - announced_;
  const std::uint32_t first = appended ? announced_ : 0;
  for (std::uint32_t i = first; i < count; ++i) {
    signals_.push_back(change_signal{.kind = change_signal_kind::archetype_created,
                                     .target = entity{},
                                     .component = component_id{},
                                     .archetype = i});
  }
  seen_ = &w;
  generation_ = generation;
  announced_ = count;
  if (report.applied.empty()) {
    return;
  }
  locate(w);
  for (const flush_report::op &op : report.applied) {
    const std::optional<change_signal_kind> kind = kind_of(op);
    if (!kind) {
      continue;
    }
    const bool alive = w.is_alive(op.target) && op.target.index < where_.size();
    signals_.push_back(change_signal{.kind = *kind,
                                     .target = op.target,
                                     .component = op.component,
                                     .archetype = alive ? where_[op.target.index] : no_archetype});
  }
}

void structural_log::locate(const world &w) noexcept {
  where_.assign(w.entities().next_index(), no_archetype);
  for (const archetype *a : w.archetypes()) {
    for (std::uint32_t c = 0; c < a->chunk_count(); ++c) {
      if (a->rows(c) == 0) {
        break;
      }
      for (const entity e : a->entities(c)) {
        if (e.index < where_.size()) {
          where_[e.index] = a->creation_index();
        }
      }
    }
  }
}

} // namespace sl::runtime::ecs
