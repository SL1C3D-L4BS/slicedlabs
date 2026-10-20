// RT-INV-SYNC-03 (SL-ARCH-003 ARCH3-R057, ADR-0024): sl::runtime::counting_semaphore (the std
// counting semaphore) never admits more holders than its count under 16 threads. Each thread runs
// in a std::thread and must finish within 5 s (std::future::wait_for); the counts and the hold
// pattern come from std::mt19937_64 and the seed is printed on failure.

#include "sl/runtime/sync.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <random>
#include <semaphore>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace rt = sl::runtime;
using namespace std::chrono_literals;

constexpr auto wait_bound = 5s;
constexpr std::size_t thread_count = 16;

static_assert(std::is_same_v<rt::counting_semaphore, std::counting_semaphore<>>);

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

struct admission {
  std::atomic<std::uint32_t> holders{0};
  std::atomic<std::uint32_t> peak{0};
  std::atomic<std::uint64_t> entries{0};
};

void hold(rt::counting_semaphore &semaphore, admission &seen, std::uint64_t seed,
          std::uint32_t rounds) {
  std::mt19937_64 rng(seed);
  for (std::uint32_t r = 0; r < rounds; ++r) {
    semaphore.acquire();
    const std::uint32_t now = seen.holders.fetch_add(1) + 1;
    std::uint32_t peak = seen.peak.load();
    while (now > peak && !seen.peak.compare_exchange_weak(peak, now)) {
    }
    seen.entries.fetch_add(1);
    if (rng() % 4 == 0) {
      std::this_thread::yield();
    }
    seen.holders.fetch_sub(1);
    semaphore.release();
  }
}

} // namespace

TEST_CASE("RT-INV-SYNC-03: the counting semaphore admits at most its count under 16 threads") {
  constexpr std::uint64_t base_seed = 0x5EED0870003ULL;
  constexpr std::uint32_t rounds = 200;
  for (const std::ptrdiff_t count : {std::ptrdiff_t{1}, std::ptrdiff_t{3}, std::ptrdiff_t{5}}) {
    const std::uint64_t seed = base_seed + static_cast<std::uint64_t>(count);
    CAPTURE(count);
    CAPTURE(seed);
    rt::counting_semaphore semaphore(count);
    admission seen;
    {
      std::vector<bounded_thread> threads;
      threads.reserve(thread_count);
      for (std::size_t t = 0; t < thread_count; ++t) {
        threads.emplace_back(
            [&semaphore, &seen, s = seed + (t << 8U)] { hold(semaphore, seen, s, rounds); });
      }
      for (const bounded_thread &t : threads) {
        REQUIRE(t.finished_within(wait_bound));
      }
    }
    CHECK(seen.peak.load() >= 1);
    CHECK(seen.peak.load() <= static_cast<std::uint32_t>(count));
    CHECK(seen.holders.load() == 0);
    CHECK(seen.entries.load() == std::uint64_t{thread_count} * rounds);
  }
}

TEST_CASE("RT-INV-SYNC-03: a full counting semaphore refuses one more holder") {
  rt::counting_semaphore semaphore(2);
  REQUIRE(semaphore.try_acquire());
  REQUIRE(semaphore.try_acquire());
  CHECK_FALSE(semaphore.try_acquire());
  CHECK_FALSE(semaphore.try_acquire_for(10ms));
  semaphore.release();
  CHECK(semaphore.try_acquire());
  semaphore.release(2);
}
