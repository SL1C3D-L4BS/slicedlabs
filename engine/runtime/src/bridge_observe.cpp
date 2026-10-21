// The observe system of the link (WO-097, SL-ARCH-003 ARCH3-R154, R160; ADR-0027, ADR-0026).

#include "sl/runtime/bridge.hpp"

#include "sl/fabric/boundary.hpp"
#include "sl/fabric/value.hpp"
#include "sl/runtime/ecs/change.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/replay.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace sl::runtime::link::detail {

namespace fab = sl::fabric;

namespace {

// What one observed component writes: its binding, its buffer and the next key.
struct observe_target {
  const binding_registry::entry *bound = nullptr;
  outbox::buffer *out = nullptr;
  det::ordered_key key;
  std::uint32_t stride = 0;
};

// Pushes the bound fields of the row at `row_value` (the component of `record`) that differ from
// the current projection's.
void observe_row(link_state &s, observe_target &t, const fab::record_id &record,
                 const void *row_value) noexcept {
  auto now = t.bound->binding.to_value(row_value);
  if (!now) {
    return;
  }
  const auto reference = s.projected.find(record);
  for (const fabric_binding::bound_field &f : t.bound->binding.fields()) {
    const fab::value *v = now->find_field(f.fabric_field);
    if (v == nullptr) {
      continue;
    }
    if (reference != s.projected.end()) {
      const fab::value *was = reference->second.find_field(f.fabric_field);
      if (was != nullptr && *was == *v) {
        ++s.stats.echo_suppressed;
        continue;
      }
    }
    t.out->push(t.key, fab::observed_change{.record = record,
                                            .path = t.bound->binding.path_of(f.fabric_field),
                                            .value = *v});
    ++t.key.local_seq;
  }
}

void observe_component(link_state &s, observe_target &t, ecs::component_id component,
                       std::uint32_t since) noexcept {
  for (const ecs::archetype *a : s.world->archetypes()) {
    if (!a->contains(component)) {
      continue;
    }
    for (std::uint32_t chunk = 0; chunk < a->chunk_count(); ++chunk) {
      const std::uint32_t rows = a->rows(chunk);
      if (rows == 0) {
        continue;
      }
      const std::span<const std::uint32_t> ticks = a->changed_ticks(component, chunk);
      const std::span<const ecs::entity> entities = a->entities(chunk);
      const auto *column = static_cast<const std::byte *>(a->column(component, chunk));
      for (std::uint32_t row = 0; row < rows; ++row) {
        if (!ecs::tick_after(ticks[row], since)) {
          continue;
        }
        // Only ingested records are observed: an entity the runtime spawned has no record.
        auto record = s.entities.key_of(entities[row]);
        if (!record) {
          continue;
        }
        observe_row(s, t, *record, column + (std::size_t{row} * t.stride));
      }
    }
  }
}

} // namespace

void observe(link_state &s, ecs::system_context &ctx) noexcept {
  for (std::size_t i = 0; i < s.observed.size(); ++i) {
    const ecs::component_id component = s.observed[i];
    observe_target t{.bound = s.bindings->find_component(component),
                     .out = &s.out.buffer_at(i),
                     .key = det::ordered_key{.tick = ctx.tick(),
                                             .phase = static_cast<std::uint8_t>(phase::observe),
                                             .system_index = ctx.system_index(),
                                             .chunk_index = static_cast<std::uint32_t>(i),
                                             .local_seq = 0},
                     .stride = s.world->registry().info(component).size};
    if (t.bound != nullptr) {
      observe_component(s, t, component, ctx.args.since);
    }
  }
}

} // namespace sl::runtime::link::detail
