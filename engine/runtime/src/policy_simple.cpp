// The reference scheduler_policy (WO-082, SL-ARCH-003 ARCH3-R070, `limits` spin_limit): victims
// in ring order (self + 1 + attempt) % workers, the injection queues polled only when the local
// queue is empty, no background aging, spin_limit 64 and no counters (they read zero). It holds
// only the worker count, written by on_start before any worker runs.

#include "sl/runtime/sync.hpp"
#include "sl/runtime/work_queue.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <new>
#include <span>

namespace sl::runtime::jobs {

namespace {

class simple_policy final : public scheduler_policy {
public:
  void on_start(std::uint32_t workers, std::uint64_t /*seed*/) noexcept override {
    workers_ = workers;
  }

  [[nodiscard]] std::uint32_t victim(std::uint32_t self, std::uint32_t attempt) noexcept override {
    if (workers_ == 0) {
      return self;
    }
    return static_cast<std::uint32_t>((std::uint64_t{self} + 1U + attempt) % workers_);
  }

  [[nodiscard]] bool poll_injection(std::uint32_t /*self*/,
                                    std::uint64_t /*local_pops*/) noexcept override {
    return false;
  }

  [[nodiscard]] bool promote_background(std::uint32_t /*waited_rounds*/) const noexcept override {
    return false;
  }

  [[nodiscard]] std::uint32_t spin_limit() const noexcept override {
    return sl::runtime::spin_limit;
  }

  void on_steal(std::uint32_t /*self*/, bool /*ok*/) noexcept override {}
  void on_park(std::uint32_t /*self*/) noexcept override {}
  void on_wake(std::uint32_t /*self*/) noexcept override {}

  void read_counters(std::span<worker_counters> out) const noexcept override {
    std::ranges::fill(out, worker_counters{});
  }

private:
  std::uint32_t workers_ = 0;
};

} // namespace

std::unique_ptr<scheduler_policy> make_reference_policy() noexcept {
  return std::unique_ptr<scheduler_policy>(new (std::nothrow) simple_policy());
}

} // namespace sl::runtime::jobs
