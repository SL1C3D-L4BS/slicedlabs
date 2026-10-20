// RT-INV-SYNC-01 (SL-ARCH-003 ARCH3-R059, ADR-0024): a cpu_fence signalled from N threads releases
// all M waiters exactly once, holds them until the last signal, and a later signal has no effect.
// Every waiter runs in its own std::thread and must finish within 5 s (std::future::wait_for);
// the seeded sweep draws N, M and the signal sizes from std::mt19937_64 and prints the seed on
// failure.

#include "sl/runtime/sync.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <random>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace rt = sl::runtime;
using namespace std::chrono_literals;

constexpr auto wait_bound = 5s;

// A std::thread whose completion is observed through a future with a bounded wait.
class bounded_thread {
public:
  template <class Body> explicit bounded_thread(Body body) {
    std::promise<void> finished;
    done_ = finished.get_future();
    thread_ = std::jthread([run = std::move(body), finished = std::move(finished)] mutable {
      run();
      finished.set_value();
    });
  }

  [[nodiscard]] bool finished_within(std::chrono::milliseconds bound) const {
    return done_.wait_for(bound) == std::future_status::ready;
  }

private:
  std::future<void> done_;
  std::jthread thread_;
};

// Waits on `fence` from `waiters` threads and signals it from `signallers` threads; `amount(i)`
// is the signal size of signaller i. Returns true when every waiter finished within the bound;
// `released` counts the waiters that returned from wait().
template <class Amount>
bool run_fence(rt::cpu_fence &fence, std::size_t waiters, std::size_t signallers, Amount amount,
               std::atomic<std::uint32_t> &released) {
  std::vector<bounded_thread> waiting;
  waiting.reserve(waiters);
  for (std::size_t w = 0; w < waiters; ++w) {
    waiting.emplace_back([&fence, &released] {
      fence.wait();
      released.fetch_add(1);
    });
  }
  {
    std::vector<std::jthread> signalling;
    signalling.reserve(signallers);
    for (std::size_t s = 0; s < signallers; ++s) {
      signalling.emplace_back([&fence, n = amount(s)] { fence.signal(n); });
    }
  }
  bool all = true;
  for (const bounded_thread &t : waiting) {
    all = t.finished_within(wait_bound) && all;
  }
  return all;
}

} // namespace

TEST_CASE("RT-INV-SYNC-01: a cpu_fence signalled from 8 threads releases 8 waiters exactly once") {
  rt::cpu_fence fence(8);
  std::atomic<std::uint32_t> released{0};
  REQUIRE(run_fence(fence, 8, 8, [](std::size_t) { return std::uint32_t{1}; }, released));
  CHECK(released.load() == 8);
  CHECK(fence.is_signalled());
}

TEST_CASE("RT-INV-SYNC-01: a cpu_fence holds its waiters until the last signal") {
  rt::cpu_fence fence(3);
  std::atomic<std::uint32_t> released{0};
  std::vector<bounded_thread> waiting;
  waiting.reserve(4);
  for (int w = 0; w < 4; ++w) {
    waiting.emplace_back([&fence, &released] {
      fence.wait();
      released.fetch_add(1);
    });
  }
  fence.signal();
  fence.signal();
  std::this_thread::sleep_for(20ms);
  CHECK_FALSE(fence.is_signalled());
  CHECK(released.load() == 0);
  fence.signal();
  for (const bounded_thread &t : waiting) {
    CHECK(t.finished_within(wait_bound));
  }
  CHECK(released.load() == 4);
}

TEST_CASE("RT-INV-SYNC-01: a later signal has no effect and a later wait returns at once") {
  rt::cpu_fence fence(2);
  fence.signal(2);
  REQUIRE(fence.is_signalled());
  fence.signal();
  fence.signal(1000);
  CHECK(fence.is_signalled());
  std::atomic<std::uint32_t> released{0};
  REQUIRE(run_fence(fence, 3, 3, [](std::size_t) { return std::uint32_t{1}; }, released));
  CHECK(released.load() == 3);
  CHECK(fence.is_signalled());
}

TEST_CASE("RT-INV-SYNC-01: a cpu_fence of count 0 is released from the start") {
  const rt::cpu_fence fence(0);
  CHECK(fence.is_signalled());
  bounded_thread waiter([&fence] { fence.wait(); });
  CHECK(waiter.finished_within(wait_bound));
}

TEST_CASE("RT-INV-SYNC-01: seeded fences release every waiter exactly once") {
  constexpr std::uint64_t base_seed = 0x5EED0870001ULL;
  for (std::uint64_t round = 0; round < 16; ++round) {
    const std::uint64_t seed = base_seed + round;
    CAPTURE(seed);
    std::mt19937_64 rng(seed);
    const std::size_t signallers = 1 + static_cast<std::size_t>(rng() % 8);
    const std::size_t waiters = 1 + static_cast<std::size_t>(rng() % 8);
    std::vector<std::uint32_t> amounts(signallers);
    std::uint32_t total = 0;
    for (std::uint32_t &a : amounts) {
      a = 1 + static_cast<std::uint32_t>(rng() % 3);
      total += a;
    }
    // Over-signalling (count below the sum) is clamped; every signal past zero is ignored.
    const std::uint32_t count = 1 + static_cast<std::uint32_t>(rng() % total);
    rt::cpu_fence fence(count);
    std::atomic<std::uint32_t> released{0};
    REQUIRE(run_fence(
        fence, waiters, signallers, [&amounts](std::size_t i) { return amounts[i]; }, released));
    CHECK(released.load() == waiters);
    CHECK(fence.is_signalled());
  }
}
