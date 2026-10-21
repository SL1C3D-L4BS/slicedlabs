// RT-INV-SNAP-06 (SL-ARCH-003 ARCH3-R146, ADR-0026): the breakdown localizes a planted
// single-component divergence to its archetype and component. Two worlds built the same way have
// equal breakdowns; one value changed through get<T> in the second makes exactly one part differ,
// {its archetype in creation order, its component}, and first_difference names it. A change to a
// snapshot-only (unhashed) field changes no part and not the state hash; a component resource
// change is {no_archetype, its component}; a contributor change shows only in .contributors.

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/memory.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/replay.hpp"
#include "sl/runtime/snapshot.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace ecs = sl::runtime::ecs;
namespace mem = sl::runtime::mem;
namespace det = sl::runtime::det;

constexpr rt::field_flags kept = rt::field_flags::snapshot | rt::field_flags::hashed;
constexpr ecs::resource_key spot_key{4};

struct spot {
  float x = 0.0F;
  std::int32_t level = 0;
};

struct mark {
  std::uint16_t code = 0;
  std::uint16_t note = 0; // snapshot only: not hashed
};

/// A one-byte contributor.
class byte_contributor final : public det::snapshot_contributor {
public:
  explicit byte_contributor(std::uint8_t value) noexcept : value_(value) {}
  [[nodiscard]] std::string_view name() const noexcept override { return "test.byte"; }
  [[nodiscard]] std::uint32_t version() const noexcept override { return 1; }
  [[nodiscard]] sl::result<void> capture(std::vector<std::byte> &out) const override {
    out.push_back(std::byte{value_});
    return {};
  }
  [[nodiscard]] sl::result<void> restore(std::span<const std::byte> in) override {
    value_ = std::to_integer<std::uint8_t>(in[0]);
    return {};
  }
  void hash(det::state_hasher &h) const override { h.u8(value_); }

private:
  std::uint8_t value_;
};

/// 40 entities over three archetypes ({spot}, {spot, mark}, {mark}) and a spot resource.
struct fixture {
  fixture() {
    rt::type_builder<spot> s("snap.spot");
    SL_RT_FIELD(s, spot, x, kept);
    SL_RT_FIELD(s, spot, level, kept);
    rt::type_builder<mark> m("snap.mark");
    SL_RT_FIELD(m, mark, code, kept);
    SL_RT_FIELD(m, mark, note, rt::field_flags::snapshot);
    auto spot_type = types.add(s.build());
    auto mark_type = types.add(m.build());
    REQUIRE(spot_type.has_value());
    REQUIRE(mark_type.has_value());
    REQUIRE(types.freeze().has_value());
    auto spot_id = registry.add<spot>("snap.spot", *spot_type);
    auto mark_id = registry.add<mark>("snap.mark", *mark_type);
    REQUIRE(spot_id.has_value());
    REQUIRE(mark_id.has_value());
    spot_component = *spot_id;
    mark_component = *mark_id;
    REQUIRE(registry.freeze().has_value());
    auto made = ecs::world::make(registry, memory);
    REQUIRE(made.has_value());
    w = std::move(*made);
    for (std::int32_t i = 0; i < 40; ++i) {
      auto e = w->create();
      REQUIRE(e.has_value());
      handles.push_back(*e);
      if (i % 3 != 2) {
        const spot value{.x = 0.25F * static_cast<float>(i), .level = i};
        REQUIRE(w->add(*e, value).has_value());
      }
      if (i % 3 != 0) {
        const mark value{.code = static_cast<std::uint16_t>(i), .note = 1};
        REQUIRE(w->add(*e, value).has_value());
      }
    }
    REQUIRE(w->add_resource(spot_key, spot{.x = 2.0F, .level = 9}).has_value());
  }

  [[nodiscard]] rt::hash_breakdown parts(std::uint8_t contributor = 0) const {
    const byte_contributor c(contributor);
    const std::array<const det::snapshot_contributor *, 1> contributors{&c};
    auto b = rt::breakdown(*w, registry, types, contributors);
    REQUIRE(b.has_value());
    return *b;
  }

  [[nodiscard]] sl::fabric::digest hash(std::uint8_t contributor = 0) const {
    const byte_contributor c(contributor);
    const std::array<const det::snapshot_contributor *, 1> contributors{&c};
    auto h = rt::state_hash(*w, registry, types, contributors);
    REQUIRE(h.has_value());
    return *h;
  }

  /// The creation-order index of the archetype that holds `e`.
  [[nodiscard]] std::uint32_t archetype_of(ecs::entity e) const {
    const std::span<const ecs::archetype *const> all = w->archetypes();
    for (std::uint32_t i = 0; i < all.size(); ++i) {
      for (std::uint32_t c = 0; c < all[i]->chunk_count(); ++c) {
        if (std::ranges::find(all[i]->entities(c), e) != all[i]->entities(c).end()) {
          return i;
        }
      }
    }
    FAIL("entity not in any archetype");
    return 0;
  }

  rt::type_registry types;
  ecs::component_registry registry;
  ecs::component_id spot_component{};
  ecs::component_id mark_component{};
  mem::tracking_resource memory{"snap.breakdown"};
  std::unique_ptr<ecs::world> w;
  std::vector<ecs::entity> handles;
};

/// How many parts differ between two breakdowns with the same part list.
std::size_t differing(const rt::hash_breakdown &a, const rt::hash_breakdown &b) {
  REQUIRE(a.parts.size() == b.parts.size());
  std::size_t n = 0;
  for (std::size_t i = 0; i < a.parts.size(); ++i) {
    CHECK(a.parts[i].archetype == b.parts[i].archetype);
    CHECK(a.parts[i].component == b.parts[i].component);
    n += a.parts[i].digest == b.parts[i].digest ? 0U : 1U;
  }
  return n;
}

/// The first differing part of two breakdowns; fails the case when there is none.
rt::hash_breakdown::part located(const rt::hash_breakdown &a, const rt::hash_breakdown &b) {
  const std::optional<rt::hash_breakdown::part> first = rt::first_difference(a, b);
  REQUIRE(first.has_value());
  return first.value_or(rt::hash_breakdown::part{});
}

} // namespace

TEST_CASE("RT-INV-SNAP-06: worlds built the same way have equal breakdowns") {
  const fixture a;
  const fixture b;
  const rt::hash_breakdown pa = a.parts();
  const rt::hash_breakdown pb = b.parts();
  CHECK(pa.parts.size() >= 2 + 3 + 4 + 1); // entities, 3 row parts, 4 columns, 1 resource
  CHECK(differing(pa, pb) == 0);
  CHECK(pa.contributors == pb.contributors);
  CHECK_FALSE(rt::first_difference(pa, pb).has_value());
  CHECK(a.hash() == b.hash());
  CHECK(rt::detail::hash_of(pa) == a.hash());
}

TEST_CASE("RT-INV-SNAP-06: a planted component divergence is localized to archetype and "
          "component") {
  for (const std::size_t victim : {std::size_t{0}, std::size_t{4}, std::size_t{37}}) {
    CAPTURE(victim);
    const fixture a;
    const fixture b;
    const ecs::entity e = b.handles.at(victim);
    auto value = b.w->get<spot>(e);
    REQUIRE(value.has_value());
    (*value)->level += 1000;
    const rt::hash_breakdown pa = a.parts();
    const rt::hash_breakdown pb = b.parts();
    CHECK(differing(pa, pb) == 1);
    const rt::hash_breakdown::part first = located(pa, pb);
    CHECK(first.archetype == b.archetype_of(e));
    CHECK(first.component == b.spot_component);
    CHECK(pa.contributors == pb.contributors);
    CHECK(a.hash() != b.hash());
  }
  // a mark value in the {spot, mark} archetype names mark, not spot
  const fixture a;
  const fixture b;
  const ecs::entity e = b.handles.at(1);
  auto value = b.w->get<mark>(e);
  REQUIRE(value.has_value());
  (*value)->code = 0xBEEF;
  const rt::hash_breakdown::part first = located(a.parts(), b.parts());
  CHECK(first.archetype == b.archetype_of(e));
  CHECK(first.component == b.mark_component);
}

TEST_CASE("RT-INV-SNAP-06: an unhashed field changes no part; a resource and a contributor are "
          "localized") {
  const fixture a;
  const fixture b;
  auto value = b.w->get<mark>(b.handles.at(2));
  REQUIRE(value.has_value());
  (*value)->note = 77; // snapshot only
  CHECK(differing(a.parts(), b.parts()) == 0);
  CHECK(a.hash() == b.hash());
  auto taken_a = rt::take(*a.w, a.registry, a.types, {}, {});
  auto taken_b = rt::take(*b.w, b.registry, b.types, {}, {});
  REQUIRE(taken_a.has_value());
  REQUIRE(taken_b.has_value());
  CHECK_FALSE(std::ranges::equal(taken_a->bytes(), taken_b->bytes())); // but it is snapshotted
  CHECK(taken_a->state_hash() == taken_b->state_hash());

  auto resource = b.w->resource<spot>(spot_key);
  REQUIRE(resource.has_value());
  (*resource)->x = -1.0F;
  CHECK(differing(a.parts(), b.parts()) == 1);
  const rt::hash_breakdown::part first = located(a.parts(), b.parts());
  CHECK(first.archetype == rt::hash_breakdown::no_archetype);
  CHECK(first.component == b.spot_component);

  const fixture c;
  const rt::hash_breakdown p0 = c.parts(0);
  const rt::hash_breakdown p1 = c.parts(1);
  CHECK(differing(p0, p1) == 0);
  CHECK(p0.contributors != p1.contributors);
  CHECK_FALSE(rt::first_difference(p0, p1).has_value()); // parts only: see .contributors
  CHECK(c.hash(0) != c.hash(1));
}
