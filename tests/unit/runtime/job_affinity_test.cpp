// RT-INV-JOB-04 (SL-ARCH-003 ARCH3-R066, R071): main-affinity jobs run on the owner thread only:
// submitted from the owner or from inside worker jobs, with or without a worker-job predecessor,
// drained by run_main_affinity or inside an owner-thread wait; with workers = 0 every job runs on
// the owner thread. Each job records the thread it ran on.

#include "sl/core/error.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/sync.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace jobs = sl::runtime::jobs;
namespace rtt = sl::runtime::testing;

struct tally {
  std::thread::id owner = std::this_thread::get_id();
  std::atomic<std::uint32_t> main_runs{0};
  std::atomic<std::uint32_t> main_off_owner{0};
  std::atomic<std::uint32_t> any_runs{0};
  std::atomic<std::uint32_t> any_on_owner{0};
};

jobs::job_desc main_job(tally &t) {
  return {.fn =
              [&t](jobs::job_context &) noexcept {
                t.main_runs.fetch_add(1);
                if (std::this_thread::get_id() != t.owner) {
                  t.main_off_owner.fetch_add(1);
                }
              },
          .aff = jobs::affinity::main,
          .name = "main"};
}

jobs::job_desc any_job(tally &t) {
  return {.fn =
              [&t](jobs::job_context &) noexcept {
                t.any_runs.fetch_add(1);
                if (std::this_thread::get_id() == t.owner) {
                  t.any_on_owner.fetch_add(1);
                }
              },
          .name = "any"};
}

jobs::job_config config(std::uint32_t workers) {
  jobs::job_config c;
  c.workers = workers;
  return c;
}

} // namespace

TEST_CASE("RT-INV-JOB-04: main-affinity jobs run on the owner thread only") {
  rtt::scripted_platform plat;
  auto js = jobs::job_system::start(plat.get(), config(4));
  REQUIRE(js);
  tally t;
  rt::job_counter all;
  // from the owner, drained by run_main_affinity
  for (int i = 0; i < 100; ++i) {
    (*js)->submit(main_job(t), {}, &all);
  }
  (*js)->run_main_affinity();
  CHECK(t.main_runs.load() == 100);
  // from inside worker jobs, each after a worker-job predecessor; drained by the owner's wait
  jobs::job_system *sys = js->get();
  for (int i = 0; i < 50; ++i) {
    (*js)->submit({.fn =
                       [sys, &t, &all](jobs::job_context &) noexcept {
                         const jobs::job_handle pred = sys->submit(any_job(t), {}, &all);
                         const std::array after{jobs::dependency::on(pred)};
                         sys->submit(main_job(t), after, &all);
                       },
                   .name = "spawner"},
                  {}, &all);
  }
  REQUIRE((*js)->wait(all));
  CHECK(t.main_runs.load() == 150);
  CHECK(t.any_runs.load() == 50);
  CHECK(t.main_off_owner.load() == 0);
  // a worker job waiting on a main-affinity job: the owner runs it inside its own wait
  const jobs::job_handle outer = (*js)->submit({.fn =
                                                    [sys, &t](jobs::job_context &ctx) noexcept {
                                                      if (!sys->wait(sys->submit(main_job(t)))) {
                                                        ctx.fail(sl::errc::internal);
                                                      }
                                                    },
                                                .name = "outer"});
  REQUIRE((*js)->wait(outer));
  CHECK(t.main_runs.load() == 151);
  CHECK(t.main_off_owner.load() == 0);
  REQUIRE((*js)->stop());
}

TEST_CASE("RT-INV-JOB-04: with workers = 0 every job runs on the owner thread") {
  rtt::scripted_platform plat;
  auto js = jobs::job_system::start(plat.get(), config(0));
  REQUIRE(js);
  CHECK((*js)->worker_count() == 0);
  tally t;
  rt::job_counter all;
  for (int i = 0; i < 64; ++i) {
    (*js)->submit(i % 2 == 0 ? main_job(t) : any_job(t), {}, &all);
  }
  REQUIRE((*js)->wait(all));
  CHECK(t.main_runs.load() == 32);
  CHECK(t.main_off_owner.load() == 0);
  CHECK(t.any_runs.load() == 32);
  CHECK(t.any_on_owner.load() == 32);
  REQUIRE((*js)->stop());
}
