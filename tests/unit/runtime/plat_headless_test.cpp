// RT-INV-PLAT-01 (SL-ARCH-003 ARCH3-R036, ADR-0006): headless_platform runs start, 100 frames and
// stop with no display, window or input device; the window reports headless. Start spawns one
// worker, each frame takes one wall sample and polls input, stop joins the worker. The display
// variables are removed first, so nothing can reach a display server.

#include "sl/core/error.hpp"
#include "sl/runtime/headless_platform.hpp"
#include "sl/runtime/platform.hpp"

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

namespace {

namespace rt = sl::runtime;

constexpr std::uint64_t frame_count = 100;

std::string work_dir(const char *leaf) {
  return std::string(SL_RUNTIME_EVIDENCE_ROOT) + "/work/" + leaf;
}

} // namespace

TEST_CASE("RT-INV-PLAT-01: headless_platform runs start, 100 frames and stop with no display") {
  ::unsetenv("DISPLAY");
  ::unsetenv("WAYLAND_DISPLAY");

  auto made = rt::make_headless_platform({.root_dir = work_dir("plat_headless"), .instance_id = 3});
  REQUIRE(made.has_value());
  rt::platform &p = (*made)->get();
  CHECK(p.instance_id == 3);
  CHECK(p.win.headless());
  CHECK(p.win.width() == 0);
  CHECK(p.win.height() == 0);

  // start: one worker that runs until stop
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> worker_spins{0};
  rt::thread_group workers;
  REQUIRE(workers
              .spawn(p.threads, "w0",
                     [&stop, &worker_spins]() noexcept {
                       while (!stop.load()) {
                         worker_spins.fetch_add(1);
                       }
                     })
              .has_value());

  std::array<rt::input_event, 8> buffer{};
  rt::nanos previous{0};
  std::uint64_t frames = 0;
  std::size_t events = 0;
  for (std::uint64_t frame = 0; frame < frame_count; ++frame) {
    const rt::nanos sample = rt::record_wall_sample(p.clock, p.sink, frame);
    CHECK(sample >= previous);
    previous = sample;
    events += rt::record_input(p.input, p.sink, frame, frame, buffer);
    ++frames;
  }

  // stop
  stop.store(true);
  const std::vector<std::string> released = workers.release();
  CHECK(frames == frame_count);
  CHECK(events == 0);
  REQUIRE(released.size() == 1);
  CHECK(released[0] == "slrt3-w0");
  CHECK(workers.size() == 0);
}

TEST_CASE("RT-INV-PLAT-01: headless files live under root_dir and wall_clock stamps are real") {
  auto made = rt::make_headless_platform({.root_dir = work_dir("plat_headless_files")});
  REQUIRE(made.has_value());
  rt::platform &p = (*made)->get();
  CHECK(p.files.root() == work_dir("plat_headless_files"));
  const std::array<std::byte, 3> bytes{std::byte{1}, std::byte{2}, std::byte{3}};
  REQUIRE(p.files.atomic_write("headless.bin", bytes).has_value());
  auto back = p.files.read("headless.bin");
  REQUIRE(back.has_value());
  CHECK(*back == std::vector<std::byte>(bytes.begin(), bytes.end()));
  CHECK(p.wall.now().time_since_epoch().count() > 0);
  CHECK(p.files.remove("headless.bin").has_value());
}

TEST_CASE("RT-INV-PLAT-01: make_headless_platform is platform_unavailable without a root_dir") {
  auto made = rt::make_headless_platform({.root_dir = "/dev/null/plat"});
  REQUIRE_FALSE(made.has_value());
  CHECK(made.error().code == sl::errc::platform_unavailable);
}
