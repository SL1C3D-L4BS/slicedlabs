// RT-INV-SYNC-05 (SL-ARCH-003 ARCH3-R058, R060, ADR-0024): publication through each primitive
// (cpu_fence, job_counter, counting_semaphore, event_count) makes every payload write before it
// visible after the matching wait: plain writes (a missing happens-before is a TSan report) and
// relaxed atomic writes (checked by value). Rounds are scaled by SL_RUNTIME_STRESS_SCALE (0.1
// under the sanitizers, floored at 1). A wait spins at most spin_limit times before parking:
// spin_wait stops at spin_limit, and a waiter blocked for 200 ms on each primitive uses far less
// thread CPU time than wall time (it parked instead of spinning). Every waiter runs in a
// std::thread and must finish within 5 s (std::future::wait_for).

#include "sl/runtime/sync.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <future>
#include <memory>
#include <thread>
#include <utility>

namespace {

namespace rt = sl::runtime;
using namespace std::chrono_literals;

constexpr auto wait_bound = 5s;
constexpr double base_rounds = 2000;
constexpr std::size_t plain_words = 64;
constexpr std::size_t relaxed_words = 16;

std::uint64_t scaled(double base) {
  double scale = 1.0;
  if (const char *env = std::getenv("SL_RUNTIME_STRESS_SCALE"); env != nullptr && *env != '\0') {
    scale = std::strtod(env, nullptr);
  }
  return std::max<std::uint64_t>(1, static_cast<std::uint64_t>(base * scale));
}

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

// The payload one producer writes before publishing and one consumer reads after waiting.
struct payload {
  std::array<std::uint64_t, plain_words> plain{};
  std::array<std::atomic<std::uint64_t>, relaxed_words> relaxed{};

  void write(std::uint64_t round) noexcept {
    std::uint64_t i = 0;
    for (std::uint64_t &word : plain) {
      word = (round * 1000) + i++;
    }
    i = 0;
    for (std::atomic<std::uint64_t> &word : relaxed) {
      // pairs: the primitive's publishing operation (signal, sub, release, notify_one)
      word.store((round * 2000) + i++, std::memory_order_relaxed);
    }
  }

  [[nodiscard]] bool matches(std::uint64_t round) const noexcept {
    bool ok = true;
    std::uint64_t i = 0;
    for (const std::uint64_t &word : plain) {
      ok = ok && word == (round * 1000) + i++;
    }
    i = 0;
    for (const std::atomic<std::uint64_t> &word : relaxed) {
      // pairs: the primitive's waiting operation (wait, wait_zero, acquire, wait(key))
      ok = ok && word.load(std::memory_order_relaxed) == (round * 2000) + i++;
    }
    return ok;
  }
};

// Runs `rounds` rounds of: consumer waits through `wait_fn`, producer writes the payload and
// publishes through `publish_fn`. `make` builds the round's primitive (and any pre-wait state).
// Returns the number of rounds whose payload did not match or whose waiter missed the bound.
template <class Make, class Wait, class Publish>
std::uint64_t publish_rounds(std::uint64_t rounds, Make make, Wait wait_fn, Publish publish_fn) {
  std::uint64_t bad = 0;
  for (std::uint64_t round = 1; round <= rounds; ++round) {
    payload data;
    auto primitive = make();
    bool seen = false;
    bounded_thread consumer([&] {
      wait_fn(*primitive);
      seen = data.matches(round);
    });
    {
      const std::jthread producer([&] {
        data.write(round);
        publish_fn(*primitive);
      });
    }
    if (!consumer.finished_within(wait_bound)) {
      ++bad;
      break;
    }
    if (!seen) {
      ++bad;
    }
  }
  return bad;
}

std::chrono::nanoseconds thread_cpu_time() noexcept {
  timespec now{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
  return std::chrono::seconds(now.tv_sec) + std::chrono::nanoseconds(now.tv_nsec);
}

// Blocks a waiter for about 200 ms on `wait_fn`, then calls `release_fn`; true when the waiter
// finished within the bound and used less than half of its wall time on the CPU.
template <class Wait, class Release> bool parks(Wait wait_fn, Release release_fn) {
  constexpr auto blocked = 200ms;
  std::chrono::nanoseconds cpu{};
  std::chrono::nanoseconds wall{};
  bounded_thread waiter([&] {
    const auto cpu_start = thread_cpu_time();
    const auto wall_start = std::chrono::steady_clock::now();
    wait_fn();
    cpu = thread_cpu_time() - cpu_start;
    wall = std::chrono::steady_clock::now() - wall_start;
  });
  std::this_thread::sleep_for(blocked);
  release_fn();
  const bool done = waiter.finished_within(wait_bound);
  CAPTURE(cpu.count());
  CAPTURE(wall.count());
  return done && wall >= blocked / 2 && cpu * 2 < wall;
}

} // namespace

TEST_CASE("RT-INV-SYNC-05: cpu_fence publishes the payload written before signal") {
  const std::uint64_t rounds = scaled(base_rounds);
  CHECK(publish_rounds(
            rounds, [] { return std::make_unique<rt::cpu_fence>(1); },
            [](const rt::cpu_fence &f) { f.wait(); }, [](rt::cpu_fence &f) { f.signal(); }) == 0);
}

TEST_CASE("RT-INV-SYNC-05: job_counter publishes the payload written before sub") {
  const std::uint64_t rounds = scaled(base_rounds);
  CHECK(publish_rounds(
            rounds,
            [] {
              auto c = std::make_unique<rt::job_counter>();
              c->add();
              return c;
            },
            [](const rt::job_counter &c) { c.wait_zero(); },
            [](rt::job_counter &c) { c.sub(); }) == 0);
}

TEST_CASE("RT-INV-SYNC-05: counting_semaphore publishes the payload written before release") {
  const std::uint64_t rounds = scaled(base_rounds);
  CHECK(publish_rounds(
            rounds, [] { return std::make_unique<rt::counting_semaphore>(0); },
            [](rt::counting_semaphore &s) { s.acquire(); },
            [](rt::counting_semaphore &s) { s.release(); }) == 0);
}

TEST_CASE("RT-INV-SYNC-05: event_count publishes the payload written before notify_one") {
  struct prepared {
    rt::event_count events;
    rt::event_count::key k = events.prepare_wait();
  };
  const std::uint64_t rounds = scaled(base_rounds);
  CHECK(publish_rounds(
            rounds, [] { return std::make_unique<prepared>(); },
            [](prepared &p) { p.events.wait(p.k); },
            [](prepared &p) { p.events.notify_one(); }) == 0);
}

TEST_CASE("RT-INV-SYNC-05: spin_wait spins exactly its limit, spin_limit by default") {
  static_assert(rt::spin_limit == 64);
  rt::spin_wait spinner;
  std::uint32_t spins = 0;
  while (spinner.spin()) {
    ++spins;
    REQUIRE(spins <= rt::spin_limit);
  }
  CHECK(spins == rt::spin_limit);
  CHECK(spinner.count() == rt::spin_limit);
  CHECK_FALSE(spinner.spin());
  CHECK(spinner.count() == rt::spin_limit);
  spinner.reset();
  CHECK(spinner.count() == 0);
  CHECK(spinner.spin());
  rt::spin_wait short_spin(3);
  CHECK(short_spin.spin());
  CHECK(short_spin.spin());
  CHECK(short_spin.spin());
  CHECK_FALSE(short_spin.spin());
  CHECK(short_spin.count() == 3);
}

TEST_CASE("RT-INV-SYNC-05: a spin-then-park wait parks after at most spin_limit spins") {
  rt::event_count events;
  std::atomic<bool> ready{false};
  std::atomic<std::uint32_t> spun_before_park{0};
  std::atomic<bool> parked{false};
  bounded_thread waiter([&] {
    rt::spin_wait spinner;
    while (!ready.load()) {
      if (spinner.spin()) {
        continue;
      }
      spun_before_park.store(spinner.count());
      parked.store(true);
      const rt::event_count::key k = events.prepare_wait();
      if (ready.load()) {
        events.cancel_wait();
        break;
      }
      events.wait(k);
    }
  });
  // Release the waiter only once it has spun spin_limit times and recorded that it parks; a fixed
  // sleep races thread start-up on a loaded or single-CPU machine.
  const auto deadline = std::chrono::steady_clock::now() + wait_bound;
  while (!parked.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  ready.store(true);
  events.notify_all();
  REQUIRE(waiter.finished_within(wait_bound));
  CHECK(parked.load());
  CHECK(spun_before_park.load() == rt::spin_limit);
}

TEST_CASE("RT-INV-SYNC-05: a blocked wait parks instead of spinning on every primitive") {
  {
    rt::cpu_fence fence(1);
    CHECK(parks([&] { fence.wait(); }, [&] { fence.signal(); }));
  }
  {
    rt::job_counter counter;
    counter.add();
    CHECK(parks([&] { counter.wait_zero(); }, [&] { counter.sub(); }));
  }
  {
    rt::event_count events;
    const rt::event_count::key k = events.prepare_wait();
    CHECK(parks([&] { events.wait(k); }, [&] { events.notify_one(); }));
  }
  {
    rt::counting_semaphore semaphore(0);
    CHECK(parks([&] { semaphore.acquire(); }, [&] { semaphore.release(); }));
  }
}
