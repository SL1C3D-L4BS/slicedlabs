// RT-INV-JOB-06 (SL-ARCH-003 ARCH3-R068, master plan section 3 F6): the sl::log correlation {id,
// scope, tick} seen inside a job equals the submitter's: for jobs submitted by the owner inside a
// correlation_scope, by another thread under its own scope, by a job (which runs under its
// submitter's correlation, so the child inherits it), for background and main-affinity jobs, and
// the process correlation for jobs submitted outside any scope. A worker's own correlation is
// restored after each job.

#include "sl/core/error.hpp"
#include "sl/core/log.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/sync.hpp"

#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <string_view>
#include <thread>

namespace {

namespace rt = sl::runtime;
namespace jobs = sl::runtime::jobs;
namespace rtt = sl::runtime::testing;

bool same(const sl::log::correlation &a, const sl::log::correlation &b) {
  return a.id == b.id && a.scope == b.scope && a.tick == b.tick;
}

struct probe {
  std::atomic<std::uint32_t> seen{0};
  std::atomic<std::uint32_t> mismatched{0};
};

// A job that compares the correlation it runs under with `expected` (copied into the body).
jobs::job_desc check_job(probe &p, const sl::log::correlation &expected,
                         jobs::priority prio = jobs::priority::frame,
                         jobs::affinity aff = jobs::affinity::any) {
  return {.fn =
              [&p, expected](jobs::job_context &) noexcept {
                p.seen.fetch_add(1);
                if (!same(sl::log::current_correlation(), expected)) {
                  p.mismatched.fetch_add(1);
                }
              },
          .prio = prio,
          .aff = aff,
          .name = "check"};
}

jobs::job_config config(std::uint32_t workers) {
  jobs::job_config c;
  c.workers = workers;
  return c;
}

} // namespace

TEST_CASE("RT-INV-JOB-06: the correlation id, scope and tick inside a job equal the "
          "submitter's") {
  rtt::scripted_platform plat;
  auto js = jobs::job_system::start(plat.get(), config(4));
  REQUIRE(js);
  jobs::job_system *sys = js->get();
  probe p;
  rt::job_counter all;
  {
    const sl::log::correlation_scope scope(
        sl::log::correlation{.id = 42, .scope = "runtime/7", .tick = 9001});
    const sl::log::correlation mine = sl::log::current_correlation();
    REQUIRE(mine.id == 42);
    for (int i = 0; i < 200; ++i) {
      (*js)->submit(check_job(p, mine), {}, &all);
    }
    (*js)->submit(check_job(p, mine, jobs::priority::background), {}, &all);
    (*js)->submit(check_job(p, mine, jobs::priority::frame, jobs::affinity::main), {}, &all);
    // a job that submits a child: the child runs under the parent's (the submitter's) correlation
    for (int i = 0; i < 50; ++i) {
      (*js)->submit({.fn =
                         [sys, &p, &all](jobs::job_context &) noexcept {
                           sys->submit(check_job(p, sl::log::current_correlation()), {}, &all);
                         },
                     .name = "parent"},
                    {}, &all);
    }
    REQUIRE((*js)->wait(all));
  }
  // another thread with its own scope
  {
    std::jthread other([&] {
      const sl::log::correlation_scope scope("runtime/other", 77);
      const sl::log::correlation theirs = sl::log::current_correlation();
      rt::job_counter mine;
      for (int i = 0; i < 100; ++i) {
        sys->submit(check_job(p, theirs), {}, &mine);
      }
      CHECK(sys->wait(mine));
    });
  }
  // outside any scope: the process correlation
  const sl::log::correlation process = sl::log::current_correlation();
  CHECK(process.id == 0);
  for (int i = 0; i < 50; ++i) {
    (*js)->submit(check_job(p, process), {}, &all);
  }
  REQUIRE((*js)->wait(all));
  CHECK(p.seen.load() == 200 + 2 + 50 + 100 + 50);
  CHECK(p.mismatched.load() == 0);
  REQUIRE((*js)->stop());
}

TEST_CASE("RT-INV-JOB-06: a worker's correlation is restored after each job") {
  rtt::scripted_platform plat;
  auto js = jobs::job_system::start(plat.get(), config(1));
  REQUIRE(js);
  std::atomic<std::uint64_t> outer_id{1};
  rt::cpu_fence done(1);
  // the first job runs a nested job under a different correlation through a helping wait, then
  // checks that its own correlation came back
  jobs::job_system *sys = js->get();
  {
    const sl::log::correlation_scope scope(
        sl::log::correlation{.id = 5, .scope = "runtime/a", .tick = 1});
    (*js)->submit({.fn =
                       [sys, &outer_id, &done](jobs::job_context &) noexcept {
                         jobs::job_handle inner{};
                         {
                           const sl::log::correlation_scope nested(
                               sl::log::correlation{.id = 6, .scope = "runtime/b", .tick = 2});
                           inner = sys->submit(
                               {.fn = [](jobs::job_context &) noexcept {}, .name = "inner"});
                         }
                         const bool waited = sys->wait(inner).has_value();
                         outer_id.store(waited ? sl::log::current_correlation().id : 0);
                         done.signal();
                       },
                   .name = "outer"});
  }
  done.wait();
  CHECK(outer_id.load() == 5);
  REQUIRE((*js)->stop());
}
