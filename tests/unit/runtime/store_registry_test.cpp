// RT-INV-STORE-04 (SL-ARCH-003 ARCH3-R106, ADR-0025): the component registry assigns ids in
// registration order, refuses every registration after freeze with component_registry_frozen,
// rejects a type above 2048 bytes or 64-byte alignment statically (component_type) and at run time
// (add_raw: component_invalid), rejects a type whose move may throw by a requires-expression, and
// hashes its layout into a digest that depends on the order, names, sizes and alignments.

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/memory.hpp"

#include "../../../engine/runtime/testing/world_gen.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

namespace ecs = sl::runtime::ecs;
namespace mem = sl::runtime::mem;
namespace rtt = sl::runtime::testing;

struct too_large {
  std::array<std::uint8_t, 4096> bytes{};
};

struct alignas(128) too_aligned {
  std::uint32_t v = 0;
};

// The type under test must have a potentially-throwing move; it never throws.
struct throwing_move {
  throwing_move() = default;
  throwing_move(const throwing_move &) = default;
  // NOLINTNEXTLINE(bugprone-unsafe-to-allow-exceptions): the potentially-throwing move is the point
  throwing_move(throwing_move &&other) noexcept(false) : v(other.v) {}
  throwing_move &operator=(const throwing_move &) = default;
  // NOLINTNEXTLINE(bugprone-unsafe-to-allow-exceptions): the potentially-throwing move is the point
  throwing_move &operator=(throwing_move &&other) noexcept(false) {
    v = other.v;
    return *this;
  }
  ~throwing_move() = default;
  int v = 0;
};

struct small_a {
  std::uint32_t v = 0;
};

struct small_b {
  std::uint64_t v = 0;
};

static_assert(!ecs::component_type<too_large>);
static_assert(!ecs::component_type<too_aligned>);
static_assert(!ecs::component_type<throwing_move>);
static_assert(ecs::component_type<rtt::gen_large>);   // exactly 2048 bytes
static_assert(ecs::component_type<rtt::gen_aligned>); // exactly 64-byte aligned
static_assert(ecs::component_type<rtt::gen_tracked>);
static_assert(ecs::component_type<rtt::gen_tag>);

// True when registry.add<T> forms (a requires-expression over the constrained template).
template <class T>
constexpr bool registrable = requires(ecs::component_registry &r) { r.add<T>("x"); };

// cppcheck-suppress constParameterCallback
void noop_move(void *dst, void *src) noexcept { std::memcpy(dst, src, 4); }
void noop_destroy(void * /*p*/) noexcept {}

ecs::component_info raw(std::string_view name, std::uint32_t size, std::uint32_t align) {
  return ecs::component_info{.name = name,
                             .size = size,
                             .align = align,
                             .trivially_relocatable = true,
                             .move = noop_move,
                             .destroy = noop_destroy};
}

} // namespace

TEST_CASE("RT-INV-STORE-04: ids follow registration order and the frozen registry refuses") {
  ecs::component_registry registry;
  auto ids = rtt::register_components(registry);
  REQUIRE(ids.has_value());
  std::uint32_t expected = 0;
  for (const ecs::component_id id : ids->ids) {
    CHECK(id.value == expected++);
  }
  CHECK(registry.count() == rtt::gen_component_count);
  CHECK(registry.find("gen.health").value() == ids->ids[7]);
  CHECK(registry.id_of<rtt::gen_tracked>().value() == ids->ids[5]);
  const ecs::component_info &tracked = registry.info(ids->ids[5]);
  CHECK(tracked.name == "gen.tracked");
  CHECK(tracked.size == sizeof(rtt::gen_tracked));
  CHECK(tracked.align == alignof(rtt::gen_tracked));
  CHECK_FALSE(tracked.trivially_relocatable);
  CHECK(registry.info(ids->ids[2]).trivially_relocatable);
  CHECK(registry.info(ids->ids[6]).align == 64);
  CHECK(registry.find("gen.nothing").error().code == sl::errc::component_unknown);
  CHECK(registry.id_of<small_a>().error().code == sl::errc::component_unknown);

  CHECK_FALSE(registry.frozen());
  REQUIRE(registry.freeze().has_value());
  CHECK(registry.frozen());
  CHECK(registry.add<small_a>("late.a").error().code == sl::errc::component_registry_frozen);
  CHECK(registry.add_raw(raw("late.raw", 4, 4)).error().code ==
        sl::errc::component_registry_frozen);
  CHECK(registry.count() == rtt::gen_component_count);
  CHECK(registry.find("gen.position").value() == ids->ids[0]); // still answers
}

TEST_CASE("RT-INV-STORE-04: oversize, overaligned and throwing-move types are rejected") {
  ecs::component_registry registry;
  CHECK(registry.add_raw(raw("big", 4096, 8)).error().code == sl::errc::component_invalid);
  CHECK(registry.add_raw(raw("wide", 4, 128)).error().code == sl::errc::component_invalid);
  CHECK(registry.add_raw(raw("odd", 4, 3)).error().code == sl::errc::component_invalid);
  CHECK(registry.add_raw(raw("zero", 0, 1)).error().code == sl::errc::component_invalid);
  auto no_move = raw("no.move", 4, 4);
  no_move.move = nullptr;
  CHECK(registry.add_raw(no_move).error().code == sl::errc::component_invalid);
  CHECK(registry.add_raw(raw("", 4, 4)).error().code == sl::errc::invalid_argument);
  CHECK(registry.add_raw(raw("max", 2048, 64)).has_value());
  CHECK(registry.count() == 1);

  // add<T> is constrained on component_type: the call does not form for these types.
  CHECK_FALSE(registrable<too_large>);
  CHECK_FALSE(registrable<too_aligned>);
  CHECK_FALSE(registrable<throwing_move>);
  CHECK(registrable<small_a>);
}

TEST_CASE("RT-INV-STORE-04: duplicates, the 1024 limit and an unfrozen registry") {
  ecs::component_registry registry;
  REQUIRE(registry.add<small_a>("dup.a").has_value());
  CHECK(registry.add<small_b>("dup.a").error().code == sl::errc::already_exists);
  CHECK(registry.add<small_a>("dup.other").error().code == sl::errc::already_exists);
  CHECK(registry.add_raw(raw("dup.a", 4, 4)).error().code == sl::errc::already_exists);
  {
    mem::tracking_resource memory("store.registry");
    CHECK(ecs::world::make(registry, memory).error().code == sl::errc::invalid_argument);
  }
  std::vector<std::string> names;
  names.reserve(ecs::max_component_types);
  for (std::uint32_t i = 1; i < ecs::max_component_types; ++i) {
    names.push_back("limit." + std::to_string(i));
    REQUIRE(registry.add_raw(raw(names.back(), 4, 4)).has_value());
  }
  CHECK(registry.count() == ecs::max_component_types);
  CHECK(registry.add_raw(raw("limit.over", 4, 4)).error().code == sl::errc::component_invalid);
  CHECK(registry.add<small_b>("limit.b").error().code == sl::errc::component_invalid);
  CHECK(registry.find("limit.1023").value().value == 1023);
}

TEST_CASE("RT-INV-STORE-04: the layout digest is deterministic and order sensitive") {
  const auto digest_of = [](const std::vector<ecs::component_info> &infos) {
    ecs::component_registry r;
    for (const ecs::component_info &info : infos) {
      REQUIRE(r.add_raw(info).has_value());
    }
    REQUIRE(r.freeze().has_value());
    return r.layout_digest();
  };
  const auto a = raw("d.a", 4, 4);
  const auto b = raw("d.b", 8, 8);
  const auto base = digest_of({a, b});
  CHECK(digest_of({a, b}) == base);
  CHECK(digest_of({b, a}) != base);
  CHECK(digest_of({a, raw("d.b", 16, 8)}) != base);
  CHECK(digest_of({a, raw("d.b", 8, 16)}) != base);
  CHECK(digest_of({a, raw("d.c", 8, 8)}) != base);
  CHECK(digest_of({a}) != base);

  ecs::component_registry one;
  ecs::component_registry two;
  REQUIRE(rtt::register_components(one).has_value());
  REQUIRE(rtt::register_components(two).has_value());
  CHECK(one.layout_digest() == two.layout_digest());
}
