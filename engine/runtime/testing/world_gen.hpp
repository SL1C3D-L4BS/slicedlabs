#pragma once

// testing::world_gen (WO-088, SL-ARCH-003 ARCH3-R107 to R111, RT-INV-STORE-01 to -07): eight test
// components (gen_padded has interior padding, gen_large is 2048 bytes, gen_tag is empty,
// gen_tracked is not trivially relocatable and counts its live instances, gen_aligned is 64-byte
// aligned), a seeded operation generator (create, destroy, add, remove, set) and an oracle: a
// std::map<entity, values> plus an independent model of the archetype creation order and the
// swap-remove row order, with compare(world) listing every difference. hash_world hashes the
// allocator state and every component value field by field in iteration order (ticks are not
// hashed, `determinism`). Owner-thread (`thread_safety`, testing::*). Include it by path:
// "../../../engine/runtime/testing/world_gen.hpp".

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/replay.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sl::runtime::testing {

/// Counts the gen_tracked instances made and ended.
struct live_counter {
  std::int64_t constructed = 0;
  std::int64_t destroyed = 0;

  [[nodiscard]] std::int64_t live() const noexcept { return constructed - destroyed; }
};

struct gen_position {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
};

struct gen_velocity {
  double dx = 0.0;
  double dy = 0.0;
};

/// Interior padding after a and tail padding after c.
struct gen_padded {
  std::uint8_t a = 0;
  std::uint64_t b = 0;
  std::uint16_t c = 0;
};

/// The largest component (max_component_size).
struct gen_large {
  std::array<std::uint64_t, 256> words{};
};

/// An empty tag.
struct gen_tag {};

/// Not trivially relocatable: it remembers its own address, so a byte copy is detected, and it
/// counts every construction and destruction in a live_counter.
class gen_tracked {
public:
  gen_tracked(live_counter &counter, std::uint64_t value) noexcept
      : counter_(&counter), value_(value), self_(this) {
    ++counter_->constructed;
  }
  gen_tracked(const gen_tracked &other) noexcept
      : counter_(other.counter_), value_(other.value_), self_(this) {
    ++counter_->constructed;
  }
  gen_tracked(gen_tracked &&other) noexcept
      : counter_(other.counter_), value_(other.value_), self_(this) {
    ++counter_->constructed;
  }
  gen_tracked &operator=(const gen_tracked &) = delete;
  gen_tracked &operator=(gen_tracked &&) = delete;
  ~gen_tracked() { ++counter_->destroyed; }

  [[nodiscard]] std::uint64_t value() const noexcept { return value_; }
  /// False when the value was moved by a byte copy.
  [[nodiscard]] bool intact() const noexcept { return self_ == this; }

private:
  live_counter *counter_;
  std::uint64_t value_;
  const gen_tracked *self_;
};

struct alignas(64) gen_aligned {
  std::uint32_t value = 0;
};

/// Tail padding after armor.
struct gen_health {
  std::int32_t hp = 0;
  std::int16_t armor = 0;
};

inline constexpr std::uint32_t gen_component_count = 8;

/// The ids of the eight components, by kind (0 position .. 7 health).
struct gen_ids {
  std::array<ecs::component_id, gen_component_count> ids{};

  /// The kind of `id`, or gen_component_count.
  [[nodiscard]] std::uint32_t kind_of(ecs::component_id id) const noexcept;
};

/// Registers the eight components as gen.position, gen.velocity, gen.padded, gen.large, gen.tag,
/// gen.tracked, gen.aligned and gen.health, in that order.
[[nodiscard]] result<gen_ids> register_components(ecs::component_registry &registry);

/// One component value of kind k made from `v` (the counter is used by gen_tracked).
class value_holder {
public:
  value_holder(std::uint32_t kind, std::uint64_t v, live_counter &counter) noexcept;
  value_holder(const value_holder &) = delete;
  value_holder(value_holder &&) = delete;
  value_holder &operator=(const value_holder &) = delete;
  value_holder &operator=(value_holder &&) = delete;
  ~value_holder();

  [[nodiscard]] void *get() noexcept { return storage_.data(); }

private:
  alignas(64) std::array<std::byte, 2048> storage_{};
  std::uint32_t kind_;
};

/// True when the value of kind k at `p` equals the value made from `v` (and gen_tracked is
/// intact).
[[nodiscard]] bool value_equals(std::uint32_t kind, const void *p, std::uint64_t v) noexcept;
/// Hashes the value of kind k at `p` field by field (no padding).
void hash_value(std::uint32_t kind, const void *p, det::state_hasher &h) noexcept;

enum class op_kind : std::uint8_t { create, destroy, add, remove, set };

struct world_op {
  op_kind kind = op_kind::create;
  std::uint64_t pick = 0;      // which live entity (modulo the population, in handle order)
  std::uint32_t component = 0; // the kind for add, remove and set
  std::uint64_t value = 0;
  std::uint32_t with = 0; // create: bit k set gives the new entity component k
  bool document = false;  // set: a document write
};

/// Seeded operations: create 25 %, destroy 15 %, add 25 %, remove 20 %, set 15 % (of the
/// components in `mask`).
class op_generator {
public:
  explicit op_generator(std::uint64_t seed, std::uint32_t mask = 0xFFU) noexcept;
  [[nodiscard]] world_op next() noexcept;

private:
  [[nodiscard]] std::uint32_t pick_component() noexcept;

  det::rng_stream rng_;
  std::uint32_t mask_;
};

struct entity_values {
  std::array<std::optional<std::uint64_t>, gen_component_count> values{};
};

/// The expected world. Neither copyable nor movable: gen_tracked values point at its counter.
class world_oracle {
public:
  explicit world_oracle(const gen_ids &ids) noexcept : ids_(ids) {}
  world_oracle(const world_oracle &) = delete;
  world_oracle(world_oracle &&) = delete;
  world_oracle &operator=(const world_oracle &) = delete;
  world_oracle &operator=(world_oracle &&) = delete;
  ~world_oracle() = default;

  /// Every difference between `w` and the oracle: liveness, components, values, the archetype
  /// creation order, the row order (archetype, chunk, row), chunk density and the live
  /// gen_tracked count. Empty when they agree.
  [[nodiscard]] std::vector<std::string> compare(const ecs::world &w) const;

  void on_create(ecs::entity e, const entity_values &v);
  void on_destroy(ecs::entity e);
  void on_add(ecs::entity e, std::uint32_t kind, std::uint64_t v);
  void on_remove(ecs::entity e, std::uint32_t kind);
  void on_set(ecs::entity e, std::uint32_t kind, std::uint64_t v);

  /// The live entity `pick` selects (modulo the population, in handle order); null when empty.
  [[nodiscard]] ecs::entity choose(std::uint64_t pick) const;
  [[nodiscard]] const gen_ids &ids() const noexcept { return ids_; }
  [[nodiscard]] live_counter &counter() noexcept { return counter_; }
  [[nodiscard]] const std::map<ecs::entity, entity_values> &entities() const noexcept {
    return entities_;
  }
  /// The modelled archetypes: signature (ascending ids) and rows, in creation order.
  struct model_archetype {
    std::vector<ecs::component_id> components;
    std::vector<ecs::entity> rows;
  };
  [[nodiscard]] const std::vector<model_archetype> &order() const noexcept { return order_; }

private:
  [[nodiscard]] std::vector<ecs::component_id> signature(const entity_values &v) const;
  void place(ecs::entity e, const entity_values &v);
  void unplace(ecs::entity e);

  gen_ids ids_;
  live_counter counter_;
  std::map<ecs::entity, entity_values> entities_;
  std::map<ecs::entity, std::size_t> where_; // the modelled archetype of each entity
  std::vector<model_archetype> order_;
};

/// Applies `op` to the world and, when the world call succeeds, to the oracle; returns the
/// world's result (component_exists or component_missing for an add or remove that does not
/// apply, out_of_memory under RFP0, ...).
[[nodiscard]] result<void> apply(const world_op &op, ecs::world &w, world_oracle &oracle);

/// SHA-256 of the allocator state and of every component value (field by field) in iteration
/// order (archetype creation, chunk, row).
[[nodiscard]] fabric::digest hash_world(const ecs::world &w, const gen_ids &ids);

} // namespace sl::runtime::testing
