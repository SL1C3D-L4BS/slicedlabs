// RT-INV-EVT-04 (SL-ARCH-003 ARCH3-R142, SL-ARCH-001 ARCH1-R46 to R48): the events a frame
// delivered are exported through the runtime's own event_bus from its one event_source: every
// envelope validates as fabric.event, all come from that source with strictly increasing seq
// across frames and channels, the type is runtime.event.<name>, the payload is the reflected
// bytes as one SLB/1 bytes value and the correlation is the event's {id, scope, tick}. Off the
// owner thread export is wrong_thread and publishes nothing.

#include "sl/core/error.hpp"
#include "sl/core/log.hpp"
#include "sl/fabric/clock.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/events.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/value.hpp"
#include "sl/runtime/events.hpp"
#include "sl/runtime/reflect.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace fab = sl::fabric;

constexpr rt::field_flags kept = rt::field_flags::snapshot | rt::field_flags::hashed;

struct spark {
  std::uint32_t lantern = 0;
  float glow = 0.0F;
};

struct gust {
  std::uint64_t strength = 0;
};

rt::type_info spark_info() {
  rt::type_builder<spark> b("test.evt.spark");
  SL_RT_FIELD(b, spark, lantern, kept);
  SL_RT_FIELD(b, spark, glow, kept);
  return b.build();
}

rt::type_info gust_info() {
  rt::type_builder<gust> b("test.evt.gust");
  SL_RT_FIELD(b, gust, strength, kept);
  return b.build();
}

struct fixture {
  rt::event_registry reg;
  fab::manual_clock clock{1'700'000'000'000'000};
  fab::event_source source{fab::well_known(fab::id_class::operation, 7), clock};
  fab::event_bus bus;
  std::vector<fab::event_envelope> seen;
  fab::subscription sub;

  fixture() {
    REQUIRE(reg.add<spark>("spark", spark_info()).has_value());
    REQUIRE(reg.add<gust>("gust", gust_info()).has_value());
    REQUIRE(reg.freeze().has_value());
    sub = bus.subscribe("runtime.event.",
                        [this](const fab::event_envelope &e) noexcept { seen.push_back(e); });
  }
};

rt::writer_key writer(std::uint16_t system, std::uint64_t tick) {
  return rt::writer_key{
      .tick = tick, .ph = rt::phase::tick, .system_index = system, .chunk_index = 0};
}

} // namespace

TEST_CASE("RT-INV-EVT-04: exported envelopes are valid fabric.event envelopes from one source") {
  fixture f;
  auto sparks = rt::event_channel<spark>::make(f.reg);
  auto gusts = rt::event_channel<gust>::make(f.reg);
  REQUIRE(sparks.has_value());
  REQUIRE(gusts.has_value());
  rt::event_exporter exporter(f.bus, f.source);
  // Given in reverse type order: export goes by ascending type id.
  const std::array<const rt::event_stream *, 2> channels{&gusts->stream(), &sparks->stream()};

  std::size_t expected = 0;
  for (std::uint64_t frame = 1; frame <= 3; ++frame) {
    {
      const sl::log::correlation_scope scope(
          sl::log::correlation{.id = 40 + frame, .scope = "runtime/evt", .tick = frame});
      for (std::uint32_t i = 0; i < 3; ++i) {
        REQUIRE(sparks->write(writer(1, frame), spark{.lantern = i, .glow = 0.5F}).has_value());
      }
      REQUIRE(gusts->write(writer(0, frame), gust{.strength = frame}).has_value());
    }
    sparks->stream().deliver(frame);
    gusts->stream().deliver(frame);
    REQUIRE(exporter.export_frame(frame, channels).has_value());
    expected += 4;
    sparks->stream().end_frame();
    gusts->stream().end_frame();
  }
  CHECK(exporter.export_frame(9, channels).has_value()); // no events: nothing published

  REQUIRE(f.seen.size() == expected);
  CHECK(exporter.exported() == expected);
  CHECK(f.bus.last_seq() == expected);
  CHECK(f.source.next_seq() == expected + 1);
  const fab::record_id actor = f.seen.front().actor;
  for (std::size_t i = 0; i < f.seen.size(); ++i) {
    const fab::event_envelope &e = f.seen[i];
    CHECK(fab::validate(e).has_value());
    CHECK(e.seq == i + 1); // strictly increasing, one source
    CHECK(e.actor == actor);
    CHECK(fab::to_json_line(e).has_value());
    const std::uint64_t frame = (i / 4) + 1;
    CHECK(e.correlation.id == 40 + frame);
    CHECK(e.correlation.scope == "runtime/evt");
    CHECK(e.correlation.tick == frame);
    REQUIRE(e.payload.has_value());
    const std::vector<std::uint8_t> raw = e.payload.value_or(std::vector<std::uint8_t>{});
    auto decoded = fab::decode(raw);
    REQUIRE(decoded.has_value());
    const fab::byte_span bytes = decoded->as_bytes();
    if (i % 4 == 0) {
      CHECK(e.type == "runtime.event.spark");
      REQUIRE(bytes.size() == 8); // lantern u32le, glow f32 bits
      CHECK(bytes[0] == 0);
      CHECK(bytes[4] == 0x00);
      CHECK(bytes[7] == 0x3F); // 0.5F is 0x3F000000
    } else if (i % 4 == 3) {
      CHECK(e.type == "runtime.event.gust");
      REQUIRE(bytes.size() == 8);
      CHECK(bytes[0] == frame);
    } else {
      CHECK(e.type == "runtime.event.spark");
      CHECK(bytes[0] == (i % 4));
    }
  }
}

TEST_CASE("RT-INV-EVT-04: export off the owner thread is wrong_thread and publishes nothing") {
  fixture f;
  auto sparks = rt::event_channel<spark>::make(f.reg);
  REQUIRE(sparks.has_value());
  REQUIRE(sparks->write(writer(0, 1), spark{.lantern = 1, .glow = 1.0F}).has_value());
  sparks->stream().deliver(1);
  rt::event_exporter exporter(f.bus, f.source);
  const std::array<const rt::event_stream *, 1> channels{&sparks->stream()};
  sl::errc code = sl::errc::ok;
  std::thread t([&] {
    const auto r = exporter.export_frame(1, channels);
    code = r ? sl::errc::ok : r.error().code;
  });
  t.join();
  CHECK(code == sl::errc::wrong_thread);
  CHECK(f.seen.empty());
  REQUIRE(exporter.export_frame(1, channels).has_value());
  CHECK(f.seen.size() == 1);
}
