// RT-INV-STRESS-07 (SL-ARCH-003 ARCH3-R141, R138, `stress_matrix` S10, `limits`
// event_capacity_default): an event flood of 1e6 events per frame (scaled) against the channel
// capacity of 65536. Each of 4 frames splits its events over 8 writers (system indices 0..7, one
// thread each, writing concurrently) in seeded shares. After deliver the channel holds exactly the
// first 65536 events of the frame in delivery order (system index, then each writer's own order)
// with envelope seq 1..65536, the previous frame's 65536 stay readable, dropped() grows by exactly
// the frame's total minus 65536, and end_frame logs one runtime.events warning whose dropped field
// equals the frame's drops. The writers run under the harness watchdog; checks run on the test
// thread.

#include "sl/core/log.hpp"
#include "sl/runtime/events.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/reflect.hpp"

#include "../../../engine/runtime/testing/stress_harness.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace rtt = sl::runtime::testing;

constexpr const char *test_name = "runtime_stress_events_test";
constexpr rt::field_flags kept = rt::field_flags::snapshot | rt::field_flags::hashed;
constexpr std::uint32_t capacity = rt::event_capacity_default;
constexpr std::uint16_t writers = 8;
constexpr std::uint64_t frames = 4;

std::uint64_t splitmix64(std::uint64_t &state) noexcept {
  state += 0x9E3779B97F4A7C15ULL;
  std::uint64_t z = state;
  z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31U);
}

struct flood {
  std::uint32_t writer = 0;
  std::uint32_t n = 0;
};

rt::type_info flood_info() {
  rt::type_builder<flood> b("stress.evt.flood");
  SL_RT_FIELD(b, flood, writer, kept);
  SL_RT_FIELD(b, flood, n, kept);
  return b.build();
}

struct frame_outcome {
  std::uint64_t frame = 0;
  std::uint64_t written = 0;
  std::uint64_t write_errors = 0;
  std::uint64_t dropped = 0;
  std::uint64_t read = 0;
  std::uint64_t previous = 0;
  std::uint64_t order_errors = 0;
  std::uint64_t seq_errors = 0;
  std::uint64_t warnings = 0;
  std::int64_t warned_drops = -1;
};

// Seeded shares of `total` over the writers; some writers stay under the capacity.
std::vector<std::uint32_t> shares(std::uint64_t total, std::uint64_t &state) {
  std::vector<std::uint64_t> weight(writers);
  std::uint64_t sum = 0;
  for (auto &w : weight) {
    w = 1 + (splitmix64(state) % 100U);
    sum += w;
  }
  // Every weight is at least 1, so sum >= writers; the max makes that visible to the analyzer.
  const std::uint64_t divisor = std::max<std::uint64_t>(sum, 1);
  std::vector<std::uint32_t> out(writers);
  std::uint64_t given = 0;
  for (std::size_t i = 0; i + 1 < writers; ++i) {
    out.at(i) = static_cast<std::uint32_t>(total * weight.at(i) / divisor);
    given += out.at(i);
  }
  out.back() = static_cast<std::uint32_t>(total - given);
  return out;
}

std::int64_t int_field(const sl::log::memory_sink::record &r, std::string_view key) {
  const auto it = std::ranges::find_if(r.fields, [key](const auto &f) {
    return f.key == key && std::holds_alternative<std::int64_t>(f.value);
  });
  return it == r.fields.end() ? -1 : std::get<std::int64_t>(it->value);
}

} // namespace

TEST_CASE("RT-INV-STRESS-07: S10 an event flood of 1e6 per frame against capacity 65536 keeps "
          "delivery order and counts overflow exactly") {
  const rtt::cap_timer cap;
  const std::uint64_t seed = rtt::stress_seed(0x5EED099000AULL);
  SL_STRESS_SEED(guard, test_name, seed);
  const std::uint64_t total = std::max<std::uint64_t>(rtt::scaled(1'000'000), capacity + 1);
  rt::event_registry reg;
  REQUIRE(reg.add<flood>("stress.flood", flood_info()).has_value());
  REQUIRE(reg.freeze().has_value());
  auto made = rt::event_channel<flood>::make(reg);
  REQUIRE(made.has_value());
  rt::event_channel<flood> &ch = *made;
  auto sink = std::make_shared<sl::log::memory_sink>(64);
  sl::log::default_logger().add_sink(sink);
  std::vector<frame_outcome> outcomes;
  std::uint64_t state = seed;
  auto watched = rtt::run_watched([&] {
    for (std::uint64_t frame = 1; frame <= frames; ++frame) {
      frame_outcome o;
      o.frame = frame;
      const std::vector<std::uint32_t> share = shares(total, state);
      const std::uint64_t dropped_before = ch.dropped();
      std::vector<std::uint64_t> errors(writers, 0);
      {
        std::vector<std::jthread> pool;
        pool.reserve(writers);
        for (std::uint16_t w = 0; w < writers; ++w) {
          pool.emplace_back([&ch, &errors, &share, w, frame] {
            const rt::writer_key key{
                .tick = frame, .ph = rt::phase::tick, .system_index = w, .chunk_index = 0};
            for (std::uint32_t i = 0; i < share.at(w); ++i) {
              if (!ch.write(key, flood{.writer = w, .n = i})) {
                ++errors.at(w);
              }
            }
          });
        }
      }
      for (std::uint16_t w = 0; w < writers; ++w) {
        o.written += share.at(w);
        o.write_errors += errors.at(w);
      }
      ch.stream().deliver(frame);
      // The expected readable list: writer 0's events, then writer 1's, ..., cut at the capacity.
      std::uint32_t want_writer = 0;
      std::uint32_t want_n = 0;
      // read also yields the previous frame's list (readable until the end of the next frame).
      ch.read([&](const rt::event_envelope &env, const flood &ev) {
        if (env.tick != frame) {
          ++o.previous;
          return;
        }
        while (want_writer < writers && want_n >= share.at(want_writer)) {
          ++want_writer;
          want_n = 0;
        }
        o.order_errors += (ev.writer == want_writer && ev.n == want_n) ? 0U : 1U;
        ++o.read;
        o.seq_errors += env.seq == o.read ? 0U : 1U;
        ++want_n;
      });
      o.dropped = ch.dropped() - dropped_before;
      ch.stream().end_frame();
      for (const auto &r : sink->records()) {
        if (r.system == "runtime.events" &&
            int_field(r, "frame") == static_cast<std::int64_t>(frame)) {
          ++o.warnings;
          o.warned_drops = int_field(r, "dropped");
        }
      }
      outcomes.push_back(o);
    }
  });
  sl::log::default_logger().remove_sink(sink.get());
  REQUIRE(watched);
  REQUIRE(outcomes.size() == frames);
  for (const frame_outcome &o : outcomes) {
    INFO("frame " << o.frame << " events " << o.written);
    CHECK(o.written == total);
    CHECK(o.write_errors == 0);
    CHECK(o.read == capacity);
    CHECK(o.previous == (o.frame == 1 ? 0 : capacity));
    CHECK(o.order_errors == 0);
    CHECK(o.seq_errors == 0);
    CHECK(o.dropped == total - capacity);
    CHECK(o.warnings == 1);
    CHECK(o.warned_drops == static_cast<std::int64_t>(total - capacity));
  }
  SL_STRESS_CAP(cap);
}
