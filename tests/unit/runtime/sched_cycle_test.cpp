// RT-INV-SCHED-04 (SL-ARCH-003 ARCH3-R124): a constraint cycle is schedule_cycle naming every
// system on it. A three-cycle (a after c, b after a, c after b) with a system downstream of it
// and an independent one names a, b and c only; a system after itself is a cycle of one; two
// separate before-cycles in one phase are both named; a cycle in a later phase is found too, and
// a failed build registers nothing (the scheduler stays unbuilt).

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/frame.hpp"

#include "../../../engine/runtime/testing/sim_fixture.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace ecs = sl::runtime::ecs;
namespace rtt = sl::runtime::testing;
using sl::errc;

struct empty_body {
  void operator()(ecs::system_context & /*ctx*/) const noexcept {}
};

void add(rtt::sched_rig &r, std::string_view name, std::vector<std::string_view> before,
         std::vector<std::string_view> after, rt::phase ph = rt::phase::tick) {
  auto id = r.systems->add(ecs::system_desc{.name = name,
                                            .ph = ph,
                                            .access = {},
                                            .before = std::move(before),
                                            .after = std::move(after),
                                            .fn = empty_body{},
                                            .chunked = false,
                                            .queries = {}});
  REQUIRE(id.has_value());
}

std::unique_ptr<rtt::sched_rig> rig() {
  auto made = rtt::sched_rig::make(1);
  REQUIRE(made.has_value());
  return std::move(*made);
}

// The cycle error of build, with its message.
std::string cycle_message(rtt::sched_rig &r) {
  auto built = r.systems->build(*r.frames, *r.world, *r.queue);
  REQUIRE_FALSE(built.has_value());
  CHECK(built.error().code == errc::schedule_cycle);
  CHECK_FALSE(r.systems->built());
  return built.error().message;
}

bool names(const std::string &message, std::string_view name) {
  // Names are listed as ": a, b, c": match a whole list element.
  const std::string text = message + ",";
  return text.contains(" " + std::string(name) + ",");
}

} // namespace

TEST_CASE("RT-INV-SCHED-04: a three-cycle names its three systems and nothing else") {
  auto r = rig();
  add(*r, "cyc.a", {}, {"cyc.c"});
  add(*r, "cyc.b", {}, {"cyc.a"});
  add(*r, "cyc.c", {}, {"cyc.b"});
  add(*r, "cyc.down", {}, {"cyc.a"});
  add(*r, "cyc.free", {}, {});
  const std::string message = cycle_message(*r);
  INFO(message);
  CHECK(names(message, "cyc.a"));
  CHECK(names(message, "cyc.b"));
  CHECK(names(message, "cyc.c"));
  CHECK_FALSE(names(message, "cyc.down"));
  CHECK_FALSE(names(message, "cyc.free"));
  CHECK(message.find("tick") != std::string::npos);
}

TEST_CASE("RT-INV-SCHED-04: a system after itself is a cycle of one") {
  auto r = rig();
  add(*r, "self.ok", {}, {});
  add(*r, "self.loop", {}, {"self.loop"});
  const std::string message = cycle_message(*r);
  INFO(message);
  CHECK(names(message, "self.loop"));
  CHECK_FALSE(names(message, "self.ok"));
}

TEST_CASE("RT-INV-SCHED-04: two before-cycles of one phase are both named") {
  auto r = rig();
  add(*r, "two.p", {"two.q"}, {});
  add(*r, "two.q", {"two.p"}, {});
  add(*r, "two.r", {"two.s"}, {});
  add(*r, "two.s", {"two.r"}, {});
  add(*r, "two.t", {}, {"two.p"});
  const std::string message = cycle_message(*r);
  INFO(message);
  CHECK(names(message, "two.p"));
  CHECK(names(message, "two.q"));
  CHECK(names(message, "two.r"));
  CHECK(names(message, "two.s"));
  CHECK_FALSE(names(message, "two.t"));
}

TEST_CASE("RT-INV-SCHED-04: a cycle in a later phase fails the build and registers nothing") {
  auto r = rig();
  add(*r, "fine.a", {}, {}, rt::phase::apply_inputs);
  add(*r, "fine.b", {}, {"fine.a"}, rt::phase::apply_inputs);
  add(*r, "late.x", {"late.y"}, {}, rt::phase::observe);
  add(*r, "late.y", {}, {}, rt::phase::observe);
  add(*r, "late.z", {"late.x"}, {"late.y"}, rt::phase::observe); // x -> y -> z -> x
  const std::string message = cycle_message(*r);
  INFO(message);
  CHECK(names(message, "late.x"));
  CHECK(names(message, "late.y"));
  CHECK(names(message, "late.z"));
  CHECK_FALSE(names(message, "fine.a"));
  CHECK(message.find("observe") != std::string::npos);
  CHECK(r->systems->order(rt::phase::apply_inputs).empty());
  // Nothing reached the frame scheduler: it builds and runs empty frames.
  REQUIRE(r->frames->build().has_value());
  auto s = r->step();
  REQUIRE(s.has_value());
  CHECK(s->phases[static_cast<std::size_t>(rt::phase::apply_inputs)].jobs == 0);
}
