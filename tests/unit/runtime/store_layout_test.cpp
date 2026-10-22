// RT-INV-STORE-03 (SL-ARCH-003 ARCH3-R108, `limits` chunk_bytes, chunk_alignment,
// max_rows_per_chunk, ADR-0025): chunks are 16384 bytes carved from 2 MiB slabs (tag ecs.chunks),
// the entity column comes first, component columns sit at 64-byte aligned offsets in (alignment
// descending, id ascending) order followed by the u32 tick columns, and the capacity is the largest
// n <= 1024 whose layout fits. The expected capacity below is computed independently (a linear
// scan from 1024 down) and compared with the archetype's. The registry's layout digest walks the
// reflected fields of each component (name, offset, size, kind; WO-109).

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/memory.hpp"
#include "sl/runtime/reflect.hpp"

#include "../../../engine/runtime/testing/world_gen.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ecs = sl::runtime::ecs;
namespace mem = sl::runtime::mem;
namespace rtt = sl::runtime::testing;

struct fixture {
  explicit fixture(mem::resource *upstream = nullptr) {
    auto got = rtt::register_components(registry);
    REQUIRE(got.has_value());
    ids = *got;
    REQUIRE(registry.freeze().has_value());
    oracle = std::make_unique<rtt::world_oracle>(ids);
    auto made = ecs::world::make(registry, upstream != nullptr ? *upstream : memory);
    REQUIRE(made.has_value());
    w = std::move(*made);
  }
  ecs::component_registry registry;
  rtt::gen_ids ids;
  mem::tracking_resource memory{"store.layout"};
  std::unique_ptr<rtt::world_oracle> oracle; // outlives the world (gen_tracked counts into it)
  std::unique_ptr<ecs::world> w;
};

std::uint64_t round64(std::uint64_t v) { return (v + 63) / 64 * 64; }

template <class T> std::uintptr_t address(T *p) { return std::bit_cast<std::uintptr_t>(p); }

struct probe {
  std::int32_t a = 0;
  std::int32_t b = 0;
};

// The layout digest of a registry holding probe, reflected with one field (no reflection when
// `field` is empty).
sl::fabric::digest probe_digest(std::string_view field, std::uint32_t offset,
                                sl::runtime::field_kind kind) {
  sl::runtime::type_registry types;
  const sl::runtime::type_info *reflect = nullptr;
  if (!field.empty()) {
    sl::runtime::type_builder<probe> b("store.probe");
    b.field(field, offset, kind, sl::runtime::field_flags::snapshot);
    auto t = types.add(b.build());
    REQUIRE(t.has_value());
    reflect = t.value_or(nullptr);
  }
  ecs::component_registry registry;
  REQUIRE(registry.add<probe>("store.probe", reflect).has_value());
  REQUIRE(registry.freeze().has_value());
  return registry.layout_digest();
}

struct column_shape {
  std::uint32_t id = 0;
  std::uint32_t size = 0;
  std::uint32_t align = 0;
};

// The capacity ARCH3-R108 gives, by a linear scan from 1024 down; 0 when one row does not fit.
std::uint32_t expected_capacity(std::vector<column_shape> cols) {
  std::ranges::sort(cols, [](const column_shape &a, const column_shape &b) {
    return a.align != b.align ? a.align > b.align : a.id < b.id;
  });
  for (std::uint64_t n = 1024; n > 0; --n) {
    std::uint64_t end =
        std::ranges::fold_left(cols, 8 * n, [n](std::uint64_t at, const column_shape &c) {
          return round64(at) + (c.size * n);
        });
    for (std::size_t k = 0; k < cols.size() * 2; ++k) {
      end = round64(end) + (4 * n);
    }
    if (end <= 16384) {
      return static_cast<std::uint32_t>(n);
    }
  }
  return 0;
}

// Creates one entity holding the kinds in `mask` and returns its archetype.
const ecs::archetype &archetype_of(fixture &f, std::uint32_t mask) {
  const rtt::world_op op{.kind = rtt::op_kind::create, .value = 11, .with = mask};
  REQUIRE(rtt::apply(op, *f.w, *f.oracle).has_value());
  const ecs::entity e = std::prev(f.oracle->entities().end())->first;
  for (const ecs::archetype *a : f.w->archetypes()) {
    for (std::uint32_t c = 0; c < a->chunk_count(); ++c) {
      const auto held = a->entities(c);
      if (std::ranges::find(held, e) != held.end()) {
        return *a;
      }
    }
  }
  FAIL("the entity has no archetype");
  return *f.w->archetypes()[0];
}

void check_layout(const ecs::archetype &a, const ecs::component_registry &registry) {
  std::vector<column_shape> shapes;
  for (const ecs::component_id id : a.components()) {
    const ecs::component_info &info = registry.info(id);
    shapes.push_back(column_shape{.id = id.value, .size = info.size, .align = info.align});
  }
  const std::uint32_t cap = a.capacity();
  CHECK(cap == expected_capacity(shapes));
  REQUIRE(cap > 0);

  std::vector<ecs::archetype::column_layout> cols(a.layout().begin(), a.layout().end());
  REQUIRE(cols.size() == a.components().size());
  std::ranges::sort(cols, [](const auto &x, const auto &y) { return x.offset < y.offset; });
  std::uint64_t end = std::uint64_t{8} * cap; // the entity column
  for (std::size_t i = 0; i < cols.size(); ++i) {
    const auto &c = cols[i];
    CHECK(c.offset % 64 == 0);
    CHECK(c.offset >= end);
    CHECK(c.offset - end < 64); // packed: only the padding to the next 64 bytes
    end = c.offset + (std::uint64_t{c.size} * cap);
    if (i != 0) {
      const auto &p = cols[i - 1];
      CHECK((p.align > c.align || (p.align == c.align && p.id < c.id)));
    }
  }
  std::vector<std::uint64_t> ticks;
  for (const auto &c : cols) {
    ticks.push_back(c.added_offset);
    ticks.push_back(c.changed_offset);
    CHECK(c.added_offset % 64 == 0);
    CHECK(c.changed_offset % 64 == 0);
  }
  std::ranges::sort(ticks);
  for (const std::uint64_t t : ticks) {
    CHECK(t >= end);
    end = t + (std::uint64_t{4} * cap);
  }
  CHECK(end <= ecs::chunk_bytes);

  for (std::uint32_t c = 0; c < a.chunk_count(); ++c) {
    const auto base = address(a.entities(c).data());
    CHECK(base % ecs::chunk_alignment == 0);
    for (const auto &col : a.layout()) {
      CHECK(address(a.column(col.id, c)) == base + col.offset);
      CHECK(address(a.added_ticks(col.id, c).data()) == base + col.added_offset);
      CHECK(address(a.changed_ticks(col.id, c).data()) == base + col.changed_offset);
    }
  }
}

} // namespace

TEST_CASE("RT-INV-STORE-03: columns are 64-byte aligned in (alignment desc, id asc) order") {
  fixture f;
  for (const std::uint32_t mask : {0x00U, 0x01U, 0x0EU, 0x10U, 0x48U, 0xC6U, 0xF7U, 0xFFU}) {
    INFO("mask " << mask);
    check_layout(archetype_of(f, mask), f.registry);
  }
}

TEST_CASE("RT-INV-STORE-03: the layout digest changes with a reflected field's kind or offset") {
  using sl::runtime::field_kind;
  const sl::fabric::digest base = probe_digest("a", 0, field_kind::i32);
  CHECK(probe_digest("a", 0, field_kind::i32) == base);
  CHECK(probe_digest("a", 0, field_kind::f32) != base); // kind (same size)
  CHECK(probe_digest("a", 0, field_kind::u32) != base);
  CHECK(probe_digest("a", 4, field_kind::i32) != base); // offset
  CHECK(probe_digest("b", 0, field_kind::i32) != base); // name
  CHECK(probe_digest("a", 0, field_kind::i16) != base); // size and kind
  CHECK(probe_digest("", 0, field_kind::i32) != base);  // no reflection
}

TEST_CASE("RT-INV-STORE-03: known capacities") {
  fixture f;
  CHECK(archetype_of(f, 0x00U).capacity() == 1024); // the entity column only: 8 KiB
  CHECK(archetype_of(f, 0x08U).capacity() == 7);    // gen_large: 2048 bytes a row
  const std::vector<column_shape> position{{.id = 0, .size = 12, .align = 4}};
  CHECK(archetype_of(f, 0x01U).capacity() == expected_capacity(position));
}

TEST_CASE("RT-INV-STORE-03: chunks are 16384 bytes apart, carved from 2 MiB slabs") {
  mem::tag_registry tags;
  auto entities_tag = tags.add("ecs.entities");
  REQUIRE(entities_tag.has_value());
  auto chunks_tag = tags.add("ecs.chunks");
  REQUIRE(chunks_tag.has_value());
  mem::tracking_resource memory("store.layout.tags", *entities_tag, &tags);
  {
    fixture f(&memory);
    CHECK(memory.counters(*chunks_tag).live_bytes == 0);
    for (int i = 0; i < (3 * 1024) + 1; ++i) {
      REQUIRE(f.w->create().has_value());
    }
    const ecs::archetype &a = *f.w->archetypes()[0];
    REQUIRE(a.chunk_count() == 4);
    CHECK(a.rows(3) == 1);
    const auto first = address(a.entities(0).data());
    for (std::uint32_t c = 0; c < a.chunk_count(); ++c) {
      const auto at = address(a.entities(c).data());
      CHECK(at % ecs::chunk_alignment == 0);
      const std::uintptr_t gap = at > first ? at - first : first - at;
      CHECK(gap % ecs::chunk_bytes == 0);
      CHECK(gap < ecs::chunk_pool_slab_bytes);
    }
    CHECK(f.w->stats().slabs == 1);
    CHECK(f.w->stats().chunks == 4);
    CHECK(f.w->stats().row_capacity == 4 * 1024);
    CHECK(memory.counters(*chunks_tag).live_bytes == ecs::chunk_pool_slab_bytes);
    CHECK(memory.counters(*entities_tag).live_bytes > 0); // the world's metadata
    for (int i = 0; i < 300; ++i) {
      REQUIRE(f.w->create_with(rtt::gen_large{}).has_value());
    }
    CHECK(f.w->stats().slabs == 1); // 4 + ceil(300 / 7) = 47 chunks of 128
    CHECK(f.w->stats().chunks == 4 + 43);
  }
  CHECK(memory.totals().live_bytes == 0);
}

TEST_CASE("RT-INV-STORE-03: an archetype whose row does not fit one chunk is archetype_limit") {
  ecs::component_registry registry;
  std::vector<ecs::component_id> ids(8);
  const std::vector<std::string> names{"big.0", "big.1", "big.2", "big.3",
                                       "big.4", "big.5", "big.6", "big.7"};
  for (std::size_t i = 0; i < ids.size(); ++i) {
    auto id = registry.add_raw(ecs::component_info{
        .name = names[i],
        .size = 2048,
        .align = 8,
        .trivially_relocatable = true,
        .move = [](void *dst, void *src) noexcept { std::memcpy(dst, src, 2048); },
        .destroy = [](void *) noexcept {}});
    REQUIRE(id.has_value());
    ids[i] = *id;
  }
  REQUIRE(registry.freeze().has_value());
  mem::tracking_resource memory("store.layout.big");
  {
    auto made = ecs::world::make(registry, memory);
    REQUIRE(made.has_value());
    ecs::world &w = **made;
    alignas(64) std::array<std::byte, 2048> value{};
    const std::array<void *, 8> values{value.data(), value.data(), value.data(), value.data(),
                                       value.data(), value.data(), value.data(), value.data()};
    auto seven = w.create_raw(std::span(ids).first(7), std::span(values).first(7));
    REQUIRE(seven.has_value());
    REQUIRE(w.archetypes().size() == 1);
    CHECK(w.archetypes()[0]->capacity() == 1);
    auto eight = w.create_raw(ids, values);
    REQUIRE_FALSE(eight.has_value());
    CHECK(eight.error().code == sl::errc::archetype_limit);
    CHECK(w.archetypes().size() == 1);
    CHECK(w.entities().alive() == 1);
    CHECK(w.add_raw(*seven, ids[7], value.data(), ecs::write_source::runtime).error().code ==
          sl::errc::archetype_limit);
    CHECK(w.has(*seven, ids[0]));
    CHECK_FALSE(w.has(*seven, ids[7]));
  }
  CHECK(memory.totals().live_bytes == 0);
}
