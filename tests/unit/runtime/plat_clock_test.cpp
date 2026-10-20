// RT-INV-PLAT-03 (SL-ARCH-003 ARCH3-R079): clock_source never goes backward across 1e5 samples on
// each of 8 threads (scaled by SL_RUNTIME_STRESS_SCALE, 0.1 under the sanitizers, floored at 1).
// The threads come from the platform's own thread factory, and every sample is also checked
// against the samples another thread published before it.

#include "sl/runtime/headless_platform.hpp"
#include "sl/runtime/platform.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace {

namespace rt = sl::runtime;

constexpr std::size_t thread_count = 8;
constexpr double base_samples = 1e5;

std::uint64_t scaled(double base) {
  double scale = 1.0;
  if (const char *env = std::getenv("SL_RUNTIME_STRESS_SCALE"); env != nullptr && *env != '\0') {
    scale = std::strtod(env, nullptr);
  }
  return std::max<std::uint64_t>(1, static_cast<std::uint64_t>(base * scale));
}

} // namespace

TEST_CASE("RT-INV-PLAT-03: the steady clock_source never goes backward on 8 threads") {
  const rt::steady_clock_source clock;
  rt::os_thread_factory factory(1);
  const std::uint64_t samples = scaled(base_samples);
  std::atomic<std::uint64_t> backward{0};
  std::atomic<std::uint64_t> taken{0};
  std::atomic<rt::nanos::rep> latest{0}; // the newest sample any thread published
  {
    rt::thread_group group;
    for (std::size_t t = 0; t < thread_count; ++t) {
      auto body = [&clock, &backward, &taken, &latest, samples] noexcept {
        rt::nanos previous = clock.now();
        for (std::uint64_t i = 0; i < samples; ++i) {
          const rt::nanos seen_published{latest.load()};
          const rt::nanos now = clock.now();
          if (now < previous || now < seen_published) {
            backward.fetch_add(1);
          }
          previous = now;
          rt::nanos::rep expected = latest.load();
          while (expected < now.count() && !latest.compare_exchange_weak(expected, now.count())) {
          }
        }
        taken.fetch_add(samples);
      };
      REQUIRE(group.spawn(factory, "w" + std::to_string(t), body).has_value());
    }
  }
  CHECK(backward.load() == 0);
  CHECK(taken.load() == samples * thread_count);
}

TEST_CASE("RT-INV-PLAT-03: wall_clock is the calendar clock and is not the monotonic source") {
  const rt::system_wall_clock wall;
  const rt::steady_clock_source clock;
  const auto stamp = wall.now();
  CHECK(stamp.time_since_epoch().count() > 0);
  const rt::nanos a = clock.now();
  const rt::nanos b = clock.now();
  CHECK(b >= a);
}
