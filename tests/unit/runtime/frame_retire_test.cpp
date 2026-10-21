// RT-INV-FRAME-06 (SL-ARCH-003 ARCH3-R098, seam V07-2, `boundary_slots` retire_after): a
// retire_after(2) callback registered during frame 3 runs exactly at the end_frame of frame 5, on
// the owner thread, inside the retire_after slot (after the lifecycle_requests hooks and before
// the arena_flip hooks); one registered between frames runs `frames` frames after the current
// frame index; callbacks due in one frame run in registration order and a callback may register
// another. retire_after(0) and an empty callback are invalid_argument, another thread gets
// wrong_thread.

#include "sl/core/error.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/memory.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace jobs = sl::runtime::jobs;
namespace mem = sl::runtime::mem;
namespace rtt = sl::runtime::testing;

using sl::errc;

struct rig {
  rtt::scripted_platform plat;
  mem::tracking_resource upstream{"frame.test"};
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

} // namespace

TEST_CASE("RT-INV-FRAME-06: retire_after(2) runs at the end_frame two frames later") {
  rig r(2);
  const std::thread::id owner = std::this_thread::get_id();
  std::vector<std::string> log;
  std::uint32_t off_owner = 0;
  r.fs->add_boundary_hook(rt::boundary_slot::snapshot, [&](const rt::boundary_context &ctx) {
    if (ctx.frame_index == 3) {
      CHECK(r.fs->retire_after(2, [&] {
        off_owner += std::this_thread::get_id() == owner ? 0U : 1U;
        log.push_back("retired@" + std::to_string(r.fs->current().frame_index));
      }));
    }
  });
  r.fs->add_boundary_hook(rt::boundary_slot::lifecycle_requests,
                          [&](const rt::boundary_context &ctx) {
                            log.push_back("lifecycle@" + std::to_string(ctx.frame_index));
                          });
  r.fs->add_boundary_hook(rt::boundary_slot::arena_flip, [&](const rt::boundary_context &ctx) {
    log.push_back("flip@" + std::to_string(ctx.frame_index));
  });
  REQUIRE(r.fs->build());
  for (int f = 0; f < 7; ++f) {
    REQUIRE(r.fs->run_frame());
  }
  const std::vector<std::string> want{"lifecycle@1", "flip@1",      "lifecycle@2", "flip@2",
                                      "lifecycle@3", "flip@3",      "lifecycle@4", "flip@4",
                                      "lifecycle@5", "retired@5",   "flip@5",      "lifecycle@6",
                                      "flip@6",      "lifecycle@7", "flip@7"};
  CHECK(log == want);
  CHECK(off_owner == 0);
}

TEST_CASE("RT-INV-FRAME-06: between frames, order and re-registration") {
  rig r(1);
  REQUIRE(r.fs->build());
  std::vector<std::string> log;
  const auto at = [&](const std::string &name) {
    return [&log, &r, name] {
      log.push_back(name + "@" + std::to_string(r.fs->current().frame_index));
    };
  };
  REQUIRE(r.fs->run_frame()); // frame 1
  REQUIRE(r.fs->retire_after(1, at("one")));
  REQUIRE(r.fs->retire_after(2, at("two")));
  REQUIRE(r.fs->retire_after(1, at("one-b")));
  REQUIRE(r.fs->retire_after(1, [&] {
    log.push_back("chain@" + std::to_string(r.fs->current().frame_index));
    CHECK(r.fs->retire_after(1, at("chained")));
  }));
  for (int f = 0; f < 4; ++f) {
    REQUIRE(r.fs->run_frame()); // frames 2 to 5
  }
  const std::vector<std::string> want{"one@2", "one-b@2", "chain@2", "two@3", "chained@3"};
  CHECK(log == want);
}

TEST_CASE("RT-INV-FRAME-06: retire_after refuses 0 frames, an empty callback and other threads") {
  rig r(1);
  REQUIRE(r.fs->build());
  auto zero = r.fs->retire_after(0, [] {});
  REQUIRE_FALSE(zero.has_value());
  CHECK(zero.error().code == errc::invalid_argument);
  auto empty = r.fs->retire_after(1, std::function<void()>{});
  REQUIRE_FALSE(empty.has_value());
  CHECK(empty.error().code == errc::invalid_argument);
  errc code = errc::ok;
  std::thread other([&] {
    auto got = r.fs->retire_after(1, [] {});
    code = got ? errc::ok : got.error().code;
  });
  other.join();
  CHECK(code == errc::wrong_thread);
}
