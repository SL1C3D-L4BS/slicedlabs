// RT-INV-SYNC-04 (SL-ARCH-003 ARCH3-R060, ADR-0024): event_count loses no wakeup across 1e6
// seeded prepare, notify and wait interleavings (scaled by SL_RUNTIME_STRESS_SCALE, 0.1 under the
// sanitizers, floored at 1). One producer publishes items and notifies (notify_one, sometimes
// notify_all); three consumers take items and otherwise prepare_wait, re-check, then wait or
// cancel_wait. The producer and the consumers draw yields and the notify kind from
// std::mt19937_64; the seed is printed on failure. Every wait is bounded at 5 s: a watchdog fails
// the round when no item is taken for 5 s (a lost wakeup), then releases the threads with
// notify_all so they can be joined. Every thread finishes through std::future::wait_for.

#include "sl/runtime/sync.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <random>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace rt = sl::runtime;
using namespace std::chrono_literals;

constexpr auto wait_bound = 5s;
constexpr double base_interleavings = 1e6;
constexpr std::uint64_t rounds = 8;
constexpr std::size_t consumer_count = 3;

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

struct channel {
  rt::event_count events;
  std::atomic<std::uint64_t> produced{0};
  std::atomic<std::uint64_t> taken{0};
  std::atomic<bool> abort{false};
  std::uint64_t total = 0;
};

// Takes one published item; false when none is available.
bool try_take(channel &c) {
  std::uint64_t t = c.taken.load();
  while (t < c.produced.load()) {
    if (c.taken.compare_exchange_weak(t, t + 1)) {
      if (t + 1 == c.total) {
        c.events.notify_all(); // the last item: release the other consumers
      }
      return true;
    }
  }
  return false;
}

bool finished(const channel &c) { return c.taken.load() >= c.total || c.abort.load(); }

void consume(channel &c, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  while (!finished(c)) {
    if (try_take(c)) {
      continue;
    }
    const rt::event_count::key k = c.events.prepare_wait();
    if (c.taken.load() < c.produced.load() || finished(c)) {
      c.events.cancel_wait();
      continue;
    }
    if (rng() % 8 == 0) {
      std::this_thread::yield(); // widen the window between prepare_wait and wait
    }
    c.events.wait(k);
  }
}

void produce(channel &c, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  for (std::uint64_t i = 0; i < c.total && !c.abort.load(); ++i) {
    c.produced.fetch_add(1);
    const std::uint64_t draw = rng();
    if (draw % 16 == 0) {
      c.events.notify_all();
    } else {
      c.events.notify_one();
    }
    if ((draw >> 8U) % 32 == 0) {
      std::this_thread::yield();
    }
  }
}

// One seeded round; returns false when the watchdog saw no progress for 5 s.
bool run_round(std::uint64_t seed, std::uint64_t items) {
  channel c;
  c.total = items;
  std::vector<bounded_thread> threads;
  threads.reserve(consumer_count + 1);
  for (std::size_t k = 0; k < consumer_count; ++k) {
    threads.emplace_back([&c, s = seed + 1 + k] { consume(c, s); });
  }
  threads.emplace_back([&c, seed] { produce(c, seed); });
  bool progressed = true;
  std::uint64_t last = c.taken.load();
  auto since = std::chrono::steady_clock::now();
  while (c.taken.load() < c.total) {
    std::this_thread::sleep_for(5ms);
    const std::uint64_t now = c.taken.load();
    if (now != last) {
      last = now;
      since = std::chrono::steady_clock::now();
    } else if (std::chrono::steady_clock::now() - since > wait_bound) {
      progressed = false;
      break;
    }
  }
  if (!progressed) {
    c.abort.store(true);
    c.events.notify_all();
  }
  bool joined = true;
  for (const bounded_thread &t : threads) {
    joined = t.finished_within(wait_bound) && joined;
  }
  return progressed && joined && c.taken.load() == c.total;
}

} // namespace

TEST_CASE("RT-INV-SYNC-04: a notify between prepare_wait and wait is not lost") {
  rt::event_count events;
  const rt::event_count::key k = events.prepare_wait();
  events.notify_one();
  bounded_thread waiter([&events, k] { events.wait(k); }); // the epoch moved: no park
  const bool returned = waiter.finished_within(wait_bound);
  CHECK(returned);
  if (!returned) {
    events.notify_all(); // release the lost waiter so it can be joined
  }
  const rt::event_count::key k2 = events.prepare_wait();
  CHECK(k2 != k);
  events.notify_all();
  events.wait(k2); // notify_all moved the epoch
  const rt::event_count::key k3 = events.prepare_wait();
  events.cancel_wait();
  events.notify_one(); // no sleeper left: advances the epoch only
  CHECK(events.prepare_wait() != k3);
  events.cancel_wait();
}

TEST_CASE("RT-INV-SYNC-04: a parked waiter is woken by notify_one and by notify_all") {
  rt::event_count events;
  std::atomic<bool> flag{false};
  for (int kind = 0; kind < 2; ++kind) {
    CAPTURE(kind);
    flag.store(false);
    bounded_thread waiter([&events, &flag] {
      while (!flag.load()) {
        const rt::event_count::key k = events.prepare_wait();
        if (flag.load()) {
          events.cancel_wait();
          break;
        }
        events.wait(k);
      }
    });
    std::this_thread::sleep_for(20ms); // let it park
    flag.store(true);
    if (kind == 0) {
      events.notify_one();
    } else {
      events.notify_all();
    }
    const bool woken = waiter.finished_within(wait_bound);
    CHECK(woken);
    if (!woken) {
      events.notify_all(); // release the lost waiter so it can be joined
    }
  }
}

TEST_CASE("RT-INV-SYNC-04: event_count loses no wakeup across 1e6 seeded interleavings") {
  constexpr std::uint64_t base_seed = 0x5EED0870004ULL;
  const std::uint64_t items = std::max<std::uint64_t>(1, scaled(base_interleavings) / rounds);
  for (std::uint64_t round = 0; round < rounds; ++round) {
    const std::uint64_t seed = base_seed + (round << 4U);
    CAPTURE(seed);
    CAPTURE(items);
    REQUIRE(run_round(seed, items));
  }
}
