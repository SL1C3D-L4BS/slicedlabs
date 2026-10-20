// event_count (WO-080, SL-ARCH-003 ARCH3-R057, R060, ADR-0024). The epoch and the sleeper count
// are seq_cst (the default order of every operation here). A waiter increments the sleeper count
// and then reads the epoch; a notifier advances the epoch and then reads the sleeper count. In the
// single total order of seq_cst operations either the notifier sees the sleeper (and wakes it) or
// the waiter's epoch read follows the notifier's epoch increment (and its later condition check
// sees what the notifier published before notifying), so no wakeup is lost. Parking is
// std::atomic::wait on the epoch after spin_limit spins.

#include "sl/runtime/sync.hpp"

#include <atomic>
#include <cstdint>

namespace sl::runtime {

event_count::key event_count::prepare_wait() noexcept {
  sleepers_.fetch_add(1);
  return epoch_.load();
}

void event_count::cancel_wait() noexcept { sleepers_.fetch_sub(1); }

void event_count::wait(key k) noexcept {
  spin_wait spinner;
  while (epoch_.load() == k) {
    if (!spinner.spin()) {
      epoch_.wait(k);
    }
  }
  sleepers_.fetch_sub(1);
}

void event_count::notify_one() noexcept {
  epoch_.fetch_add(1); // neg-lost-wakeup
  if (sleepers_.load() != 0) {
    epoch_.notify_one();
  }
}

void event_count::notify_all() noexcept {
  epoch_.fetch_add(1);
  if (sleepers_.load() != 0) {
    epoch_.notify_all();
  }
}

} // namespace sl::runtime
