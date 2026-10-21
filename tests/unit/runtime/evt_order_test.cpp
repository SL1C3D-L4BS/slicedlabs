// RT-INV-EVT-01 (SL-ARCH-003 ARCH3-R139, R086, `determinism` pending events and correlation ids):
// events written by parallel jobs are delivered in (phase, system_index, chunk_index, seq) order
// whatever the worker count: four systems added out of index order write three chunks each in
// apply_inputs, tick and observe, and the readable lists, the runtime.events capture bytes and its
// hash are equal on 1, 2 and 4 workers. Running under other correlation ids changes the ids the
// envelopes carry but neither the order nor a byte of the capture.

#include "sl/core/log.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/events.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/memory.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/replay.hpp"
#include "sl/runtime/time.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace det = sl::runtime::det;
namespace jobs = sl::runtime::jobs;
namespace mem = sl::runtime::mem;
namespace rtt = sl::runtime::testing;

constexpr rt::nanos one_tick{16'666'667};
constexpr rt::field_flags kept = rt::field_flags::snapshot | rt::field_flags::hashed;

struct mark {
  std::uint32_t ph = 0;
  std::uint32_t system = 0;
  std::uint32_t chunk = 0;
  std::uint32_t k = 0;
};

rt::type_info mark_info() {
  rt::type_builder<mark> b("test.evt.mark");
  SL_RT_FIELD(b, mark, ph, kept);
  SL_RT_FIELD(b, mark, system, kept);
  SL_RT_FIELD(b, mark, chunk, kept);
  SL_RT_FIELD(b, mark, k, kept);
  return b.build();
}

struct rig {
  rtt::scripted_platform plat;
  mem::tracking_resource upstream{"evt.order"};
  std::unique_ptr<jobs::job_system> js;
  std::unique_ptr<mem::frame_arena> arena;
  std::unique_ptr<rt::frame_scheduler> fs;

  explicit rig(std::uint32_t workers) {
    jobs::job_config c;
    c.workers = workers;
    c.capacity = 4096;
    auto started = jobs::job_system::start(plat.get(), std::move(c));
    REQUIRE(started.has_value());
    js = std::move(*started);
    arena = std::make_unique<mem::frame_arena>(upstream, workers, 65536);
    auto made = rt::frame_scheduler::make(*js, plat.get(), *arena);
    REQUIRE(made.has_value());
    fs = std::move(*made);
  }
};

void emit(rt::event_channel<mark> &ch, const rt::task_context &ctx, rt::phase ph,
          std::uint16_t system) noexcept {
  for (const std::uint32_t chunk : {2U, 0U, 1U}) {
    for (std::uint32_t k = 0; k < 3; ++k) {
      const rt::writer_key w{
          .tick = ctx.tick, .ph = ph, .system_index = system, .chunk_index = chunk};
      const auto wrote = ch.write(
          w, mark{.ph = static_cast<std::uint32_t>(ph), .system = system, .chunk = chunk, .k = k});
      static_cast<void>(wrote);
      std::this_thread::yield();
    }
  }
}

struct outcome {
  std::vector<std::byte> bytes;
  std::vector<sl::fabric::digest> hashes;
  std::vector<mark> order;
  std::vector<std::uint32_t> seqs;
  std::vector<std::uint64_t> correlation_ids;
};

outcome run(std::uint32_t workers, std::uint64_t correlation_id) {
  rt::event_registry reg;
  REQUIRE(reg.add<mark>("test.mark", mark_info()).has_value());
  REQUIRE(reg.freeze().has_value());
  auto made = rt::event_channel<mark>::make(reg);
  REQUIRE(made.has_value());
  rt::event_channel<mark> &ch = *made;

  rig r(workers);
  rt::attach(ch.stream(), *r.fs);
  constexpr std::array<std::uint16_t, 4> systems{5, 1, 3, 0};
  for (const rt::phase ph : {rt::phase::apply_inputs, rt::phase::tick, rt::phase::observe}) {
    for (const std::uint16_t s : systems) {
      REQUIRE(r.fs->add_task(rt::task_desc{
          .name = "writer",
          .ph = ph,
          .fn = [chp = &ch, ph, s](rt::task_context &ctx) noexcept { emit(*chp, ctx, ph, s); },
          .after = {}}));
    }
  }

  outcome out;
  std::array<rt::event_stream *, 1> streams{&ch.stream()};
  const rt::events_contributor contributor(streams);
  r.fs->add_boundary_hook(rt::boundary_slot::snapshot, [&](const rt::boundary_context &b) {
    REQUIRE(contributor.capture(out.bytes).has_value());
    det::state_hasher h;
    contributor.hash(h);
    out.hashes.push_back(h.finish());
    ch.read_refs([&](const rt::event_ref &ref, const rt::event_envelope &e, const mark &m) {
      if (ref.frame == b.frame_index) {
        out.order.push_back(m);
        out.seqs.push_back(e.seq);
        out.correlation_ids.push_back(e.correlation.id);
      }
    });
  });
  REQUIRE(r.fs->build());

  const sl::log::correlation_scope outer(
      sl::log::correlation{.id = correlation_id, .scope = "evt.order", .tick = 0});
  for (int f = 0; f < 4; ++f) {
    r.plat.clock().advance(one_tick);
    REQUIRE(r.fs->run_frame().has_value());
  }
  return out;
}

auto key_of(const mark &m) { return std::tuple(m.ph, m.system, m.chunk, m.k); }

} // namespace

TEST_CASE("RT-INV-EVT-01: delivery order is (phase, system_index, chunk_index, seq)") {
  const outcome o = run(4, 0x1111);
  // Frame 1 runs apply_inputs and observe, frames 2-4 also one tick: 36 events per task phase.
  constexpr std::size_t per_phase = std::size_t{4} * 3 * 3;
  REQUIRE(o.order.size() == (per_phase * 2) + (per_phase * 9));
  std::size_t frame_start = 0;
  for (std::size_t i = 0; i < o.order.size(); ++i) {
    if (o.seqs[i] == 1) {
      frame_start = i;
    }
    CHECK(o.seqs[i] == i - frame_start + 1); // seq 1.. per frame in delivery order
    if (i > frame_start) {
      CHECK(key_of(o.order[i - 1]) < key_of(o.order[i]));
    }
  }
}

TEST_CASE("RT-INV-EVT-01: order, capture bytes and hash do not depend on the worker count") {
  const outcome one = run(1, 0x2222);
  const outcome two = run(2, 0x2222);
  const outcome four = run(4, 0x2222);
  REQUIRE(!one.bytes.empty());
  CHECK(one.bytes == two.bytes);
  CHECK(one.bytes == four.bytes);
  CHECK(one.hashes == two.hashes);
  CHECK(one.hashes == four.hashes);
  REQUIRE(one.order.size() == four.order.size());
  for (std::size_t i = 0; i < one.order.size(); ++i) {
    CHECK(key_of(one.order[i]) == key_of(four.order[i]));
    CHECK(one.seqs[i] == four.seqs[i]);
  }
}

TEST_CASE("RT-INV-EVT-01: perturbed correlation ids change neither order nor bytes") {
  const outcome a = run(4, 0xA0A0);
  const outcome b = run(4, 0xB0B0'0000'0000'0001ULL);
  REQUIRE(!a.correlation_ids.empty());
  for (const std::uint64_t id : a.correlation_ids) {
    CHECK(id == 0xA0A0);
  }
  for (const std::uint64_t id : b.correlation_ids) {
    CHECK(id == 0xB0B0'0000'0000'0001ULL);
  }
  CHECK(a.bytes == b.bytes);
  CHECK(a.hashes == b.hashes);
  REQUIRE(a.order.size() == b.order.size());
  for (std::size_t i = 0; i < a.order.size(); ++i) {
    CHECK(key_of(a.order[i]) == key_of(b.order[i]));
  }
}
