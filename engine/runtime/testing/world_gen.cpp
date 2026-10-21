// testing::world_gen (WO-088): test components, the seeded operation generator, the oracle and the
// field-wise world hash. See world_gen.hpp.

#include "world_gen.hpp"

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/replay.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sl::runtime::testing {

// Every subscript below is a component kind (< gen_component_count, checked by kind_of and the
// generator), a position below an archetype's count, or a word index below 256.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index)

namespace {

constexpr std::array<std::string_view, gen_component_count> gen_names = {
    "gen.position", "gen.velocity", "gen.padded",  "gen.large",
    "gen.tag",      "gen.tracked",  "gen.aligned", "gen.health"};

constexpr std::uint32_t k_position = 0;
constexpr std::uint32_t k_velocity = 1;
constexpr std::uint32_t k_padded = 2;
constexpr std::uint32_t k_large = 3;
constexpr std::uint32_t k_tag = 4;
constexpr std::uint32_t k_tracked = 5;
constexpr std::uint32_t k_aligned = 6;
constexpr std::uint32_t k_health = 7;

gen_position make_position(std::uint64_t v) noexcept {
  return {.x = static_cast<float>(v & 0xFFFFU),
          .y = static_cast<float>((v >> 16U) & 0xFFFFU),
          .z = -static_cast<float>((v >> 32U) & 0xFFFFU)};
}

gen_velocity make_velocity(std::uint64_t v) noexcept {
  return {.dx = static_cast<double>(v & 0xFFFFFFFFU) * 0.5, .dy = -static_cast<double>(v >> 40U)};
}

gen_padded make_padded(std::uint64_t v) noexcept {
  return {.a = static_cast<std::uint8_t>(v), .b = v, .c = static_cast<std::uint16_t>(v >> 8U)};
}

gen_large make_large(std::uint64_t v) noexcept {
  gen_large g;
  for (std::size_t i = 0; i < g.words.size(); ++i) {
    g.words[i] = v ^ (std::uint64_t{i} * 0x9E3779B97F4A7C15ULL);
  }
  return g;
}

gen_aligned make_aligned(std::uint64_t v) noexcept {
  return {.value = static_cast<std::uint32_t>(v ^ (v >> 32U))};
}

gen_health make_health(std::uint64_t v) noexcept {
  return {.hp = static_cast<std::int32_t>(v & 0x7FFFFFFFU),
          .armor = static_cast<std::int16_t>(v & 0x7FFFU)};
}

template <class T> const T &as(const void *p) noexcept { return *static_cast<const T *>(p); }

std::string describe(ecs::entity e) {
  return "{" + std::to_string(e.index) + ", " + std::to_string(e.generation) + "}";
}

} // namespace

std::uint32_t gen_ids::kind_of(ecs::component_id id) const noexcept {
  return static_cast<std::uint32_t>(std::ranges::find(ids, id) - ids.begin());
}

result<gen_ids> register_components(ecs::component_registry &registry) {
  gen_ids out;
  const std::array<result<ecs::component_id>, gen_component_count> got = {
      registry.add<gen_position>(gen_names[k_position]),
      registry.add<gen_velocity>(gen_names[k_velocity]),
      registry.add<gen_padded>(gen_names[k_padded]),
      registry.add<gen_large>(gen_names[k_large]),
      registry.add<gen_tag>(gen_names[k_tag]),
      registry.add<gen_tracked>(gen_names[k_tracked]),
      registry.add<gen_aligned>(gen_names[k_aligned]),
      registry.add<gen_health>(gen_names[k_health])};
  for (std::uint32_t k = 0; k < gen_component_count; ++k) {
    if (!got[k]) {
      return std::unexpected(got[k].error());
    }
    out.ids[k] = *got[k];
  }
  return out;
}

// --- values ---

value_holder::value_holder(std::uint32_t kind, std::uint64_t v, live_counter &counter) noexcept
    : kind_(kind) {
  void *p = storage_.data();
  switch (kind) {
  case k_position:
    ::new (p) gen_position(make_position(v));
    break;
  case k_velocity:
    ::new (p) gen_velocity(make_velocity(v));
    break;
  case k_padded:
    ::new (p) gen_padded(make_padded(v));
    break;
  case k_large:
    ::new (p) gen_large(make_large(v));
    break;
  case k_tag:
    ::new (p) gen_tag();
    break;
  case k_tracked:
    ::new (p) gen_tracked(counter, v);
    break;
  case k_aligned:
    ::new (p) gen_aligned(make_aligned(v));
    break;
  default:
    ::new (p) gen_health(make_health(v));
    break;
  }
}

value_holder::~value_holder() {
  if (kind_ == k_tracked) {
    std::destroy_at(static_cast<gen_tracked *>(get()));
  }
}

bool value_equals(std::uint32_t kind, const void *p, std::uint64_t v) noexcept {
  switch (kind) {
  case k_position: {
    const gen_position want = make_position(v);
    const auto &got = as<gen_position>(p);
    return got.x == want.x && got.y == want.y && got.z == want.z;
  }
  case k_velocity: {
    const gen_velocity want = make_velocity(v);
    const auto &got = as<gen_velocity>(p);
    return got.dx == want.dx && got.dy == want.dy;
  }
  case k_padded: {
    const gen_padded want = make_padded(v);
    const auto &got = as<gen_padded>(p);
    return got.a == want.a && got.b == want.b && got.c == want.c;
  }
  case k_large:
    return as<gen_large>(p).words == make_large(v).words;
  case k_tag:
    return true;
  case k_tracked:
    return as<gen_tracked>(p).value() == v && as<gen_tracked>(p).intact();
  case k_aligned:
    return as<gen_aligned>(p).value == make_aligned(v).value;
  default: {
    const gen_health want = make_health(v);
    const auto &got = as<gen_health>(p);
    return got.hp == want.hp && got.armor == want.armor;
  }
  }
}

void hash_value(std::uint32_t kind, const void *p, det::state_hasher &h) noexcept {
  switch (kind) {
  case k_position:
    h.f32(as<gen_position>(p).x);
    h.f32(as<gen_position>(p).y);
    h.f32(as<gen_position>(p).z);
    break;
  case k_velocity:
    h.f64(as<gen_velocity>(p).dx);
    h.f64(as<gen_velocity>(p).dy);
    break;
  case k_padded:
    h.u8(as<gen_padded>(p).a);
    h.u64(as<gen_padded>(p).b);
    h.u16(as<gen_padded>(p).c);
    break;
  case k_large:
    for (const std::uint64_t w : as<gen_large>(p).words) {
      h.u64(w);
    }
    break;
  case k_tag:
    break;
  case k_tracked:
    h.u64(as<gen_tracked>(p).value());
    break;
  case k_aligned:
    h.u32(as<gen_aligned>(p).value);
    break;
  default:
    h.i32(as<gen_health>(p).hp);
    h.i16(as<gen_health>(p).armor);
    break;
  }
}

// --- the generator ---

op_generator::op_generator(std::uint64_t seed, std::uint32_t mask) noexcept
    : rng_(seed, 0x0880), mask_(mask == 0 ? 1U : mask) {}

std::uint32_t op_generator::pick_component() noexcept {
  for (;;) {
    const auto k = static_cast<std::uint32_t>(rng_.next_below(gen_component_count));
    if (((mask_ >> k) & 1U) != 0) {
      return k;
    }
  }
}

world_op op_generator::next() noexcept {
  world_op op;
  const std::uint64_t roll = rng_.next_below(100);
  if (roll < 25) {
    op.kind = op_kind::create;
  } else if (roll < 40) {
    op.kind = op_kind::destroy;
  } else if (roll < 65) {
    op.kind = op_kind::add;
  } else if (roll < 85) {
    op.kind = op_kind::remove;
  } else {
    op.kind = op_kind::set;
  }
  op.pick = rng_.next_u64();
  op.component = pick_component();
  op.value = rng_.next_u64();
  op.with = static_cast<std::uint32_t>(rng_.next_below(256)) & mask_;
  op.document = rng_.next_below(4) == 0;
  return op;
}

// --- the oracle ---

std::vector<ecs::component_id> world_oracle::signature(const entity_values &v) const {
  std::vector<ecs::component_id> sig;
  for (std::uint32_t k = 0; k < gen_component_count; ++k) {
    if (v.values[k].has_value()) {
      sig.push_back(ids_.ids[k]);
    }
  }
  std::ranges::sort(sig);
  return sig;
}

void world_oracle::place(ecs::entity e, const entity_values &v) {
  const std::vector<ecs::component_id> sig = signature(v);
  std::size_t at = 0;
  while (at < order_.size() && order_[at].components != sig) {
    ++at;
  }
  if (at == order_.size()) {
    order_.push_back(model_archetype{.components = sig, .rows = {}});
  }
  order_[at].rows.push_back(e);
  where_[e] = at;
}

void world_oracle::unplace(ecs::entity e) {
  const auto it = where_.find(e);
  if (it == where_.end()) {
    return;
  }
  std::vector<ecs::entity> &rows = order_[it->second].rows;
  const auto pos = std::ranges::find(rows, e);
  if (pos != rows.end()) {
    *pos = rows.back();
    rows.pop_back();
  }
  where_.erase(it);
}

void world_oracle::on_create(ecs::entity e, const entity_values &v) {
  entities_[e] = v;
  place(e, v);
}

void world_oracle::on_destroy(ecs::entity e) {
  unplace(e);
  entities_.erase(e);
}

void world_oracle::on_add(ecs::entity e, std::uint32_t kind, std::uint64_t v) {
  unplace(e);
  entity_values &vals = entities_[e];
  vals.values[kind] = v;
  place(e, vals);
}

void world_oracle::on_remove(ecs::entity e, std::uint32_t kind) {
  unplace(e);
  entity_values &vals = entities_[e];
  vals.values[kind].reset();
  place(e, vals);
}

void world_oracle::on_set(ecs::entity e, std::uint32_t kind, std::uint64_t v) {
  entity_values &vals = entities_[e];
  if (vals.values[kind].has_value()) {
    vals.values[kind] = v;
  } else {
    on_add(e, kind, v);
  }
}

ecs::entity world_oracle::choose(std::uint64_t pick) const {
  if (entities_.empty()) {
    return ecs::null_entity;
  }
  auto it = entities_.begin();
  std::advance(it, static_cast<std::ptrdiff_t>(pick % entities_.size()));
  return it->first;
}

std::vector<std::string> world_oracle::compare(const ecs::world &w) const {
  std::vector<std::string> out;
  constexpr std::size_t max_reports = 32;
  const auto report = [&out](std::string text) {
    if (out.size() < max_reports) {
      out.push_back(std::move(text));
    }
  };
  if (w.entities().alive() != entities_.size()) {
    report("alive " + std::to_string(w.entities().alive()) + " != oracle " +
           std::to_string(entities_.size()));
  }
  std::int64_t tracked = 0;
  for (const auto &[e, vals] : entities_) {
    if (!w.is_alive(e)) {
      report("entity " + describe(e) + " is not alive");
      continue;
    }
    for (std::uint32_t k = 0; k < gen_component_count; ++k) {
      const std::optional<std::uint64_t> &expected = vals.values[k];
      if (w.has(e, ids_.ids[k]) != expected.has_value()) {
        report("entity " + describe(e) + " component " + std::string(gen_names[k]) +
               (expected.has_value() ? " missing" : " unexpected"));
        continue;
      }
      if (!expected.has_value()) {
        continue;
      }
      if (k == k_tracked) {
        ++tracked;
      }
      auto p = w.get_raw(e, ids_.ids[k]);
      if (!p || !value_equals(k, *p, *expected)) {
        report("entity " + describe(e) + " component " + std::string(gen_names[k]) +
               " has the wrong value");
      }
    }
  }
  if (counter_.live() != tracked) {
    report("live gen_tracked " + std::to_string(counter_.live()) +
           " != " + std::to_string(tracked));
  }
  const auto archetypes = w.archetypes();
  if (archetypes.size() != order_.size()) {
    report("archetypes " + std::to_string(archetypes.size()) + " != model " +
           std::to_string(order_.size()));
    return out;
  }
  for (std::size_t i = 0; i < archetypes.size(); ++i) {
    const ecs::archetype &a = *archetypes[i];
    const model_archetype &m = order_[i];
    if (a.creation_index() != i) {
      report("archetype " + std::to_string(i) + " has creation index " +
             std::to_string(a.creation_index()));
    }
    if (!std::ranges::equal(a.components(), m.components)) {
      report("archetype " + std::to_string(i) + " has another signature than the model");
      continue;
    }
    std::size_t row = 0;
    const std::size_t n = m.rows.size();
    for (std::uint32_t c = 0; c < a.chunk_count(); ++c) {
      const std::size_t first = std::size_t{c} * a.capacity();
      const std::size_t dense = first >= n ? 0 : std::min<std::size_t>(a.capacity(), n - first);
      if (a.rows(c) != dense) {
        report("archetype " + std::to_string(i) + " chunk " + std::to_string(c) + " has " +
               std::to_string(a.rows(c)) + " rows, dense layout " + std::to_string(dense));
      }
      for (const ecs::entity e : a.entities(c)) {
        if (row >= n || m.rows[row] != e) {
          report("archetype " + std::to_string(i) + " row " + std::to_string(row) + " holds " +
                 describe(e) + " out of model order");
        }
        ++row;
      }
    }
    if (row != n) {
      report("archetype " + std::to_string(i) + " has " + std::to_string(row) + " rows, model " +
             std::to_string(n));
    }
  }
  return out;
}

// --- applying operations ---

result<void> apply(const world_op &op, ecs::world &w, world_oracle &oracle) {
  const gen_ids &ids = oracle.ids();
  const ecs::entity target = oracle.choose(op.pick);
  if (op.kind == op_kind::create || target.is_null()) {
    std::array<std::unique_ptr<value_holder>, gen_component_count> holders;
    std::vector<ecs::component_id> cids;
    std::vector<void *> values;
    entity_values vals;
    for (std::uint32_t k = 0; k < gen_component_count; ++k) {
      if (((op.with >> k) & 1U) == 0) {
        continue;
      }
      const std::uint64_t v = op.value + k;
      holders[k] = std::make_unique<value_holder>(k, v, oracle.counter());
      cids.push_back(ids.ids[k]);
      values.push_back(holders[k]->get());
      vals.values[k] = v;
    }
    auto e = w.create_raw(cids, values);
    if (!e) {
      return std::unexpected(e.error());
    }
    oracle.on_create(*e, vals);
    return {};
  }
  const ecs::component_id id = ids.ids[op.component];
  switch (op.kind) {
  case op_kind::destroy: {
    auto ok = w.destroy(target);
    if (ok) {
      oracle.on_destroy(target);
    }
    return ok;
  }
  case op_kind::add: {
    value_holder holder(op.component, op.value, oracle.counter());
    auto ok = w.add_raw(target, id, holder.get(), ecs::write_source::runtime);
    if (ok) {
      oracle.on_add(target, op.component, op.value);
    }
    return ok;
  }
  case op_kind::remove: {
    auto ok = w.remove_raw(target, id);
    if (ok) {
      oracle.on_remove(target, op.component);
    }
    return ok;
  }
  default: {
    value_holder holder(op.component, op.value, oracle.counter());
    auto ok = w.set_raw(target, id, holder.get(),
                        op.document ? ecs::write_source::document : ecs::write_source::runtime);
    if (ok) {
      oracle.on_set(target, op.component, op.value);
    }
    return ok;
  }
  }
}

fabric::digest hash_world(const ecs::world &w, const gen_ids &ids) {
  det::state_hasher h;
  const ecs::entity_allocator::state s = w.entities().capture();
  h.u32(s.next_index);
  for (const std::uint32_t g : s.generations) {
    h.u32(g);
  }
  h.u64(s.free_fifo.size());
  for (const std::uint32_t i : s.free_fifo) {
    h.u32(i);
  }
  h.u64(s.retired.size());
  for (const std::uint32_t i : s.retired) {
    h.u32(i);
  }
  for (const ecs::archetype *a : w.archetypes()) {
    h.u32(static_cast<std::uint32_t>(a->components().size()));
    for (const ecs::component_id id : a->components()) {
      h.u16(id.value);
    }
    for (std::uint32_t c = 0; c < a->chunk_count(); ++c) {
      const auto ents = a->entities(c);
      for (std::size_t r = 0; r < ents.size(); ++r) {
        h.u64(ents[r].bits());
        for (std::size_t p = 0; p < a->components().size(); ++p) {
          const ecs::component_id id = a->components()[p];
          const auto *col = static_cast<const std::byte *>(a->column(id, c));
          hash_value(ids.kind_of(id), col + (r * a->layout()[p].size), h);
        }
      }
    }
  }
  return h.finish();
}

// NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)

} // namespace sl::runtime::testing
