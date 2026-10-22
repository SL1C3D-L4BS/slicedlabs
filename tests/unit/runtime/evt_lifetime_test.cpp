// RT-INV-EVT-02 (SL-ARCH-003 ARCH3-R139, R095): an event written in phase p becomes readable at
// p's boundary and stays readable until the end of the following frame, never after. Under the
// frame scheduler a tick task writes one event per frame, stamped with its frame index, and every
// task phase records what it can read; by hand, deliver and end_frame show the same two-list
// window. A frame marked faulted (ARCH3-R028) delivers nothing from the mark on and keeps the older
// list at arena_flip; its undelivered writes are dropped (WO-109).

#include "sl/runtime/allocators.hpp"
#include "sl/runtime/events.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/memory.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/time.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace jobs = sl::runtime::jobs;
namespace mem = sl::runtime::mem;
namespace rtt = sl::runtime::testing;

constexpr rt::nanos one_tick{16'666'667};

struct stamp {
  std::uint64_t frame = 0;
};

rt::type_info stamp_info() {
  rt::type_builder<stamp> b("test.evt.stamp");
  SL_RT_FIELD(b, stamp, frame, rt::field_flags::snapshot | rt::field_flags::hashed);
  return b.build();
}

struct registry_fixture {
  rt::event_registry reg;
  registry_fixture() {
    REQUIRE(reg.add<stamp>("test.stamp", stamp_info()).has_value());
    REQUIRE(reg.freeze().has_value());
  }
};

std::vector<std::uint64_t> visible(const rt::event_channel<stamp> &ch) {
  std::vector<std::uint64_t> out;
  ch.read([&out](const rt::event_envelope &, const stamp &s) { out.push_back(s.frame); });
  return out;
}

// What one phase run could read: the frame, the phase and the stamps visible.
struct sight {
  std::uint64_t frame = 0;
  rt::phase ph = rt::phase::tick;
  std::vector<std::uint64_t> seen = std::vector<std::uint64_t>(4);
  std::uint32_t count = 0;
};

constexpr std::size_t max_sights = 64;

struct journal {
  std::vector<sight> sights = std::vector<sight>(max_sights);
  std::size_t used = 0; // one task per phase: phases never overlap

  void look(const rt::event_channel<stamp> &ch, const rt::task_context &ctx,
            rt::phase ph) noexcept {
    if (used == sights.size()) {
      return;
    }
    sight &s = sights[used++];
    s.frame = ctx.time.frame_index;
    s.ph = ph;
    ch.read([&s](const rt::event_envelope &, const stamp &st) noexcept {
      if (s.count < s.seen.size()) {
        s.seen[s.count] = st.frame;
      }
      ++s.count;
    });
  }
};

struct rig {
  rtt::scripted_platform plat;
  mem::tracking_resource upstream{"evt.lifetime"};
  std::unique_ptr<jobs::job_system> js;
  std::unique_ptr<mem::frame_arena> arena;
  std::unique_ptr<rt::frame_scheduler> fs;

  rig() {
    jobs::job_config c;
    c.workers = 2;
    c.capacity = 1024;
    auto started = jobs::job_system::start(plat.get(), std::move(c));
    REQUIRE(started.has_value());
    js = std::move(*started);
    arena = std::make_unique<mem::frame_arena>(upstream, 2, 65536);
    auto made = rt::frame_scheduler::make(*js, plat.get(), *arena);
    REQUIRE(made.has_value());
    fs = std::move(*made);
  }
};

} // namespace

TEST_CASE("RT-INV-EVT-02: deliver and end_frame give a two-frame window") {
  const registry_fixture f;
  auto made = rt::event_channel<stamp>::make(f.reg);
  REQUIRE(made.has_value());
  rt::event_channel<stamp> &ch = *made;
  const rt::writer_key w{.tick = 0, .ph = rt::phase::tick, .system_index = 0, .chunk_index = 0};

  REQUIRE(ch.write(w, stamp{.frame = 1}).has_value());
  CHECK(visible(ch).empty()); // not before the boundary
  ch.stream().deliver(1);
  CHECK(visible(ch) == std::vector<std::uint64_t>{1});
  ch.stream().end_frame();
  CHECK(visible(ch) == std::vector<std::uint64_t>{1}); // the following frame still sees it
  REQUIRE(ch.write(w, stamp{.frame = 2}).has_value());
  ch.stream().deliver(2);
  CHECK(visible(ch) == std::vector<std::uint64_t>{1, 2});
  ch.stream().end_frame();
  CHECK(visible(ch) == std::vector<std::uint64_t>{2}); // frame 1 is gone after frame 2 ends
  ch.stream().deliver(3);
  ch.stream().end_frame();
  CHECK(visible(ch).empty());
}

TEST_CASE("RT-INV-EVT-02: under the scheduler an event is visible from its boundary for a frame") {
  const registry_fixture f;
  auto made = rt::event_channel<stamp>::make(f.reg);
  REQUIRE(made.has_value());
  rt::event_channel<stamp> &ch = *made;
  rig r;
  rt::attach(ch.stream(), *r.fs);
  journal j;
  auto look = [&](rt::phase ph) {
    REQUIRE(r.fs->add_task(rt::task_desc{
        .name = "look",
        .ph = ph,
        .fn = [chp = &ch, jp = &j, ph](rt::task_context &ctx) noexcept { jp->look(*chp, ctx, ph); },
        .after = {}}));
  };
  look(rt::phase::apply_inputs);
  look(rt::phase::pre_tick);
  // The tick task writes its frame's stamp and checks that its own write is not yet visible.
  std::atomic<std::uint32_t> self_visible{0};
  REQUIRE(r.fs->add_task(rt::task_desc{
      .name = "write",
      .ph = rt::phase::tick,
      .fn =
          [chp = &ch, sv = &self_visible](rt::task_context &ctx) noexcept {
            const rt::writer_key w{
                .tick = ctx.tick, .ph = rt::phase::tick, .system_index = 1, .chunk_index = 0};
            const auto wrote = chp->write(w, stamp{.frame = ctx.time.frame_index});
            static_cast<void>(wrote);
            chp->read([&](const rt::event_envelope &, const stamp &s) noexcept {
              if (s.frame == ctx.time.frame_index) {
                sv->fetch_add(1);
              }
            });
          },
      .after = {}}));
  look(rt::phase::post_tick);
  look(rt::phase::observe);
  REQUIRE(r.fs->build());

  constexpr std::uint64_t frames = 5; // frame 1 runs no tick, frames 2-5 one tick each
  for (std::uint64_t i = 0; i < frames; ++i) {
    r.plat.clock().advance(one_tick);
    REQUIRE(r.fs->run_frame().has_value());
  }
  CHECK(self_visible.load() == 0);

  std::uint32_t checked = 0;
  for (std::size_t i = 0; i < j.used; ++i) {
    const sight &s = j.sights[i];
    std::vector<std::uint64_t> want;
    if (s.frame >= 3) {
      want.push_back(s.frame - 1); // the previous frame's stamp, until this frame ends
    }
    if (s.frame >= 2 && (s.ph == rt::phase::post_tick || s.ph == rt::phase::observe)) {
      want.push_back(s.frame); // this frame's stamp from the tick boundary on
    }
    REQUIRE(s.count <= s.seen.size());
    const std::vector<std::uint64_t> got(s.seen.begin(), s.seen.begin() + s.count);
    CHECK_MESSAGE(got == want, "frame ", s.frame, " phase ", static_cast<int>(s.ph));
    ++checked;
  }
  // frame 1: apply_inputs and observe; frames 2-5: all four looking phases
  CHECK(checked == 2 + (4 * (frames - 1)));
  CHECK(std::ranges::none_of(visible(ch), [](std::uint64_t v) { return v < frames; }));
}

TEST_CASE("RT-INV-EVT-02: a faulted frame keeps the older list and delivers nothing") {
  const registry_fixture f;
  auto made = rt::event_channel<stamp>::make(f.reg);
  REQUIRE(made.has_value());
  rt::event_channel<stamp> &ch = *made;
  rig r;
  constexpr std::uint64_t faulted = 3;
  // Before attach, as the runtime's own hook: frame 3 faults at its tick boundary.
  r.fs->add_phase_end_hook([fs = r.fs.get()](rt::phase ph, rt::time::tick /*t*/) noexcept {
    if (ph == rt::phase::tick && fs->current().frame_index == faulted) {
      fs->set_frame_faulted(true);
    }
  });
  rt::attach(ch.stream(), *r.fs);
  journal j;
  for (const rt::phase ph : {rt::phase::pre_tick, rt::phase::post_tick}) {
    REQUIRE(r.fs->add_task(rt::task_desc{
        .name = "look",
        .ph = ph,
        .fn = [chp = &ch, jp = &j, ph](rt::task_context &ctx) noexcept { jp->look(*chp, ctx, ph); },
        .after = {}}));
  }
  REQUIRE(r.fs->add_task(rt::task_desc{
      .name = "write",
      .ph = rt::phase::tick,
      .fn =
          [chp = &ch](rt::task_context &ctx) noexcept {
            const rt::writer_key w{
                .tick = ctx.tick, .ph = rt::phase::tick, .system_index = 1, .chunk_index = 0};
            const auto wrote = chp->write(w, stamp{.frame = ctx.time.frame_index});
            static_cast<void>(wrote);
          },
      .after = {}}));
  REQUIRE(r.fs->build());

  std::vector<std::vector<std::uint64_t>> after_frame;
  for (std::uint64_t i = 0; i < 5; ++i) {
    r.plat.clock().advance(one_tick);
    REQUIRE(r.fs->run_frame().has_value());
    CHECK(r.fs->frame_faulted() == (r.fs->current().frame_index == faulted));
    after_frame.push_back(visible(ch));
  }
  // After frame 2 the list holds stamp 2; the faulted frame 3 keeps it (no flip), frame 4 flips.
  CHECK(after_frame[1] == std::vector<std::uint64_t>{2});
  CHECK(after_frame[2] == std::vector<std::uint64_t>{2});
  CHECK(after_frame[3] == std::vector<std::uint64_t>{4});
  CHECK(after_frame[4] == std::vector<std::uint64_t>{5});
  std::uint32_t checked = 0;
  for (std::size_t i = 0; i < j.used; ++i) {
    const sight &s = j.sights[i];
    const std::vector<std::uint64_t> got(s.seen.begin(), s.seen.begin() + s.count);
    CHECK_MESSAGE(std::ranges::find(got, faulted) == got.end(), "frame ", s.frame); // never seen
    if (s.frame == faulted || s.frame == faulted + 1) {
      std::vector<std::uint64_t> want{2}; // the older list the faulted frame kept
      if (s.frame == faulted + 1 && s.ph == rt::phase::post_tick) {
        want.push_back(faulted + 1);
      }
      CHECK_MESSAGE(got == want, "frame ", s.frame, " phase ", static_cast<int>(s.ph));
      ++checked;
    }
  }
  CHECK(checked == 4);
}
