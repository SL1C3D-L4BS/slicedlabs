// RT-INV-JOB-05 (SL-ARCH-003 ARCH3-R067, `failpoints` RFP2 and RFP3, ADR-0009): job_context::fail
// cancels exactly the transitive dependents of the failed job (job_cancelled, their bodies never
// run) and its waiter receives job_failed naming the code; unrelated jobs and jobs whose
// predecessors all succeeded still run. fail_fatal and RFP3 reach the fault sink (on_fatal) and
// the worker runs the next job; RFP2 fails a job without the sink. Failpoint cases are skipped
// when the failpoints are compiled out (Release).

#include "sl/core/error.hpp"
#include "sl/runtime/faults.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/sync.hpp"

#include "../../../engine/runtime/testing/faults_arm.hpp"
#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace jobs = sl::runtime::jobs;
namespace rtt = sl::runtime::testing;

struct flags {
  std::array<std::atomic<int>, 32> ran{};
};

jobs::job_desc mark(flags &f, int i, sl::errc fail_with = sl::errc::ok) {
  return {.fn =
              [&f, i, fail_with](jobs::job_context &ctx) noexcept {
                f.ran.at(static_cast<std::size_t>(i)).fetch_add(1);
                if (fail_with != sl::errc::ok) {
                  ctx.fail(fail_with);
                }
              },
          .name = "mark"};
}

struct fatal_sink {
  std::mutex mutex;
  std::vector<jobs::fatal_report> reports;
};

jobs::job_config config(std::uint32_t workers, fatal_sink *sink = nullptr) {
  jobs::job_config c;
  c.workers = workers;
  if (sink != nullptr) {
    c.on_fatal = [sink](const jobs::fatal_report &r) {
      const std::scoped_lock lock(sink->mutex);
      sink->reports.push_back(r);
    };
  }
  return c;
}

} // namespace

TEST_CASE("RT-INV-JOB-05: fail cancels exactly the transitive dependents and the waiter "
          "receives job_failed") {
  rtt::scripted_platform plat;
  auto js = jobs::job_system::start(plat.get(), config(2));
  REQUIRE(js);
  flags f;
  rt::cpu_fence go(1);
  // a gate so the whole graph is submitted before anything runs
  const std::array after_go{jobs::dependency::on(go)};
  const jobs::job_handle a = (*js)->submit(mark(f, 0, sl::errc::invalid_argument), after_go);
  const jobs::job_handle e = (*js)->submit(mark(f, 1), after_go);
  const jobs::job_handle b = (*js)->submit(mark(f, 2), std::array{jobs::dependency::on(a)});
  const jobs::job_handle c = (*js)->submit(mark(f, 3), std::array{jobs::dependency::on(b)});
  const jobs::job_handle d =
      (*js)->submit(mark(f, 4), std::array{jobs::dependency::on(e), jobs::dependency::on(a)});
  const jobs::job_handle g = (*js)->submit(mark(f, 5), std::array{jobs::dependency::on(e)});
  // ten more direct dependents of a: past the four fixed slots, through the overflow chain
  std::vector<jobs::job_handle> fan;
  fan.reserve(10);
  for (int i = 0; i < 10; ++i) {
    fan.push_back((*js)->submit(mark(f, 10 + i), std::array{jobs::dependency::on(a)}));
  }
  const jobs::job_handle h = (*js)->submit(mark(f, 6), std::array{jobs::dependency::on(fan[9])});
  go.signal();

  auto failed = (*js)->wait(a);
  REQUIRE_FALSE(failed);
  CHECK(failed.error().code == sl::errc::job_failed);
  CHECK(failed.error().message.find("invalid_argument") != std::string::npos);
  for (const jobs::job_handle cancelled : {b, c, d, h}) {
    auto r = (*js)->wait(cancelled);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == sl::errc::job_cancelled);
  }
  for (const jobs::job_handle cancelled : fan) {
    auto r = (*js)->wait(cancelled);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == sl::errc::job_cancelled);
  }
  CHECK((*js)->wait(e));
  CHECK((*js)->wait(g));
  CHECK(f.ran[0].load() == 1);
  CHECK(f.ran[1].load() == 1);
  CHECK(f.ran[5].load() == 1);
  for (const int i : {2, 3, 4, 6}) {
    CHECK(f.ran.at(static_cast<std::size_t>(i)).load() == 0);
  }
  for (int i = 10; i < 20; ++i) {
    CHECK(f.ran.at(static_cast<std::size_t>(i)).load() == 0);
  }
  CHECK((*js)->status(a).value() == jobs::job_status::failed);
  CHECK((*js)->status(c).value() == jobs::job_status::cancelled);
  CHECK((*js)->status(g).value() == jobs::job_status::done);
  const jobs::job_stats s = (*js)->stats();
  CHECK(s.failed == 1);
  CHECK(s.cancelled == 14);
  // a dependent submitted after its predecessor failed is cancelled too
  auto late = (*js)->wait((*js)->submit(mark(f, 7), std::array{jobs::dependency::on(a)}));
  REQUIRE_FALSE(late);
  CHECK(late.error().code == sl::errc::job_cancelled);
  CHECK(f.ran[7].load() == 0);
  REQUIRE((*js)->stop());
}

TEST_CASE("RT-INV-JOB-05: fail_fatal reaches the fault sink and the worker runs the next job") {
  rtt::scripted_platform plat;
  fatal_sink sink;
  auto js = jobs::job_system::start(plat.get(), config(1, &sink));
  REQUIRE(js);
  std::atomic<int> after{0};
  std::thread::id fatal_thread{};
  std::thread::id next_thread{};
  const jobs::job_handle bad = (*js)->submit({.fn =
                                                  [&fatal_thread](jobs::job_context &ctx) noexcept {
                                                    fatal_thread = std::this_thread::get_id();
                                                    ctx.fail_fatal(sl::errc::out_of_memory);
                                                  },
                                              .name = "fatal"});
  auto r = (*js)->wait(bad);
  REQUIRE_FALSE(r);
  CHECK(r.error().code == sl::errc::job_failed);
  CHECK(r.error().message.find("out_of_memory") != std::string::npos);
  const jobs::job_handle next =
      (*js)->submit({.fn =
                         [&after, &next_thread](jobs::job_context &) noexcept {
                           next_thread = std::this_thread::get_id();
                           after.fetch_add(1);
                         },
                     .name = "next"});
  REQUIRE((*js)->wait(next));
  CHECK(after.load() == 1);
  {
    const std::scoped_lock lock(sink.mutex);
    REQUIRE(sink.reports.size() == 1);
    CHECK(sink.reports[0].code == sl::errc::out_of_memory);
    CHECK(sink.reports[0].name == "fatal");
    CHECK(sink.reports[0].job == bad);
  }
  CHECK((*js)->stats().fatal == 1);
  REQUIRE((*js)->stop());
}

TEST_CASE("RT-INV-JOB-05: RFP3 reaches the fault sink, RFP2 fails without it, and the worker "
          "runs the next job") {
  if (!rt::faults_compiled) {
    MESSAGE("failpoints compiled out (Release): RFP2 and RFP3 are not visited");
    return;
  }
  rtt::scripted_platform plat;
  fatal_sink sink;
  auto js = jobs::job_system::start(plat.get(), config(1, &sink));
  REQUIRE(js);
  flags f;
  {
    const rtt::scoped_arm fatal(rt::failpoint::rfp3, 1);
    REQUIRE(fatal.status());
    const jobs::job_handle j = (*js)->submit(mark(f, 0));
    const jobs::job_handle dep = (*js)->submit(mark(f, 1), std::array{jobs::dependency::on(j)});
    auto r = (*js)->wait(j);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == sl::errc::job_failed);
    CHECK((*js)->wait(dep).error().code == sl::errc::job_cancelled);
  }
  CHECK(f.ran[0].load() == 0); // the body was not run
  CHECK(f.ran[1].load() == 0);
  {
    const std::scoped_lock lock(sink.mutex);
    REQUIRE(sink.reports.size() == 1);
    CHECK(sink.reports[0].code == sl::errc::job_failed);
  }
  {
    const rtt::scoped_arm error(rt::failpoint::rfp2, 1);
    REQUIRE(error.status());
    auto r = (*js)->wait((*js)->submit(mark(f, 2)));
    REQUIRE_FALSE(r);
    CHECK(r.error().code == sl::errc::job_failed);
  }
  CHECK(f.ran[2].load() == 0);
  CHECK((*js)->wait((*js)->submit(mark(f, 3))));
  CHECK(f.ran[3].load() == 1);
  {
    const std::scoped_lock lock(sink.mutex);
    CHECK(sink.reports.size() == 1); // RFP2 does not reach the sink
  }
  REQUIRE((*js)->stop());
}

TEST_CASE("RT-INV-JOB-05: stop cancels jobs that have not started") {
  rtt::scripted_platform plat;
  auto js = jobs::job_system::start(plat.get(), config(1));
  REQUIRE(js);
  flags f;
  rt::cpu_fence never(1);
  const jobs::job_handle held = (*js)->submit(mark(f, 0), std::array{jobs::dependency::on(never)});
  const jobs::job_handle dep = (*js)->submit(mark(f, 1), std::array{jobs::dependency::on(held)});
  REQUIRE((*js)->stop());
  CHECK((*js)->wait(held).error().code == sl::errc::job_cancelled);
  CHECK((*js)->wait(dep).error().code == sl::errc::job_cancelled);
  CHECK(f.ran[0].load() == 0);
  CHECK(f.ran[1].load() == 0);
}
