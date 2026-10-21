// RT-INV-EVT-06 (SL-ARCH-003 ARCH3-R140, `error_codes` event_payload_invalid): in Debug builds
// write and try_write reject a payload whose f32 or f64 field is NaN or infinite with
// event_payload_invalid, write nothing and count no drop; finite values (-0.0, denormals,
// extremes) pass. Release builds do not validate (event_validation is false).

#include "sl/core/error.hpp"
#include "sl/runtime/events.hpp"
#include "sl/runtime/reflect.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <limits>
#include <vector>

namespace {

namespace rt = sl::runtime;

constexpr rt::field_flags kept = rt::field_flags::snapshot | rt::field_flags::hashed;

struct sample {
  std::uint32_t id = 0;
  float speed = 0.0F;
  double distance = 0.0;
};

rt::type_info sample_info() {
  rt::type_builder<sample> b("test.evt.sample");
  SL_RT_FIELD(b, sample, id, kept);
  SL_RT_FIELD(b, sample, speed, kept);
  SL_RT_FIELD(b, sample, distance, rt::field_flags::transient);
  return b.build();
}

std::size_t delivered(rt::event_channel<sample> &ch) {
  ch.stream().deliver(1);
  std::size_t n = 0;
  ch.read([&n](const rt::event_envelope &, const sample &) { ++n; });
  return n;
}

} // namespace

TEST_CASE("RT-INV-EVT-06: an invalid payload is event_payload_invalid in Debug") {
  rt::event_registry reg;
  REQUIRE(reg.add<sample>("test.sample", sample_info()).has_value());
  REQUIRE(reg.freeze().has_value());
  auto made = rt::event_channel<sample>::make(reg);
  REQUIRE(made.has_value());
  rt::event_channel<sample> &ch = *made;
  const rt::writer_key w{.tick = 1, .ph = rt::phase::tick, .system_index = 0, .chunk_index = 0};

  const std::vector<sample> invalid{
      {.id = 1, .speed = std::numeric_limits<float>::quiet_NaN(), .distance = 0.0},
      {.id = 2, .speed = std::numeric_limits<float>::infinity(), .distance = 0.0},
      {.id = 3, .speed = 0.0F, .distance = -std::numeric_limits<double>::infinity()},
      {.id = 4, .speed = 0.0F, .distance = std::numeric_limits<double>::signaling_NaN()},
  };
  for (const sample &s : invalid) {
    const auto wrote = ch.write(w, sample{s});
    const auto tried = ch.try_write(w, sample{s});
    if constexpr (rt::event_validation) {
      REQUIRE_FALSE(wrote.has_value());
      CHECK(wrote.error().code == sl::errc::event_payload_invalid);
      REQUIRE_FALSE(tried.has_value());
      CHECK(tried.error().code == sl::errc::event_payload_invalid);
    } else {
      CHECK(wrote.has_value());
      CHECK(tried.has_value());
    }
  }
  if constexpr (rt::event_validation) {
    CHECK(delivered(ch) == 0);
  } else {
    CHECK(delivered(ch) == 2 * invalid.size());
  }
  CHECK(ch.dropped() == 0);
}

TEST_CASE("RT-INV-EVT-06: finite payloads pass validation") {
  rt::event_registry reg;
  REQUIRE(reg.add<sample>("test.sample", sample_info()).has_value());
  REQUIRE(reg.freeze().has_value());
  auto made = rt::event_channel<sample>::make(reg);
  REQUIRE(made.has_value());
  rt::event_channel<sample> &ch = *made;
  const rt::writer_key w{.tick = 1, .ph = rt::phase::tick, .system_index = 0, .chunk_index = 0};
  REQUIRE(ch.write(w, sample{.id = 1, .speed = -0.0F, .distance = -0.0}).has_value());
  REQUIRE(ch.write(w, sample{.id = 2,
                             .speed = std::numeric_limits<float>::max(),
                             .distance = std::numeric_limits<double>::lowest()})
              .has_value());
  REQUIRE(ch.try_write(w, sample{.id = 3,
                                 .speed = std::numeric_limits<float>::denorm_min(),
                                 .distance = std::numeric_limits<double>::denorm_min()})
              .has_value());
  CHECK(delivered(ch) == 3);
}
