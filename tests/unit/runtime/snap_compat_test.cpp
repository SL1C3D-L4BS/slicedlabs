// RT-INV-SNAP-05 (SL-ARCH-003 ARCH3-R143, R144, ADR-0026): take inside a phase is
// snapshot_mid_tick, and a type-digest, runtime_abi_digest or contributor mismatch is refused with
// its code and restores nothing. Each refusal is checked against the target's allocator state,
// state hash and contributor state before and after. A contributor that fails its restore and a
// world that refuses the image (archetype_limit) roll the contributors already restored back.

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/memory.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/replay.hpp"
#include "sl/runtime/snapshot.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
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

struct spot {
  float x = 0.0F;
  std::int32_t level = 0;
};

struct mark {
  std::uint16_t code = 0;
};

enum class variant : std::uint8_t { same, mark_unhashed, spot_only };

/// Two reflected components (or one); `entities` entities, every other one with a mark.
struct fixture {
  explicit fixture(variant v = variant::same, std::uint32_t entities = 6,
                   std::uint32_t max_archetypes = ecs::max_archetypes) {
    rt::type_builder<spot> s("snap.spot");
    SL_RT_FIELD(s, spot, x, kept);
    SL_RT_FIELD(s, spot, level, kept);
    rt::type_builder<mark> m("snap.mark");
    SL_RT_FIELD(m, mark, code, v == variant::mark_unhashed ? rt::field_flags::snapshot : kept);
    auto spot_type = types.add(s.build());
    REQUIRE(spot_type.has_value());
    REQUIRE(registry.add<spot>("snap.spot", *spot_type).has_value());
    if (v != variant::spot_only) {
      auto mark_type = types.add(m.build());
      REQUIRE(mark_type.has_value());
      REQUIRE(registry.add<mark>("snap.mark", *mark_type).has_value());
    }
    REQUIRE(types.freeze().has_value());
    REQUIRE(registry.freeze().has_value());
    auto made = ecs::world::make(registry, memory, {.max_archetypes = max_archetypes});
    REQUIRE(made.has_value());
    w = std::move(*made);
    for (std::uint32_t i = 0; i < entities; ++i) {
      auto e = w->create();
      REQUIRE(e.has_value());
      const spot value{.x = static_cast<float>(i), .level = static_cast<int>(i)};
      REQUIRE(w->add(*e, value).has_value());
      if (v != variant::spot_only && i % 2 == 0) {
        REQUIRE(w->add(*e, mark{.code = static_cast<std::uint16_t>(i)}).has_value());
      }
    }
  }

  rt::type_registry types;
  ecs::component_registry registry;
  mem::tracking_resource memory{"snap.compat"};
  std::unique_ptr<ecs::world> w;
};

/// A one-byte contributor; restore refuses the byte `refuse` (snapshot_corrupt).
class byte_contributor final : public det::snapshot_contributor {
public:
  byte_contributor(std::string_view name, std::uint32_t version, std::uint8_t value,
                   int refuse = -1) noexcept
      : name_(name), version_(version), value_(value), refuse_(refuse) {}

  [[nodiscard]] std::string_view name() const noexcept override { return name_; }
  [[nodiscard]] std::uint32_t version() const noexcept override { return version_; }
  [[nodiscard]] sl::result<void> capture(std::vector<std::byte> &out) const override {
    out.push_back(std::byte{value_});
    return {};
  }
  [[nodiscard]] sl::result<void> restore(std::span<const std::byte> in) override {
    if (in.size() != 1 || std::to_integer<int>(in[0]) == refuse_) {
      return rt::make_error(sl::errc::snapshot_corrupt, "byte_contributor: refused");
    }
    value_ = std::to_integer<std::uint8_t>(in[0]);
    return {};
  }
  void hash(det::state_hasher &h) const override { h.u8(value_); }
  [[nodiscard]] std::uint8_t value() const noexcept { return value_; }

private:
  std::string_view name_;
  std::uint32_t version_;
  std::uint8_t value_;
  int refuse_;
};

sl::fabric::digest abi(std::uint8_t first) {
  sl::fabric::digest d;
  d.bytes.at(0) = first;
  return d;
}

rt::snapshot take(const fixture &f,
                  std::span<const det::snapshot_contributor *const> contributors) {
  rt::snapshot_meta meta;
  meta.frame = 9;
  meta.tick = 9;
  meta.runtime_abi_digest = abi(1);
  auto s = rt::take(*f.w, f.registry, f.types, contributors, meta);
  REQUIRE(s.has_value());
  return *s;
}

/// What a refused restore must leave unchanged.
struct observed {
  ecs::entity_allocator::state entities;
  sl::fabric::digest hash;
  friend bool operator==(const observed &, const observed &) = default;
};

observed observe(const fixture &f) {
  auto h = rt::state_hash(*f.w, f.registry, f.types, {});
  REQUIRE(h.has_value());
  return {.entities = f.w->entities().capture(), .hash = *h};
}

sl::errc restore_code(fixture &f, std::span<det::snapshot_contributor *const> contributors,
                      const rt::snapshot &s, const sl::fabric::digest &expected) {
  auto r = rt::restore(*f.w, f.registry, f.types, contributors, s, expected);
  return r.has_value() ? sl::errc::ok : r.error().code;
}

} // namespace

TEST_CASE("RT-INV-SNAP-05: take, restore and the state hash inside a phase are snapshot_mid_tick") {
  fixture f;
  const rt::snapshot s = take(f, {});
  fixture target(variant::same, 2);
  const observed before = observe(target);
  f.w->set_phase_active(true);
  target.w->set_phase_active(true);
  auto inside = rt::take(*f.w, f.registry, f.types, {}, {});
  REQUIRE_FALSE(inside.has_value());
  CHECK(inside.error().code == sl::errc::snapshot_mid_tick);
  auto hashed = rt::state_hash(*f.w, f.registry, f.types, {});
  REQUIRE_FALSE(hashed.has_value());
  CHECK(hashed.error().code == sl::errc::snapshot_mid_tick);
  auto parts = rt::breakdown(*f.w, f.registry, f.types, {});
  REQUIRE_FALSE(parts.has_value());
  CHECK(parts.error().code == sl::errc::snapshot_mid_tick);
  CHECK(restore_code(target, {}, s, abi(1)) == sl::errc::snapshot_mid_tick);
  target.w->set_phase_active(false);
  f.w->set_phase_active(false);
  CHECK(observe(target) == before);
  CHECK(restore_code(target, {}, s, abi(1)) == sl::errc::ok);
  CHECK(observe(target) == observe(f));
}

TEST_CASE("RT-INV-SNAP-05: a runtime_abi_digest mismatch is snapshot_abi_mismatch") {
  fixture f;
  const rt::snapshot s = take(f, {});
  fixture target(variant::same, 3);
  const observed before = observe(target);
  CHECK(restore_code(target, {}, s, abi(2)) == sl::errc::snapshot_abi_mismatch);
  CHECK(observe(target) == before);
}

TEST_CASE("RT-INV-SNAP-05: a layout digest that differs in one field kind refuses restore") {
  // runtime::runtime_abi_digest folds component_registry::layout_digest in (ARCH3-R032); two
  // registries whose spot differs only in the reflected kind of `level` give two digests, so a
  // snapshot recorded under one is snapshot_abi_mismatch under the other (WO-109).
  const auto layout = [](rt::field_kind level) {
    rt::type_registry types;
    rt::type_builder<spot> b("snap.spot");
    b.field("x", offsetof(spot, x), rt::field_kind::f32, kept);
    b.field("level", offsetof(spot, level), level, kept);
    auto t = types.add(b.build());
    REQUIRE(t.has_value());
    ecs::component_registry registry;
    REQUIRE(registry.add<spot>("snap.spot", t.value_or(nullptr)).has_value());
    REQUIRE(registry.freeze().has_value());
    return registry.layout_digest();
  };
  const sl::fabric::digest signed_level = layout(rt::field_kind::i32);
  const sl::fabric::digest unsigned_level = layout(rt::field_kind::u32);
  REQUIRE(signed_level != unsigned_level);
  fixture f;
  rt::snapshot_meta meta;
  meta.frame = 9;
  meta.tick = 9;
  meta.runtime_abi_digest = signed_level;
  auto s = rt::take(*f.w, f.registry, f.types, {}, meta);
  REQUIRE(s.has_value());
  fixture target(variant::same, 3);
  const observed before = observe(target);
  CHECK(restore_code(target, {}, *s, unsigned_level) == sl::errc::snapshot_abi_mismatch);
  CHECK(observe(target) == before);
  CHECK(restore_code(target, {}, *s, signed_level) == sl::errc::ok);
  CHECK(observe(target) == observe(f));
}

TEST_CASE("RT-INV-SNAP-05: a type digest or type count mismatch is snapshot_schema_mismatch") {
  fixture f;
  const rt::snapshot s = take(f, {});
  for (const variant v : {variant::mark_unhashed, variant::spot_only}) {
    fixture target(v, 3);
    const observed before = observe(target);
    CHECK(restore_code(target, {}, s, abi(1)) == sl::errc::snapshot_schema_mismatch);
    CHECK(observe(target) == before);
  }
}

TEST_CASE("RT-INV-SNAP-05: an unregistered contributor or version is "
          "snapshot_contributor_unknown") {
  fixture f;
  const byte_contributor recorded("test.byte", 1, 7);
  const std::array<const det::snapshot_contributor *, 1> taken{&recorded};
  const rt::snapshot s = take(f, taken);
  fixture target(variant::same, 1);
  const observed before = observe(target);
  CHECK(restore_code(target, {}, s, abi(1)) == sl::errc::snapshot_contributor_unknown);
  byte_contributor other_version("test.byte", 2, 3);
  byte_contributor other_name("test.other", 1, 3);
  for (det::snapshot_contributor *c :
       std::array<det::snapshot_contributor *, 2>{&other_version, &other_name}) {
    const std::array<det::snapshot_contributor *, 1> offered{c};
    CHECK(restore_code(target, offered, s, abi(1)) == sl::errc::snapshot_contributor_unknown);
  }
  CHECK(other_version.value() == 3);
  CHECK(other_name.value() == 3);
  CHECK(observe(target) == before);
  byte_contributor matching("test.byte", 1, 3);
  const std::array<det::snapshot_contributor *, 1> offered{&matching};
  CHECK(restore_code(target, offered, s, abi(1)) == sl::errc::ok);
  CHECK(matching.value() == 7);
}

TEST_CASE("RT-INV-SNAP-05: a failing contributor or a refused image restores nothing") {
  fixture f;
  const byte_contributor first("test.a", 1, 10);
  const byte_contributor second("test.b", 1, 20);
  const std::array<const det::snapshot_contributor *, 2> taken{&second, &first};
  const rt::snapshot s = take(f, taken);

  // test.b refuses the recorded byte: test.a, restored first (by name), is rolled back
  fixture target(variant::same, 2);
  const observed before = observe(target);
  byte_contributor a("test.a", 1, 1);
  byte_contributor b("test.b", 1, 2, 20);
  const std::array<det::snapshot_contributor *, 2> offered{&a, &b};
  CHECK(restore_code(target, offered, s, abi(1)) == sl::errc::snapshot_corrupt);
  CHECK(a.value() == 1);
  CHECK(b.value() == 2);
  CHECK(observe(target) == before);

  // the world refuses an image with more archetypes than it may hold: both are rolled back
  fixture small(variant::same, 0, 2);
  const observed small_before = observe(small);
  byte_contributor c("test.a", 1, 1);
  byte_contributor d("test.b", 1, 2);
  const std::array<det::snapshot_contributor *, 2> offered_small{&c, &d};
  CHECK(restore_code(small, offered_small, s, abi(1)) == sl::errc::archetype_limit);
  CHECK(c.value() == 1);
  CHECK(d.value() == 2);
  CHECK(observe(small) == small_before);
}
