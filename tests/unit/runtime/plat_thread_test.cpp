// RT-INV-PLAT-04 (SL-ARCH-003 ARCH3-R037, R040, RFP1): thread_factory names threads
// slrt<instance>-w<k>; a spawn failing under RFP1 is thread_spawn_failed with no thread left;
// handles release in reverse acquisition order. Threads left behind are counted from
// /proc/self/task, read until the count is stable (an exiting thread may linger for a moment
// after its join returned).

#include "sl/core/error.hpp"
#include "sl/runtime/faults.hpp"
#include "sl/runtime/headless_platform.hpp"
#include "sl/runtime/platform.hpp"

#include "../../../engine/runtime/testing/faults_arm.hpp"
#include "../../../engine/runtime/testing/scripted_platform.hpp"

#include <doctest/doctest.h>

#include <pthread.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace rtt = sl::runtime::testing;
using sl::runtime::failpoint;

std::size_t task_count() {
  return static_cast<std::size_t>(
      std::distance(std::filesystem::directory_iterator("/proc/self/task"),
                    std::filesystem::directory_iterator{}));
}

// The task count once it has not changed for 20 consecutive reads 1 ms apart (at most 2 s).
std::size_t settled_task_count() {
  std::size_t last = task_count();
  int stable = 0;
  for (int i = 0; i < 2000 && stable < 20; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const std::size_t now = task_count();
    stable = now == last ? stable + 1 : 0;
    last = now;
  }
  return last;
}

std::string os_name_of_self() {
  std::array<char, 16> buf{};
  static_cast<void>(pthread_getname_np(pthread_self(), buf.data(), buf.size()));
  return {buf.data()};
}

} // namespace

static_assert(!std::is_copy_constructible_v<rt::thread_handle>);
static_assert(!std::is_copy_assignable_v<rt::thread_handle>);
static_assert(std::is_nothrow_move_constructible_v<rt::thread_handle>);
static_assert(std::is_nothrow_move_assignable_v<rt::thread_handle>);
static_assert(!std::is_copy_constructible_v<rt::thread_group>);
static_assert(std::is_nothrow_move_constructible_v<rt::thread_group>);

TEST_CASE("RT-INV-PLAT-04: thread_factory names threads slrt<instance>-w<k>") {
  rt::os_thread_factory factory(7);
  std::array<std::string, 4> os_names{};
  std::vector<rt::thread_handle> handles;
  for (std::size_t k = 0; k < os_names.size(); ++k) {
    std::string *slot = &os_names.at(k);
    auto h = factory.spawn(
        "w" + std::to_string(k), [slot] { *slot = os_name_of_self(); },
        rt::affinity_hint{.core = static_cast<std::int32_t>(k)});
    REQUIRE(h.has_value());
    CHECK(h->joinable());
    CHECK(h->name() == "slrt7-w" + std::to_string(k));
    handles.push_back(std::move(*h));
  }
  for (rt::thread_handle &h : handles) {
    h.join();
    CHECK_FALSE(h.joinable());
  }
  for (std::size_t k = 0; k < os_names.size(); ++k) {
    CHECK(os_names.at(k) == "slrt7-w" + std::to_string(k));
  }

  // the OS name is the first 15 bytes; name() keeps the whole name
  rt::os_thread_factory wide(4000000000U);
  std::string long_name;
  auto h = wide.spawn("w12", [&long_name] { long_name = os_name_of_self(); });
  REQUIRE(h.has_value());
  CHECK(h->name() == "slrt4000000000-w12");
  h->join();
  CHECK(long_name == "slrt4000000000-");

  auto empty = factory.spawn("", [] {});
  REQUIRE_FALSE(empty.has_value());
  CHECK(empty.error().code == sl::errc::invalid_argument);
}

TEST_CASE("RT-INV-PLAT-04: a spawn failing under RFP1 is thread_spawn_failed with no thread left") {
  if (!rt::faults_compiled) {
    CHECK(rtt::failing_thread_factory::arm_rfp1(1).error().code == sl::errc::unsupported);
    return;
  }
  rtt::disarm_all();
  const std::size_t before = settled_task_count();
  rt::os_thread_factory factory(2);
  std::atomic<bool> ran{false};
  {
    const rtt::scoped_arm armed(failpoint::rfp1, 1);
    REQUIRE(armed.status().has_value());
    auto h = factory.spawn("w0", [&ran] { ran.store(true); });
    REQUIRE_FALSE(h.has_value());
    CHECK(h.error().code == sl::errc::thread_spawn_failed);
  }
  CHECK_FALSE(ran.load());
  CHECK(settled_task_count() == before);

  // the scripted factory: RFP1 at the third spawn of a group of four, then a scripted failure
  rtt::failing_thread_factory scripted(2);
  std::atomic<int> runs{0};
  {
    rt::thread_group group;
    REQUIRE(rtt::failing_thread_factory::arm_rfp1(3).has_value());
    int failures = 0;
    for (int k = 0; k < 4; ++k) {
      auto r = group.spawn(scripted, "w" + std::to_string(k), [&runs] { runs.fetch_add(1); });
      if (!r) {
        CHECK(r.error().code == sl::errc::thread_spawn_failed);
        ++failures;
      }
    }
    rtt::disarm_all();
    CHECK(failures == 1);
    CHECK(group.size() == 3);
    scripted.fail_at(2);
    auto second = scripted.spawn("w4", [&runs] { runs.fetch_add(1); });
    REQUIRE(second.has_value());
    second->join();
    auto failed = scripted.spawn("w5", [&runs] { runs.fetch_add(1); });
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == sl::errc::thread_spawn_failed);
  }
  CHECK(runs.load() == 4);
  CHECK(scripted.spawns() == 6);
  CHECK(settled_task_count() == before);
}

TEST_CASE("RT-INV-PLAT-04: handles release in reverse acquisition order") {
  rt::os_thread_factory factory(5);
  std::atomic<bool> go{false};
  std::atomic<int> finished{0};
  rt::thread_group group;
  for (int k = 0; k < 4; ++k) {
    REQUIRE(group
                .spawn(factory, "w" + std::to_string(k),
                       [&go, &finished] {
                         while (!go.load()) {
                           std::this_thread::yield();
                         }
                         finished.fetch_add(1);
                       })
                .has_value());
  }
  CHECK(group.size() == 4);
  go.store(true);
  const std::vector<std::string> order = group.release();
  CHECK(order == std::vector<std::string>{"slrt5-w3", "slrt5-w2", "slrt5-w1", "slrt5-w0"});
  CHECK(finished.load() == 4);
  CHECK(group.size() == 0);

  // a moved handle carries the thread; the destructor joins it
  std::atomic<bool> done{false};
  {
    auto h = factory.spawn("w9", [&done] {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      done.store(true);
    });
    REQUIRE(h.has_value());
    rt::thread_handle moved = std::move(*h);
    CHECK_FALSE(h->joinable());
    CHECK(h->name().empty());
    CHECK(moved.joinable());
  }
  CHECK(done.load());
}
