// The stealing scheduler_policy (WO-086, SL-ARCH-003 ARCH3-R074, R075, R076, `limits`
// injection_poll_every, steal_attempts_per_round, spin_limit, aging_limit; episode 088). One slot
// per thread that takes jobs: workers 0..workers-1 and the job system's owner thread at index
// `workers`, each on its own 64-byte line. A slot's xorshift64 state and last injection poll are
// touched only by that slot's thread (the job system passes the caller's own index); its counters
// are written by that thread and read by any thread, both relaxed (diagnostic sums). on_start
// writes everything before any worker runs. promote_background has no index: it finds the
// caller's slot by the thread id the slot recorded in victim or poll_injection (the owner slot
// when none matches).

#include "sl/runtime/error.hpp"
#include "sl/runtime/scheduler_stats.hpp"
#include "sl/runtime/work_queue.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <new>
#include <span>
#include <thread>

namespace sl::runtime::jobs {

namespace {

// `limits` max_workers (jobs.hpp is not included: same layer, the policy needs only the bound).
constexpr std::uint32_t policy_max_workers = 64;
constexpr std::uint64_t golden = 0x9E3779B97F4A7C15ULL;

std::uint64_t splitmix64(std::uint64_t x) noexcept {
  std::uint64_t z = x + golden;
  z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31U);
}

std::uint64_t xorshift64(std::uint64_t &state) noexcept {
  std::uint64_t x = state;
  x ^= x << 13U;
  x ^= x >> 7U;
  x ^= x << 17U;
  state = x;
  return x;
}

struct alignas(64) slot_state {
  std::uint64_t rng = golden;  // the slot's own thread only
  std::uint64_t last_poll = 0; // local_pops at the last true poll_injection (own thread only)
  std::atomic<std::thread::id> thread;
  std::atomic<std::uint64_t> steals{0};
  std::atomic<std::uint64_t> failed_steals{0};
  std::atomic<std::uint64_t> parks{0};
  std::atomic<std::uint64_t> wakes{0};
  std::atomic<std::uint64_t> injection_polls{0};
  mutable std::atomic<std::uint64_t> aging_promotions{0};
};

void bump(std::atomic<std::uint64_t> &counter) noexcept {
  // pairs: steal_policy::read_counters relaxed loads (a diagnostic sum, no ordering needed)
  counter.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t peek(const std::atomic<std::uint64_t> &counter) noexcept {
  // pairs: bump's relaxed fetch_add (a diagnostic sum, no ordering needed)
  return counter.load(std::memory_order_relaxed);
}

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index): every slots_ index is checked
// against workers_ <= 64 or bounded by slots_.size() (65)
class steal_policy final : public scheduler_policy {
public:
  explicit steal_policy(const steal_options &options) noexcept : options_(options) {}

  void on_start(std::uint32_t workers, std::uint64_t seed) noexcept override {
    workers_ = workers < policy_max_workers ? workers : policy_max_workers;
    const std::uint64_t mixed = options_.seed ^ seed;
    for (std::uint32_t i = 0; i < slots_.size(); ++i) {
      slot_state &s = slots_[i];
      const std::uint64_t state = splitmix64(mixed ^ i);
      s.rng = state != 0 ? state : golden;
      s.last_poll = 0;
      s.thread.store(std::thread::id{});
      s.steals.store(0);
      s.failed_steals.store(0);
      s.parks.store(0);
      s.wakes.store(0);
      s.injection_polls.store(0);
      s.aging_promotions.store(0);
    }
  }

  [[nodiscard]] std::uint32_t victim(std::uint32_t self, std::uint32_t attempt) noexcept override {
    if (workers_ == 0 || self > workers_ || attempt >= 2U * workers_) {
      return self;
    }
    slot_state &s = slots_[self];
    note_thread(s);
    const std::uint64_t r = xorshift64(s.rng);
    if (self == workers_) {
      return static_cast<std::uint32_t>(r % workers_); // the owner may rob any worker
    }
    if (workers_ == 1) {
      return self; // no other worker
    }
    auto v = static_cast<std::uint32_t>(r % (workers_ - 1U));
    return v >= self ? v + 1U : v;
  }

  [[nodiscard]] bool poll_injection(std::uint32_t self,
                                    std::uint64_t local_pops) noexcept override {
    if (self > workers_) {
      return false;
    }
    slot_state &s = slots_[self];
    note_thread(s);
    if (local_pops == 0 || local_pops % options_.injection_poll_every != 0 ||
        local_pops == s.last_poll) {
      return false;
    }
    s.last_poll = local_pops;
    bump(s.injection_polls);
    return true;
  }

  [[nodiscard]] bool promote_background(std::uint32_t waited_rounds) const noexcept override {
    if (waited_rounds < options_.aging_limit) {
      return false;
    }
    if (waited_rounds == options_.aging_limit) {
      // pairs: steal_policy::read_counters relaxed loads (a diagnostic sum)
      slots_[caller_slot()].aging_promotions.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
  }

  [[nodiscard]] std::uint32_t spin_limit() const noexcept override { return options_.spin_limit; }

  void on_steal(std::uint32_t self, bool ok) noexcept override {
    if (self <= workers_) {
      bump(ok ? slots_[self].steals : slots_[self].failed_steals);
    }
  }

  void on_park(std::uint32_t self) noexcept override {
    if (self <= workers_) {
      bump(slots_[self].parks);
    }
  }

  void on_wake(std::uint32_t self) noexcept override {
    if (self <= workers_) {
      bump(slots_[self].wakes);
    }
  }

  void read_counters(std::span<worker_counters> out) const noexcept override {
    for (std::size_t i = 0; i < out.size(); ++i) {
      worker_counters c;
      if (i <= workers_) {
        const slot_state &s = slots_[i];
        c.steals = peek(s.steals);
        c.failed_steals = peek(s.failed_steals);
        c.parks = peek(s.parks);
        c.wakes = peek(s.wakes);
        c.injection_polls = peek(s.injection_polls);
        c.aging_promotions = peek(s.aging_promotions);
      }
      out[i] = c;
    }
  }

private:
  static void note_thread(slot_state &s) noexcept {
    const std::thread::id me = std::this_thread::get_id();
    if (s.thread.load() != me) {
      s.thread.store(me);
    }
  }

  [[nodiscard]] std::uint32_t caller_slot() const noexcept {
    const std::thread::id me = std::this_thread::get_id();
    const std::span<const slot_state> active = std::span(slots_).first(workers_);
    const auto it = std::ranges::find_if(
        active, [me](const slot_state &s) noexcept { return s.thread.load() == me; });
    return static_cast<std::uint32_t>(std::distance(active.begin(), it)); // workers_ if none
  }

  steal_options options_;
  std::uint32_t workers_ = 0;
  std::array<slot_state, policy_max_workers + 1> slots_{};
};
// NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)

} // namespace

result<std::unique_ptr<scheduler_policy>> make_steal_policy(const steal_options &options) noexcept {
  if (options.injection_poll_every == 0 || options.aging_limit == 0) {
    return make_error(errc::invalid_argument,
                      "make_steal_policy: injection_poll_every and aging_limit must be at least 1");
  }
  std::unique_ptr<scheduler_policy> policy(new (std::nothrow) steal_policy(options));
  if (!policy) {
    return make_error(errc::out_of_memory, "make_steal_policy: allocation failed");
  }
  return policy;
}

} // namespace sl::runtime::jobs
