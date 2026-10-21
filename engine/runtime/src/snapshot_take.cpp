// snapshot take (WO-095, SL-ARCH-003 ARCH3-R143, R144, ADR-0026): at end_frame, the SLRS/1 bytes of
// the world's image (allocator, archetypes in creation order with every non-empty chunk, the
// encoded snapshot fields and both tick columns, the resources whose type is a registered
// component), the contributors by name, the meta digests, the state hash and the trailer.

#include "sl/runtime/snapshot.hpp"

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/replay.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace sl::runtime {

namespace {

void write_array(detail::snapshot_writer &out, const std::vector<std::uint32_t> &values) {
  out.u32(static_cast<std::uint32_t>(values.size()));
  for (const std::uint32_t v : values) {
    out.u32(v);
  }
}

/// The encoded snapshot fields of `rows` values of `info` at `data`.
void write_values(detail::snapshot_writer &out, const ecs::component_info &info, const void *data,
                  std::size_t rows) {
  std::vector<std::byte> encoded;
  if (info.reflect != nullptr) {
    encoded.reserve(encoded_size(*info.reflect, field_flags::snapshot) * rows);
    for (std::size_t r = 0; r < rows; ++r) {
      const void *value = static_cast<const std::byte *>(data) + (r * info.size);
      encode(*info.reflect, value, field_flags::snapshot, encoded);
    }
  }
  out.u32(static_cast<std::uint32_t>(encoded.size()));
  out.raw(encoded);
}

[[nodiscard]] result<void> check_rebuildable(const ecs::world_image &image,
                                             const ecs::component_registry &registry) {
  for (const ecs::world_image::archetype_entry &a : image.archetypes) {
    if (a.chunks.empty()) {
      continue;
    }
    for (const ecs::component_id id : a.components) {
      const ecs::component_info &info = registry.info(id);
      if (!info.trivially_relocatable) {
        return make_error(errc::invalid_argument, "snapshot::take: component " +
                                                      std::string(info.name) +
                                                      " has values but is not trivially copyable");
      }
    }
  }
  for (const ecs::detail::resource_slot &slot : image.resources) {
    const std::optional<ecs::component_id> id = detail::resource_component(registry, slot);
    if (id && !registry.info(*id).trivially_relocatable) {
      return make_error(errc::invalid_argument, "snapshot::take: the resource of key " +
                                                    std::to_string(slot.key.value) +
                                                    " is not trivially copyable");
    }
  }
  return {};
}

void write_world(detail::snapshot_writer &out, const ecs::world_image &image,
                 const ecs::component_registry &registry) {
  out.u32(image.entities.next_index);
  write_array(out, image.entities.generations);
  write_array(out, image.entities.free_fifo);
  write_array(out, image.entities.retired);
  out.u32(image.change_tick);
  out.u32(static_cast<std::uint32_t>(image.archetypes.size()));
  for (const ecs::world_image::archetype_entry &a : image.archetypes) {
    out.u32(static_cast<std::uint32_t>(a.components.size()));
    for (const ecs::component_id id : a.components) {
      out.u16(id.value);
    }
    out.u32(static_cast<std::uint32_t>(a.chunks.size()));
    for (const ecs::world_image::chunk_entry &c : a.chunks) {
      out.u32(static_cast<std::uint32_t>(c.entities.size()));
      for (const ecs::entity e : c.entities) {
        out.u32(e.index);
        out.u32(e.generation);
      }
      for (const ecs::world_image::column_entry &col : c.columns) {
        write_values(out, registry.info(col.id), col.data, c.entities.size());
      }
      for (const ecs::world_image::column_entry &col : c.columns) {
        for (const std::uint32_t t : col.added) {
          out.u32(t);
        }
        for (const std::uint32_t t : col.changed) {
          out.u32(t);
        }
      }
    }
  }
  std::vector<std::pair<const ecs::detail::resource_slot *, ecs::component_id>> kept;
  for (const ecs::detail::resource_slot &slot : image.resources) {
    if (const std::optional<ecs::component_id> id = detail::resource_component(registry, slot)) {
      kept.emplace_back(&slot, *id);
    }
  }
  out.u32(static_cast<std::uint32_t>(kept.size()));
  for (const auto &[slot, id] : kept) {
    out.u32(slot->key.value);
    out.u16(id.value);
    write_values(out, registry.info(id), slot->data, 1);
  }
}

} // namespace

result<snapshot> take(const ecs::world &w, const ecs::component_registry &registry,
                      const type_registry &types,
                      std::span<const det::snapshot_contributor *const> contributors,
                      const snapshot_meta &meta) {
  if (w.phase_active()) {
    return make_error(errc::snapshot_mid_tick, "snapshot::take inside a phase");
  }
  if (auto ok = detail::check_inputs(w, registry, types); !ok) {
    return std::unexpected(ok.error());
  }
  auto sorted = detail::sorted_contributors(contributors);
  if (!sorted) {
    return std::unexpected(sorted.error());
  }
  const ecs::world_image image = w.capture_image();
  if (auto ok = check_rebuildable(image, registry); !ok) {
    return std::unexpected(ok.error());
  }

  detail::snapshot_writer out;
  for (const char c : std::string_view("SLRS")) {
    out.u8(static_cast<std::uint8_t>(c));
  }
  out.u8(snapshot_major);
  out.u8(snapshot_minor);
  out.u64(meta.frame);
  out.u64(meta.tick);
  out.digest(meta.runtime_abi_digest);
  out.digest(meta.schedule_digest);
  out.u32(registry.count());
  for (std::uint16_t i = 0; i < registry.count(); ++i) {
    const ecs::component_info &info = registry.info(ecs::component_id{i});
    out.name(info.name);
    out.digest(detail::type_digest(info));
  }
  out.u32(static_cast<std::uint32_t>(sorted->size()));
  for (const det::snapshot_contributor *c : *sorted) {
    std::vector<std::byte> captured;
    if (auto ok = c->capture(captured); !ok) {
      return std::unexpected(ok.error());
    }
    out.name(c->name());
    out.u32(c->version());
    out.u64(captured.size());
    out.raw(captured);
  }
  write_world(out, image, registry);
  out.digest(detail::hash_of(detail::breakdown_of(image, registry, *sorted)));
  out.digest(detail::sha256_of(out.bytes()));
  return snapshot::from_bytes(std::move(out.bytes()));
}

} // namespace sl::runtime
