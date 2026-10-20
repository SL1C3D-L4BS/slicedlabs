// RT-INV-ALLOC-06 (SL-ARCH-003 ARCH3-R049, assertions list, ADR-0024): an owner-thread resource
// used from another thread trips the captured owner assertion. The intruder runs while the owner
// waits in join, so the only finding is the assertion; synchronized_resource lifts the check. The
// owner check is SL_DEBUG_ASSERT: the cases skip with a message under NDEBUG.

#include "sl/core/assert.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/memory.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>

namespace {

#ifndef NDEBUG
namespace mem = sl::runtime::mem;

struct capture {
  std::atomic<int> fired{0};
  std::mutex mutex;
  std::string message;
};

// Allocates and releases 64 bytes of `r` on another thread and joins it.
void use_from_another_thread(mem::resource &r) {
  std::thread intruder([&r] {
    auto got = r.try_allocate(64, 16, r.default_tag());
    if (got.has_value()) {
      r.release(*got, 64, 16);
    }
  });
  intruder.join();
}

// Runs use_from_another_thread under a capturing handler; returns how often it fired.
int intrude(mem::resource &r, std::string *message = nullptr) {
  capture c;
  {
    const sl::scoped_assert_handler h([&c](const sl::assert_report &a) {
      c.fired.fetch_add(1);
      const std::scoped_lock lock(c.mutex);
      c.message = std::string(a.message);
    });
    use_from_another_thread(r);
  }
  if (message != nullptr) {
    *message = c.message;
  }
  return c.fired.load();
}

#endif

} // namespace

TEST_CASE("RT-INV-ALLOC-06: arena, stack, pool and frame_arena trip the owner assertion from "
          "another thread") {
#ifdef NDEBUG
  MESSAGE("skipped: the owner check is a Debug assertion");
#else
  mem::tracking_resource upstream("upstream");
  mem::arena_resource arena(upstream);
  mem::stack_resource stack(upstream, 65536);
  mem::pool_resource pool(upstream, 64, 16);
  mem::frame_arena frames(upstream, 2, 4096);
  std::string message;
  CHECK(arena.owner() == std::this_thread::get_id());
  CHECK(intrude(arena, &message) >= 1);
  CHECK(message.find("another thread") != std::string::npos);
  CHECK(intrude(stack) >= 1);
  CHECK(intrude(pool) >= 1);
  CHECK(intrude(frames) >= 1);
  CHECK(intrude(frames.for_worker(2)) >= 1); // the owner arena is bound to the owner thread
  // the owner itself never trips it
  int fired = 0;
  {
    const sl::scoped_assert_handler h([&fired](const sl::assert_report &) { ++fired; });
    auto got = arena.try_allocate(32, 16, arena.default_tag());
    REQUIRE(got.has_value());
    arena.release(*got, 32, 16);
    frames.flip();
  }
  CHECK(fired == 0);
#endif
}

TEST_CASE("RT-INV-ALLOC-06: a worker arena used by a second thread in one frame trips the "
          "assertion") {
#ifdef NDEBUG
  MESSAGE("skipped: the owner check is a Debug assertion");
#else
  mem::tracking_resource upstream("upstream");
  mem::frame_arena frames(upstream, 1, 4096);
  // the first worker thread binds worker 0 and stays alive, so the second gets another thread id
  // atomics, not std::latch: ThreadSanitizer sees through std::atomic but not the latch's wait
  std::atomic<int> stage{0};
  std::atomic<bool> first_ok{false};
  std::thread first([&frames, &stage, &first_ok] {
    auto got = frames.for_worker(0).try_allocate(64, 16, frames.default_tag());
    first_ok.store(got.has_value());
    stage.store(1);
    while (stage.load() != 2) {
      std::this_thread::yield();
    }
  });
  while (stage.load() != 1) {
    std::this_thread::yield();
  }
  CHECK(first_ok.load());
  CHECK(intrude(frames.for_worker(0)) >= 1); // a second thread in the same frame trips it
  stage.store(2);
  first.join();
#endif
}

TEST_CASE("RT-INV-ALLOC-06: synchronized_resource lifts the owner check of what it wraps") {
#ifdef NDEBUG
  MESSAGE("skipped: the owner check is a Debug assertion");
#else
  mem::tracking_resource upstream("upstream");
  mem::pool_resource pool(upstream, 64, 16);
  mem::arena_resource arena(upstream);
  mem::synchronized_resource over_pool(pool);
  mem::synchronized_resource over_arena(arena);
  CHECK(pool.shared());
  CHECK(intrude(over_pool) == 0);
  CHECK(intrude(over_arena) == 0);
  mem::general_resource general(&upstream);
  CHECK(intrude(general) == 0); // any-thread over a thread-safe upstream
#endif
}
