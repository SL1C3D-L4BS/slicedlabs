// RT-INV-EVT-05 (SL-ARCH-003 ARCH3-R141, `limits` event_capacity_default, `failpoints` RFP7): a
// channel keeps at most 65536 events per frame. Beyond it write drops the newest and counts each
// drop exactly, the kept events keep their order (with several writers, the kept ones are the
// first in delivery order), try_write returns event_overflow, one runtime.events warning is
// logged per frame with drops and none for a frame without, and RFP7 makes the channel behave as
// full.

#include "sl/core/error.hpp"
#include "sl/core/log.hpp"
#include "sl/runtime/events.hpp"
#include "sl/runtime/faults.hpp"
#include "sl/runtime/reflect.hpp"

#include "../../../engine/runtime/testing/faults_arm.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <string_view>
#include <variant>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace rtt = sl::runtime::testing;

constexpr rt::field_flags kept = rt::field_flags::snapshot | rt::field_flags::hashed;
constexpr std::uint32_t capacity = rt::event_capacity_default;

struct tagged {
  std::uint32_t writer = 0;
  std::uint32_t n = 0;
};

rt::type_info tagged_info() {
  rt::type_builder<tagged> b("test.evt.tagged");
  SL_RT_FIELD(b, tagged, writer, kept);
  SL_RT_FIELD(b, tagged, n, kept);
  return b.build();
}

struct fixture {
  rt::event_registry reg;
  std::unique_ptr<rt::event_channel<tagged>> ch;
  fixture() {
    REQUIRE(reg.add<tagged>("test.tagged", tagged_info()).has_value());
    REQUIRE(reg.freeze().has_value());
    auto made = rt::event_channel<tagged>::make(reg);
    REQUIRE(made.has_value());
    ch = std::make_unique<rt::event_channel<tagged>>(std::move(*made));
  }
};

rt::writer_key writer(std::uint16_t system) {
  return rt::writer_key{.tick = 1, .ph = rt::phase::tick, .system_index = system, .chunk_index = 0};
}

std::vector<tagged> readable(const rt::event_channel<tagged> &ch) {
  std::vector<tagged> out;
  ch.read([&out](const rt::event_envelope &, const tagged &t) { out.push_back(t); });
  return out;
}

// A memory sink on the default logger for the lifetime of the object.
struct attached {
  std::shared_ptr<sl::log::memory_sink> sink = std::make_shared<sl::log::memory_sink>(64);
  attached() { sl::log::default_logger().add_sink(sink); }
  attached(const attached &) = delete;
  attached &operator=(const attached &) = delete;
  attached(attached &&) = delete;
  attached &operator=(attached &&) = delete;
  ~attached() { sl::log::default_logger().remove_sink(sink.get()); }

  [[nodiscard]] std::vector<sl::log::memory_sink::record> overflow_records() const {
    std::vector<sl::log::memory_sink::record> out;
    std::ranges::copy_if(
        sink->records(), std::back_inserter(out),
        [](const sl::log::memory_sink::record &r) { return r.system == "runtime.events"; });
    return out;
  }
};

std::int64_t int_field(const sl::log::memory_sink::record &r, std::string_view key) {
  const auto it = std::ranges::find_if(r.fields, [key](const auto &f) {
    return f.key == key && std::holds_alternative<std::int64_t>(f.value);
  });
  return it == r.fields.end() ? -1 : std::get<std::int64_t>(it->value);
}

} // namespace

TEST_CASE("RT-INV-EVT-05: overflow drops the newest, counts exactly and keeps the order") {
  const attached log;
  fixture f;
  constexpr std::uint32_t extra = 100;
  for (std::uint32_t i = 0; i < capacity + extra; ++i) {
    REQUIRE(f.ch->write(writer(0), tagged{.writer = 0, .n = i}).has_value());
  }
  CHECK(f.ch->dropped() == extra);
  const auto over = f.ch->try_write(writer(0), tagged{.writer = 0, .n = capacity + extra});
  REQUIRE_FALSE(over.has_value());
  CHECK(over.error().code == sl::errc::event_overflow);
  CHECK(f.ch->dropped() == extra + 1);

  f.ch->stream().deliver(1);
  const std::vector<tagged> got = readable(*f.ch);
  REQUIRE(got.size() == capacity);
  bool ordered = true;
  for (std::uint32_t i = 0; i < capacity; ++i) {
    ordered = ordered && got[i].n == i;
  }
  CHECK(ordered);
  // The frame is full: a later write in the same frame is dropped too.
  const auto later = f.ch->try_write(writer(3), tagged{.writer = 3, .n = 0});
  REQUIRE_FALSE(later.has_value());
  CHECK(later.error().code == sl::errc::event_overflow);
  CHECK(f.ch->dropped() == extra + 2);

  CHECK(log.overflow_records().empty()); // logged at end_frame only
  f.ch->stream().end_frame();
  const auto records = log.overflow_records();
  REQUIRE(records.size() == 1);
  CHECK(records[0].level == sl::log::level::warn);
  CHECK(int_field(records[0], "dropped") == extra + 2);
  CHECK(int_field(records[0], "frame") == 1);

  // The next frame has room again and logs nothing without drops.
  REQUIRE(f.ch->try_write(writer(0), tagged{.writer = 0, .n = 7}).has_value());
  f.ch->stream().deliver(2);
  f.ch->stream().end_frame();
  CHECK(log.overflow_records().size() == 1);
  CHECK(f.ch->dropped() == extra + 2);
}

TEST_CASE("RT-INV-EVT-05: with several writers the kept events are the first in delivery order") {
  fixture f;
  constexpr std::uint32_t half = (capacity / 2) + 10;
  // Writer 1 writes first, but writer 0 comes first in delivery order.
  for (std::uint32_t i = 0; i < half; ++i) {
    REQUIRE(f.ch->write(writer(1), tagged{.writer = 1, .n = i}).has_value());
  }
  for (std::uint32_t i = 0; i < half; ++i) {
    REQUIRE(f.ch->write(writer(0), tagged{.writer = 0, .n = i}).has_value());
  }
  CHECK(f.ch->dropped() == 0); // each writer is under the capacity on its own
  f.ch->stream().deliver(1);
  CHECK(f.ch->dropped() == (2 * half) - capacity);
  const std::vector<tagged> got = readable(*f.ch);
  REQUIRE(got.size() == capacity);
  bool ordered = true;
  for (std::uint32_t i = 0; i < capacity; ++i) {
    const std::uint32_t want_writer = i < half ? 0U : 1U;
    const std::uint32_t want_n = i < half ? i : i - half;
    ordered = ordered && got[i].writer == want_writer && got[i].n == want_n;
  }
  CHECK(ordered);
}

TEST_CASE("RT-INV-EVT-05: RFP7 makes the channel behave as full") {
  if constexpr (!rt::faults_compiled) {
    return;
  }
  fixture f;
  {
    const rtt::scoped_arm arm(rt::failpoint::rfp7, 1);
    REQUIRE(arm.status().has_value());
    REQUIRE(f.ch->write(writer(0), tagged{.writer = 0, .n = 0}).has_value()); // dropped
  }
  CHECK(f.ch->dropped() == 1);
  {
    const rtt::scoped_arm arm(rt::failpoint::rfp7, 1);
    REQUIRE(arm.status().has_value());
    const auto tried = f.ch->try_write(writer(0), tagged{.writer = 0, .n = 1});
    REQUIRE_FALSE(tried.has_value());
    CHECK(tried.error().code == sl::errc::event_overflow);
  }
  CHECK(f.ch->dropped() == 2);
  REQUIRE(f.ch->write(writer(0), tagged{.writer = 0, .n = 2}).has_value());
  f.ch->stream().deliver(1);
  const std::vector<tagged> got = readable(*f.ch);
  REQUIRE(got.size() == 1);
  CHECK(got[0].n == 2);
  f.ch->stream().end_frame();
}
