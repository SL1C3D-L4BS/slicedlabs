// RT-INV-JOB-08 (SL-ARCH-003 ARCH3-R030, ADR-0024): under a saturating frame stream a background
// job still completes (the background floor: a worker polls the background queue first every
// aging_limit-th scheduling round), and background results reach the owner thread only through
// drain_results, in submission order: the results of a job wait while any background job
// submitted before it is in flight. post_result from a frame job is invalid_state;
// job_context::background and current_job_priority report the running job's priority.

#include "sl/core/error.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/sync.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace jobs = sl::runtime::jobs;
namespace rtt = sl::runtime::testing;
using namespace std::chrono_literals;

jobs::job_config config(std::uint32_t workers) {
  jobs::job_config c;
  c.workers = workers;
  return c;
}

// A background job that waits for `gate` (when given) and then posts `posts` results of `kind`,
// each one byte holding the post index.
jobs::job_desc poster(std::uint32_t kind, int posts, const rt::cpu_fence *gate,
                      std::atomic<int> &errors) {
  return {.fn =
              [kind, posts, gate, &errors](jobs::job_context &ctx) noexcept {
                if (gate != nullptr) {
                  gate->wait();
                }
                if (!ctx.background()) {
                  errors.fetch_add(1);
                }
                for (int i = 0; i < posts; ++i) {
                  const std::array bytes{static_cast<std::byte>(i)};
                  if (!ctx.post_result(kind, bytes)) {
                    errors.fetch_add(1);
                  }
                }
              },
          .prio = jobs::priority::background,
          .name = "poster"};
}

std::vector<jobs::background_result> drain(jobs::job_system &js) {
  std::vector<jobs::background_result> out;
  js.drain_results([&out](const jobs::background_result &r) { out.push_back(r); });
  return out;
}

void spin_for(std::chrono::microseconds d) {
  const auto end = std::chrono::steady_clock::now() + d;
  while (std::chrono::steady_clock::now() < end) {
  }
}

} // namespace

TEST_CASE("RT-INV-JOB-08: background results reach the owner only through drain_results in "
          "submission order") {
  rtt::scripted_platform plat;
  auto js = jobs::job_system::start(plat.get(), config(2));
  REQUIRE(js);
  std::atomic<int> errors{0};
  rt::cpu_fence first_gate(1);
  const jobs::job_handle first = (*js)->submit(poster(0, 2, &first_gate, errors));
  const jobs::job_handle second = (*js)->submit(poster(1, 3, nullptr, errors));
  REQUIRE((*js)->wait(second));
  // the second job completed, but the first (submitted earlier) is in flight: nothing yet
  CHECK(drain(**js).empty());
  first_gate.signal();
  REQUIRE((*js)->wait(first));
  const std::vector<jobs::background_result> got = drain(**js);
  REQUIRE(got.size() == 5);
  CHECK(got[0].kind == 0);
  CHECK(got[1].kind == 0);
  for (std::size_t i = 2; i < 5; ++i) {
    CHECK(got[i].kind == 1);
  }
  CHECK(got[0].job_seq == got[1].job_seq);
  CHECK(got[0].job_seq < got[2].job_seq);
  for (std::size_t i = 0; i < 5; ++i) {
    const std::uint32_t expected =
        i < 2 ? static_cast<std::uint32_t>(i) : static_cast<std::uint32_t>(i - 2);
    CHECK(got[i].post_index == expected);
    REQUIRE(got[i].bytes.size() == 1);
    CHECK(got[i].bytes[0] == static_cast<std::byte>(expected));
  }
  CHECK(drain(**js).empty()); // delivered once
  CHECK(errors.load() == 0);
  REQUIRE((*js)->stop());
}

TEST_CASE("RT-INV-JOB-08: under a saturating frame stream a background job completes") {
  rtt::scripted_platform plat;
  auto js = jobs::job_system::start(plat.get(), config(2));
  REQUIRE(js);
  std::atomic<int> errors{0};
  std::atomic<bool> background_done{false};
  std::atomic<std::uint32_t> frame_runs{0};
  std::atomic<std::uint32_t> runs_at_background{0};
  jobs::job_system *sys = js->get();
  const jobs::job_handle bg =
      (*js)->submit({.fn =
                         [sys, &errors, &background_done, &frame_runs,
                          &runs_at_background](jobs::job_context &ctx) noexcept {
                           if (sys->current_job_priority() != jobs::priority::background) {
                             errors.fetch_add(1);
                           }
                           const std::array bytes{std::byte{7}};
                           if (!ctx.post_result(9, bytes)) {
                             errors.fetch_add(1);
                           }
                           runs_at_background.store(frame_runs.load());
                           background_done.store(true);
                         },
                     .prio = jobs::priority::background,
                     .name = "background"});
  // a pipelined stream: batch k+1 is queued before the owner waits for batch k, so the frame
  // queue never drains while the background job is pending
  constexpr int batch = 256;
  std::array<rt::job_counter, 2> counters;
  const auto frame = [&](rt::job_counter &c) {
    for (int i = 0; i < batch; ++i) {
      sys->submit({.fn =
                       [sys, &frame_runs, &errors](jobs::job_context &ctx) noexcept {
                         if (ctx.background() ||
                             sys->current_job_priority() != jobs::priority::frame) {
                           errors.fetch_add(1);
                         }
                         if (ctx.post_result(1, {}).error().code != sl::errc::invalid_state) {
                           errors.fetch_add(1);
                         }
                         spin_for(20us);
                         frame_runs.fetch_add(1);
                       },
                   .name = "frame"},
                  {}, &c);
    }
  };
  frame(counters[0]);
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  std::size_t k = 0;
  int batches = 1;
  while (!background_done.load() && std::chrono::steady_clock::now() < deadline) {
    frame(counters.at((k + 1) % 2));
    ++batches;
    REQUIRE((*js)->wait(counters.at(k % 2)));
    ++k;
  }
  REQUIRE((*js)->wait(counters.at(k % 2)));
  CHECK(background_done.load());
  // the background job ran while frame jobs were still queued behind it
  CHECK(runs_at_background.load() < static_cast<std::uint32_t>(batches * batch));
  REQUIRE((*js)->wait(bg));
  CHECK_FALSE((*js)->current_job_priority().has_value());
  const std::vector<jobs::background_result> got = drain(**js);
  REQUIRE(got.size() == 1);
  CHECK(got[0].kind == 9);
  CHECK(got[0].post_index == 0);
  CHECK(errors.load() == 0);
  REQUIRE((*js)->stop());
}
