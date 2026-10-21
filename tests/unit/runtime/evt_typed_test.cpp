// RT-INV-EVT-03 (SL-ARCH-003 ARCH3-R137, `error_codes` event_type_unknown, type_registry_frozen):
// event types are reflected structs registered in the event registry, and channels are world
// resources under channel_key. An unregistered type is event_type_unknown at every lookup (by
// name, by type, making a channel, finding a channel in the world), so no channel for it exists
// and nothing can be written; registration refuses a frozen registry, repeats, a mismatched
// type_info and other threads.

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/events.hpp"
#include "sl/runtime/memory.hpp"
#include "sl/runtime/reflect.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace ecs = sl::runtime::ecs;
namespace mem = sl::runtime::mem;

constexpr rt::field_flags kept = rt::field_flags::snapshot | rt::field_flags::hashed;

struct hit {
  std::uint32_t target = 0;
  float damage = 0.0F;
};

struct heal {
  std::uint32_t target = 0;
  std::uint32_t amount = 0;
};

struct never_registered {
  std::uint32_t v = 0;
};

rt::type_info hit_info() {
  rt::type_builder<hit> b("test.evt.hit");
  SL_RT_FIELD(b, hit, target, kept);
  SL_RT_FIELD(b, hit, damage, kept);
  return b.build();
}

rt::type_info heal_info() {
  rt::type_builder<heal> b("test.evt.heal");
  SL_RT_FIELD(b, heal, target, kept);
  SL_RT_FIELD(b, heal, amount, kept);
  return b.build();
}

rt::type_info never_info() {
  rt::type_builder<never_registered> b("test.evt.never");
  SL_RT_FIELD(b, never_registered, v, kept);
  return b.build();
}

struct world_fixture {
  ecs::component_registry components;
  mem::tracking_resource memory{"evt.typed"};
  std::unique_ptr<ecs::world> w;
  world_fixture() {
    REQUIRE(components.freeze().has_value());
    auto made = ecs::world::make(components, memory);
    REQUIRE(made.has_value());
    w = std::move(*made);
  }
};

} // namespace

TEST_CASE("RT-INV-EVT-03: an unregistered event type is event_type_unknown at every lookup") {
  rt::event_registry reg;
  auto id = reg.add<hit>("test.hit", hit_info());
  REQUIRE(id.has_value());
  CHECK(id->value == 1);
  REQUIRE(reg.freeze().has_value());

  const auto by_name = reg.find("test.missing");
  REQUIRE_FALSE(by_name.has_value());
  CHECK(by_name.error().code == sl::errc::event_type_unknown);
  const auto by_type = reg.id_of<never_registered>();
  REQUIRE_FALSE(by_type.has_value());
  CHECK(by_type.error().code == sl::errc::event_type_unknown);
  const auto channel = rt::event_channel<never_registered>::make(reg);
  REQUIRE_FALSE(channel.has_value());
  CHECK(channel.error().code == sl::errc::event_type_unknown);
  CHECK(reg.name(rt::event_type_id{7}).empty());
  CHECK(reg.payload_type(rt::event_type_id{0}) == nullptr);

  world_fixture f;
  auto added = rt::add_event_channel<hit>(*f.w, reg);
  REQUIRE(added.has_value());
  const auto unknown = rt::find_event_channel<never_registered>(*f.w, reg);
  REQUIRE_FALSE(unknown.has_value());
  CHECK(unknown.error().code == sl::errc::event_type_unknown);
  const auto none = rt::add_event_channel<never_registered>(*f.w, reg);
  REQUIRE_FALSE(none.has_value());
  CHECK(none.error().code == sl::errc::event_type_unknown);
}

TEST_CASE("RT-INV-EVT-03: a registered type is written and read through its world channel") {
  rt::event_registry reg;
  const auto hit_id = reg.add<hit>("test.hit", hit_info());
  const auto heal_id = reg.add<heal>("test.heal", heal_info());
  REQUIRE(hit_id.has_value());
  REQUIRE(heal_id.has_value());
  REQUIRE(reg.freeze().has_value());
  CHECK(reg.size() == 2);
  CHECK(reg.find("test.heal").value_or(rt::event_type_id{}) == *heal_id);
  CHECK(reg.id_of<hit>().value_or(rt::event_type_id{}) == *hit_id);
  CHECK(reg.name(*hit_id) == "test.hit");
  REQUIRE(reg.payload_type(*heal_id) != nullptr);
  CHECK(reg.payload_type(*heal_id)->name == "test.evt.heal");
  CHECK(rt::event_registry::channel_key(*hit_id) != rt::event_registry::channel_key(*heal_id));

  world_fixture f;
  auto added = rt::add_event_channel<hit>(*f.w, reg);
  REQUIRE(added.has_value());
  REQUIRE(rt::add_event_channel<heal>(*f.w, reg).has_value());
  const auto again = rt::add_event_channel<hit>(*f.w, reg);
  REQUIRE_FALSE(again.has_value());
  CHECK(again.error().code == sl::errc::already_exists);
  auto found = rt::find_event_channel<hit>(*f.w, reg);
  REQUIRE(found.has_value());
  CHECK(*found == *added);
  CHECK((*found)->type() == *hit_id);

  const rt::writer_key w{.tick = 3, .ph = rt::phase::tick, .system_index = 2, .chunk_index = 0};
  REQUIRE((*found)->write(w, hit{.target = 9, .damage = 1.5F}).has_value());
  (*found)->stream().deliver(1);
  std::vector<hit> got;
  (*found)->read([&](const rt::event_envelope &e, const hit &h) {
    CHECK(e.type == *hit_id);
    CHECK(e.seq == 1);
    CHECK(e.system_index == 2);
    CHECK(e.tick == 3);
    CHECK_FALSE(e.cause.has_value());
    got.push_back(h);
  });
  REQUIRE(got.size() == 1);
  CHECK(got[0].target == 9);
  CHECK(got[0].damage == 1.5F);
}

TEST_CASE("RT-INV-EVT-03: registration refuses frozen, repeated, mismatched and off-thread adds") {
  rt::event_registry reg;
  REQUIRE(reg.add<hit>("test.hit", hit_info()).has_value());
  const auto same_name = reg.add<heal>("test.hit", heal_info());
  REQUIRE_FALSE(same_name.has_value());
  CHECK(same_name.error().code == sl::errc::already_exists);
  const auto same_type = reg.add<hit>("test.hit2", hit_info());
  REQUIRE_FALSE(same_type.has_value());
  CHECK(same_type.error().code == sl::errc::already_exists);
  const auto mismatched = reg.add<heal>("test.heal", never_info());
  REQUIRE_FALSE(mismatched.has_value());
  CHECK(mismatched.error().code == sl::errc::invalid_argument);
  const auto bad_name = reg.add<heal>("test heal", heal_info());
  REQUIRE_FALSE(bad_name.has_value());
  CHECK(bad_name.error().code == sl::errc::invalid_argument);
  const auto unfrozen = rt::event_channel<hit>::make(reg);
  REQUIRE_FALSE(unfrozen.has_value());
  CHECK(unfrozen.error().code == sl::errc::invalid_argument);

  sl::errc off_thread = sl::errc::ok;
  std::thread t([&] {
    const auto r = reg.add<heal>("test.heal", heal_info());
    off_thread = r ? sl::errc::ok : r.error().code;
  });
  t.join();
  CHECK(off_thread == sl::errc::wrong_thread);

  REQUIRE(reg.freeze().has_value());
  REQUIRE(reg.freeze().has_value());
  const auto late = reg.add<heal>("test.heal", heal_info());
  REQUIRE_FALSE(late.has_value());
  CHECK(late.error().code == sl::errc::type_registry_frozen);
  CHECK(reg.size() == 1);
}
