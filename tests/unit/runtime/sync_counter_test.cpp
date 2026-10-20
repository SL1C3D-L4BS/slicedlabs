// RT-INV-SYNC-02 (SL-ARCH-003 ARCH3-R059, ADR-0024): resetting a job_counter with a registered
// waiter is invalid_state and leaves the value unchanged; without a waiter it succeeds and sets
// the value to zero. Waiters run in std::threads and must finish within 5 s
// (std::future::wait_for).

#include "sl/core/error.hpp"
#include "sl/runtime/sync.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
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

// Polls until `counter` has `n` registered waiters or the bound passes.
bool waiters_reach(const rt::job_counter &counter, std::uint32_t n) {
  const auto deadline = std::chrono::steady_clock::now() + wait_bound;
  while (counter.waiters() != n) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

} // namespace

TEST_CASE("RT-INV-SYNC-02: resetting a job_counter without a waiter succeeds") {
  rt::job_counter counter;
  CHECK(counter.value() == 0);
  CHECK(counter.reset().has_value());
  counter.add(5);
  counter.sub(2);
  CHECK(counter.value() == 3);
  CHECK(counter.waiters() == 0);
  REQUIRE(counter.reset().has_value());
  CHECK(counter.value() == 0);
  counter.wait_zero(); // zero already: returns at once
}

TEST_CASE("RT-INV-SYNC-02: resetting a job_counter with a waiter is invalid_state") {
  rt::job_counter counter;
  counter.add(1);
  bounded_thread waiter([&counter] { counter.wait_zero(); });
  REQUIRE(waiters_reach(counter, 1));
  const sl::result<void> refused = counter.reset();
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().code == sl::errc::invalid_state);
  CHECK(counter.value() == 1);
  counter.sub(1);
  CHECK(waiter.finished_within(wait_bound));
  REQUIRE(waiters_reach(counter, 0));
  CHECK(counter.reset().has_value());
}

TEST_CASE("RT-INV-SYNC-02: wait_zero returns after 8 threads finish their work") {
  constexpr std::uint32_t threads = 8;
  constexpr std::uint32_t items = 500;
  rt::job_counter counter;
  counter.add(threads * items);
  std::vector<bounded_thread> waiting;
  waiting.reserve(3);
  for (int w = 0; w < 3; ++w) {
    waiting.emplace_back([&counter] { counter.wait_zero(); });
  }
  {
    std::vector<std::jthread> workers;
    workers.reserve(threads);
    for (std::uint32_t t = 0; t < threads; ++t) {
      workers.emplace_back([&counter] {
        for (std::uint32_t i = 0; i < items; ++i) {
          counter.sub();
        }
      });
    }
  }
  for (const bounded_thread &t : waiting) {
    CHECK(t.finished_within(wait_bound));
  }
  CHECK(counter.value() == 0);
  REQUIRE(waiters_reach(counter, 0));
  CHECK(counter.reset().has_value());
}
