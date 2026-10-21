// snapshot restore (WO-095, SL-ARCH-003 ARCH3-R143 to R145, ADR-0026). Every check runs before any
// change: the phase, the inputs, the trailer and structure, the runtime_abi_digest, the type
// digests and the recorded contributors. The values are then decoded into staging buffers (each
// value starts as zero bytes; the snapshot fields are decoded into it), the contributors restore
// with their own captures kept for a rollback, and world::restore_from swaps the whole world in.
// Resources whose type is not a registered component (runtime-owned, such as event channels) stay:
// their slots pass through the image and restore_from moves them into its new blocks.

#include "sl/runtime/snapshot.hpp"

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/replay.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace sl::runtime {

namespace {

[[nodiscard]] std::unexpected<error> corrupt(const std::string &what) {
  return make_error(errc::snapshot_corrupt, "snapshot::restore: " + what);
}

/// Values of one component: `count` zeroed values of `info` in aligned storage, with the
/// snapshot fields of `encoded` decoded into them.
class value_block {
public:
  [[nodiscard]] bool fill(const ecs::component_info &info, std::span<const std::byte> encoded,
                          std::size_t count) {
    const std::size_t per =
        info.reflect == nullptr ? 0 : encoded_size(*info.reflect, field_flags::snapshot);
    if (encoded.size() != per * count) {
      return false;
    }
    const std::size_t bytes = std::size_t{info.size} * count;
    storage_.assign(bytes + info.align, std::byte{0});
    void *at = storage_.data();
    std::size_t space = storage_.size();
    data_ = std::align(info.align, bytes, at, space);
    if (data_ == nullptr || info.reflect == nullptr) {
      return data_ != nullptr;
    }
    for (std::size_t r = 0; r < count; ++r) {
      void *value = static_cast<std::byte *>(data_) + (r * info.size);
      if (!decode(*info.reflect, encoded.subspan(r * per, per), field_flags::snapshot, value)) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] void *data() const noexcept { return data_; }

private:
  std::vector<std::byte> storage_;
  void *data_ = nullptr;
};

[[nodiscard]] bool read_u32s(std::span<const std::byte> in, std::vector<std::uint32_t> &out) {
  detail::snapshot_reader r(in);
  out.resize(in.size() / 4);
  return std::ranges::all_of(out, [&r](std::uint32_t &v) noexcept { return r.u32(v); });
}

/// The decoded world of a parsed snapshot and the image restore_from reads.
struct staged_world {
  struct chunk {
    std::vector<ecs::entity> entities;
    std::vector<value_block> values;
    std::vector<std::vector<std::uint32_t>> added;
    std::vector<std::vector<std::uint32_t>> changed;
  };
  std::vector<std::vector<chunk>> archetypes;
  std::vector<value_block> resources;
  ecs::world_image image;
};

[[nodiscard]] result<void> stage_chunk(const detail::parsed_snapshot::archetype_entry &a,
                                       const detail::parsed_snapshot::chunk_entry &in,
                                       const ecs::component_registry &registry,
                                       staged_world::chunk &out) {
  detail::snapshot_reader r(in.entities);
  out.entities.resize(in.rows);
  for (ecs::entity &e : out.entities) {
    if (!r.u32(e.index) || !r.u32(e.generation)) {
      return corrupt("a malformed entity column");
    }
  }
  const std::size_t columns = a.components.size();
  out.values.resize(columns);
  out.added.resize(columns);
  out.changed.resize(columns);
  for (std::size_t k = 0; k < columns; ++k) {
    const ecs::component_info &info = registry.info(a.components[k]);
    if (!info.trivially_relocatable) {
      return corrupt("values of " + std::string(info.name) + ", which is not trivially copyable");
    }
    if (!out.values[k].fill(info, in.values[k], in.rows)) {
      return corrupt("values of " + std::string(info.name) + " that do not decode");
    }
    if (!read_u32s(in.added[k], out.added[k]) || !read_u32s(in.changed[k], out.changed[k])) {
      return corrupt("a malformed tick column");
    }
  }
  return {};
}

[[nodiscard]] result<void> stage_archetypes(const detail::parsed_snapshot &p,
                                            const ecs::component_registry &registry,
                                            staged_world &s) {
  s.archetypes.resize(p.archetypes.size());
  s.image.archetypes.resize(p.archetypes.size());
  for (std::size_t i = 0; i < p.archetypes.size(); ++i) {
    const detail::parsed_snapshot::archetype_entry &a = p.archetypes[i];
    if (!std::ranges::all_of(a.components,
                             [&registry](ecs::component_id id) { return registry.contains(id); })) {
      return corrupt("an archetype names an unregistered component");
    }
    s.archetypes[i].resize(a.chunks.size());
    ecs::world_image::archetype_entry &img = s.image.archetypes[i];
    img.components = a.components;
    img.chunks.resize(a.chunks.size());
    for (std::size_t c = 0; c < a.chunks.size(); ++c) {
      staged_world::chunk &chunk = s.archetypes[i][c];
      if (auto ok = stage_chunk(a, a.chunks[c], registry, chunk); !ok) {
        return ok;
      }
      img.chunks[c].entities = chunk.entities;
      img.chunks[c].columns.resize(a.components.size());
      for (std::size_t k = 0; k < a.components.size(); ++k) {
        img.chunks[c].columns[k] = {.id = a.components[k],
                                    .data = chunk.values[k].data(),
                                    .added = chunk.added[k],
                                    .changed = chunk.changed[k]};
      }
    }
  }
  return {};
}

/// The snapshot's resources merged by key with the world's runtime-owned ones (the snapshot's win
/// on a shared key).
[[nodiscard]] result<void> stage_resources(const detail::parsed_snapshot &p, const ecs::world &w,
                                           const ecs::component_registry &registry,
                                           staged_world &s) {
  std::vector<ecs::detail::resource_slot> from_snapshot;
  s.resources.resize(p.resources.size());
  for (std::size_t k = 0; k < p.resources.size(); ++k) {
    const detail::parsed_snapshot::resource_entry &e = p.resources[k];
    if (!registry.contains(e.type) || (k != 0 && !(p.resources[k - 1].key < e.key))) {
      return corrupt("a malformed resource section");
    }
    const ecs::component_info &info = registry.info(e.type);
    if (info.type_key == nullptr || !info.trivially_relocatable ||
        !s.resources[k].fill(info, e.values, 1)) {
      return corrupt("resource " + std::to_string(e.key.value) + " does not decode");
    }
    from_snapshot.push_back({.key = e.key,
                             .type_key = info.type_key,
                             .data = s.resources[k].data(),
                             .size = info.size,
                             .align = info.align,
                             .move = info.move,
                             .destroy = info.destroy});
  }
  std::vector<ecs::detail::resource_slot> owned;
  const ecs::world_image current = w.capture_image();
  for (const ecs::detail::resource_slot &slot : current.resources) {
    const bool shadowed = std::ranges::binary_search(from_snapshot, slot.key, std::less<>{},
                                                     &ecs::detail::resource_slot::key);
    if (!shadowed && !detail::resource_component(registry, slot)) {
      owned.push_back(slot);
    }
  }
  s.image.resources.reserve(from_snapshot.size() + owned.size());
  std::ranges::merge(from_snapshot, owned, std::back_inserter(s.image.resources), std::less<>{},
                     &ecs::detail::resource_slot::key, &ecs::detail::resource_slot::key);
  return {};
}

[[nodiscard]] result<void> check_types(const detail::parsed_snapshot &p,
                                       const ecs::component_registry &registry) {
  if (p.types.size() != registry.count()) {
    return make_error(errc::snapshot_schema_mismatch,
                      "snapshot::restore: " + std::to_string(p.types.size()) +
                          " component types recorded, " + std::to_string(registry.count()) +
                          " registered");
  }
  for (std::uint16_t i = 0; i < registry.count(); ++i) {
    const ecs::component_info &info = registry.info(ecs::component_id{i});
    if (p.types[i].name != info.name || p.types[i].digest != detail::type_digest(info)) {
      return make_error(errc::snapshot_schema_mismatch,
                        "snapshot::restore: component " + std::to_string(i) + " (" +
                            std::string(info.name) + ") differs from the recorded " +
                            std::string(p.types[i].name));
    }
  }
  return {};
}

/// The registered contributor of each recorded one.
[[nodiscard]] result<std::vector<det::snapshot_contributor *>>
match_contributors(const detail::parsed_snapshot &p,
                   const std::vector<det::snapshot_contributor *> &sorted) {
  std::vector<det::snapshot_contributor *> out;
  out.reserve(p.contributors.size());
  for (const detail::parsed_snapshot::contributor_entry &c : p.contributors) {
    const auto it = std::ranges::find_if(sorted, [&c](const det::snapshot_contributor *x) {
      return x->name() == c.name && x->version() == c.version;
    });
    if (it == sorted.end()) {
      return make_error(errc::snapshot_contributor_unknown,
                        "snapshot::restore: contributor " + std::string(c.name) + " version " +
                            std::to_string(c.version) + " is not registered");
    }
    out.push_back(*it);
  }
  return out;
}

/// Restores the first `count` contributors from their backups.
[[nodiscard]] result<void> roll_back(std::span<det::snapshot_contributor *const> matched,
                                     const std::vector<std::vector<std::byte>> &backups,
                                     std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    if (auto ok = matched[i]->restore(backups[i]); !ok) {
      return make_error(errc::internal, "snapshot::restore: contributor " +
                                            std::string(matched[i]->name()) +
                                            " did not take back its own capture");
    }
  }
  return {};
}

} // namespace

result<void> restore(ecs::world &w, const ecs::component_registry &registry,
                     const type_registry &types,
                     std::span<det::snapshot_contributor *const> contributors, const snapshot &s,
                     const sl::fabric::digest &expected_abi) {
  if (w.phase_active()) {
    return make_error(errc::snapshot_mid_tick, "snapshot::restore inside a phase");
  }
  if (auto ok = detail::check_inputs(w, registry, types); !ok) {
    return ok;
  }
  auto sorted = detail::sorted_contributors(contributors);
  if (!sorted) {
    return std::unexpected(sorted.error());
  }
  auto parsed = detail::parse_snapshot(s.bytes());
  if (!parsed) {
    return std::unexpected(parsed.error());
  }
  if (parsed->abi != expected_abi) {
    return make_error(errc::snapshot_abi_mismatch,
                      "snapshot::restore: the runtime_abi_digest differs (recorded " +
                          parsed->abi.to_hex() + ", expected " + expected_abi.to_hex() + ")");
  }
  if (auto ok = check_types(*parsed, registry); !ok) {
    return ok;
  }
  auto matched = match_contributors(*parsed, *sorted);
  if (!matched) {
    return std::unexpected(matched.error());
  }

  staged_world staged;
  staged.image.entities = parsed->entities;
  staged.image.change_tick = parsed->change_tick;
  if (auto ok = stage_archetypes(*parsed, registry, staged); !ok) {
    return ok;
  }
  if (auto ok = stage_resources(*parsed, w, registry, staged); !ok) {
    return ok;
  }
  std::vector<std::vector<std::byte>> backups(matched->size());
  for (std::size_t i = 0; i < matched->size(); ++i) {
    if (auto ok = (*matched)[i]->capture(backups[i]); !ok) {
      return ok;
    }
  }

  // changes from here on, each undone when a later step fails
  for (std::size_t i = 0; i < matched->size(); ++i) {
    if (auto ok = (*matched)[i]->restore(parsed->contributors[i].bytes); !ok) {
      if (auto undone = roll_back(*matched, backups, i); !undone) {
        return undone;
      }
      return ok;
    }
  }
  if (auto ok = w.restore_from(staged.image); !ok) {
    if (auto undone = roll_back(*matched, backups, matched->size()); !undone) {
      return undone;
    }
    if (ok.error().code == errc::invalid_argument) {
      return corrupt("the world refused the image: " + ok.error().message);
    }
    return ok;
  }
  return {};
}

} // namespace sl::runtime
