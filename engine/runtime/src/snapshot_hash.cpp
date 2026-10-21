// The state hash and its breakdown (WO-095, SL-ARCH-003 ARCH3-R146, `determinism`, ADR-0026):
// det::state_hasher over the entity allocator, per archetype its signature and entity column in
// row order, per component column the hashed fields in row order, the hashed resources by key and
// the contributors by name. Change ticks, transient fields and runtime-owned resources (types that
// are not registered components) are not hashed. Also the input checks shared by take, restore and
// the hash, and the contributor order.

#include "sl/runtime/snapshot.hpp"

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/replay.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sl::runtime {

namespace detail {

namespace {

void hash_array(det::state_hasher &h, const std::vector<std::uint32_t> &values) noexcept {
  h.u32(static_cast<std::uint32_t>(values.size()));
  for (const std::uint32_t v : values) {
    h.u32(v);
  }
}

[[nodiscard]] const void *row_of(const void *column, std::size_t row, std::size_t size) noexcept {
  return static_cast<const std::byte *>(column) + (row * size);
}

[[nodiscard]] hash_breakdown::part archetype_rows(std::uint32_t index,
                                                  const ecs::world_image::archetype_entry &a) {
  det::state_hasher h;
  h.str("sl-rt-hash-rows/1");
  h.u32(static_cast<std::uint32_t>(a.components.size()));
  for (const ecs::component_id id : a.components) {
    h.u16(id.value);
  }
  h.u64(std::accumulate(a.chunks.begin(), a.chunks.end(), std::uint64_t{0},
                        [](std::uint64_t n, const ecs::world_image::chunk_entry &c) {
                          return n + c.entities.size();
                        }));
  for (const ecs::world_image::chunk_entry &c : a.chunks) {
    for (const ecs::entity e : c.entities) {
      h.u32(e.index);
      h.u32(e.generation);
    }
  }
  return {.archetype = index, .component = hash_breakdown::no_component, .digest = h.finish()};
}

[[nodiscard]] hash_breakdown::part archetype_column(std::uint32_t index,
                                                    const ecs::world_image::archetype_entry &a,
                                                    std::size_t k,
                                                    const ecs::component_info &info) {
  det::state_hasher h;
  h.str("sl-rt-hash-column/1");
  for (const ecs::world_image::chunk_entry &c : a.chunks) {
    const ecs::world_image::column_entry &col = c.columns[k];
    h.u32(static_cast<std::uint32_t>(c.entities.size()));
    if (info.reflect == nullptr) {
      continue;
    }
    for (std::size_t r = 0; r < c.entities.size(); ++r) {
      hash_into(*info.reflect, row_of(col.data, r, info.size), field_flags::hashed, h);
    }
  }
  return {.archetype = index, .component = a.components[k], .digest = h.finish()};
}

} // namespace

result<void> check_inputs(const ecs::world &w, const ecs::component_registry &registry,
                          const type_registry &types) noexcept {
  if (&w.registry() != &registry) {
    return make_error(errc::invalid_argument,
                      "snapshot: the component registry is not the world's");
  }
  if (!types.frozen()) {
    return make_error(errc::invalid_argument, "snapshot: the type registry is not frozen");
  }
  for (std::uint16_t i = 0; i < registry.count(); ++i) {
    const ecs::component_info &info = registry.info(ecs::component_id{i});
    if (info.reflect == nullptr) {
      continue;
    }
    auto found = types.find(info.reflect->name);
    if (!found || *found != info.reflect) {
      return make_error(errc::invalid_argument, "snapshot: the reflected type of component " +
                                                    std::string(info.name) +
                                                    " is not in the type registry");
    }
  }
  return {};
}

template <class C> result<std::vector<C *>> sorted_contributors(std::span<C *const> contributors) {
  std::vector<C *> out(contributors.begin(), contributors.end());
  if (std::ranges::any_of(out, [](const C *c) noexcept { return c == nullptr; })) {
    return make_error(errc::invalid_argument, "snapshot: a null contributor");
  }
  std::ranges::sort(out, [](const C *a, const C *b) { return a->name() < b->name(); });
  const auto repeat = std::ranges::adjacent_find(
      out, [](const C *a, const C *b) { return a->name() == b->name(); });
  if (repeat != out.end()) {
    return make_error(errc::invalid_argument,
                      "snapshot: contributor " + std::string((*repeat)->name()) + " repeats");
  }
  return out;
}

template result<std::vector<const det::snapshot_contributor *>>
sorted_contributors<const det::snapshot_contributor>(
    std::span<const det::snapshot_contributor *const> contributors);
template result<std::vector<det::snapshot_contributor *>>
sorted_contributors<det::snapshot_contributor>(
    std::span<det::snapshot_contributor *const> contributors);

std::optional<ecs::component_id>
resource_component(const ecs::component_registry &registry,
                   const ecs::detail::resource_slot &slot) noexcept {
  if (slot.type_key == nullptr) {
    return std::nullopt;
  }
  auto id = registry.find_key(slot.type_key);
  if (!id) {
    return std::nullopt;
  }
  return *id;
}

hash_breakdown breakdown_of(const ecs::world_image &image, const ecs::component_registry &registry,
                            std::span<const det::snapshot_contributor *const> sorted) {
  hash_breakdown b;
  {
    det::state_hasher h;
    h.str("sl-rt-hash-entities/1");
    h.u32(image.entities.next_index);
    hash_array(h, image.entities.generations);
    hash_array(h, image.entities.free_fifo);
    hash_array(h, image.entities.retired);
    b.parts.push_back({.archetype = hash_breakdown::no_archetype,
                       .component = hash_breakdown::no_component,
                       .digest = h.finish()});
  }
  for (std::size_t i = 0; i < image.archetypes.size(); ++i) {
    const ecs::world_image::archetype_entry &a = image.archetypes[i];
    const auto index = static_cast<std::uint32_t>(i);
    b.parts.push_back(archetype_rows(index, a));
    for (std::size_t k = 0; k < a.components.size(); ++k) {
      b.parts.push_back(archetype_column(index, a, k, registry.info(a.components[k])));
    }
  }
  for (const ecs::detail::resource_slot &slot : image.resources) {
    const std::optional<ecs::component_id> id = resource_component(registry, slot);
    if (!id) {
      continue;
    }
    const ecs::component_info &info = registry.info(*id);
    det::state_hasher h;
    h.str("sl-rt-hash-resource/1");
    h.u32(slot.key.value);
    if (info.reflect != nullptr) {
      hash_into(*info.reflect, slot.data, field_flags::hashed, h);
    }
    b.parts.push_back(
        {.archetype = hash_breakdown::no_archetype, .component = *id, .digest = h.finish()});
  }
  det::state_hasher h;
  h.str("sl-rt-hash-contributors/1");
  h.u32(static_cast<std::uint32_t>(sorted.size()));
  for (const det::snapshot_contributor *c : sorted) {
    h.str(c->name());
    h.u32(c->version());
    c->hash(h);
  }
  b.contributors = h.finish();
  return b;
}

sl::fabric::digest hash_of(const hash_breakdown &b) noexcept {
  det::state_hasher h;
  h.str("sl-rt-state/1");
  h.u32(static_cast<std::uint32_t>(b.parts.size()));
  for (const hash_breakdown::part &p : b.parts) {
    h.u32(p.archetype);
    h.u16(p.component.value);
    h.bytes(std::as_bytes(std::span(p.digest.bytes)));
  }
  h.bytes(std::as_bytes(std::span(b.contributors.bytes)));
  return h.finish();
}

} // namespace detail

result<hash_breakdown> breakdown(const ecs::world &w, const ecs::component_registry &registry,
                                 const type_registry &types,
                                 std::span<const det::snapshot_contributor *const> contributors) {
  if (w.phase_active()) {
    return make_error(errc::snapshot_mid_tick, "snapshot: state hash inside a phase");
  }
  if (auto ok = detail::check_inputs(w, registry, types); !ok) {
    return std::unexpected(ok.error());
  }
  auto sorted = detail::sorted_contributors(contributors);
  if (!sorted) {
    return std::unexpected(sorted.error());
  }
  return detail::breakdown_of(w.capture_image(), registry, *sorted);
}

result<sl::fabric::digest>
state_hash(const ecs::world &w, const ecs::component_registry &registry, const type_registry &types,
           std::span<const det::snapshot_contributor *const> contributors) {
  auto b = breakdown(w, registry, types, contributors);
  if (!b) {
    return std::unexpected(b.error());
  }
  return detail::hash_of(*b);
}

std::optional<hash_breakdown::part> first_difference(const hash_breakdown &a,
                                                     const hash_breakdown &b) {
  const std::size_t common = std::min(a.parts.size(), b.parts.size());
  for (std::size_t i = 0; i < common; ++i) {
    if (a.parts[i] != b.parts[i]) {
      return a.parts[i];
    }
  }
  if (a.parts.size() > common) {
    return a.parts[common];
  }
  if (b.parts.size() > common) {
    return b.parts[common];
  }
  return std::nullopt;
}

} // namespace sl::runtime
