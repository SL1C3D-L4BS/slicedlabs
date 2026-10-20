// The work_queue contract suite (WO-082, SL-ARCH-003 ARCH3-R070). Sequential rules first (empty,
// capacity, order, quiesce), then the concurrent run: one owner pushes `items` indices, popping
// one after a seeded share of its pushes (and whenever the queue is full), while `thieves`
// threads steal until the owner has drained the rest; every index must be taken exactly once.
// Threads are spawned through os_thread_factory (R-RT-PLATFORM). Atomics are seq_cst.

#include "queue_contract.hpp"

#include "sl/runtime/headless_platform.hpp"
#include "sl/runtime/platform.hpp"
#include "sl/runtime/work_queue.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sl::runtime::testing {

namespace {

std::uint64_t splitmix64(std::uint64_t &state) noexcept {
  state += 0x9E3779B97F4A7C15ULL;
  std::uint64_t z = state;
  z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31U);
}

void sequential_rules(jobs::work_queue &q, std::size_t asked, std::vector<std::string> &failures) {
  if (q.capacity() < asked) {
    failures.push_back("capacity() " + std::to_string(q.capacity()) + " is below the " +
                       std::to_string(asked) + " asked");
  }
  if (q.pop() || q.steal()) {
    failures.emplace_back("a new queue is not empty");
  }
  const std::size_t cap = q.capacity();
  for (std::size_t i = 0; i < cap; ++i) {
    if (!q.push(static_cast<std::uint32_t>(i))) {
      failures.push_back("push " + std::to_string(i) + " failed below capacity");
      break;
    }
  }
  if (q.push(0xFFFFFFFFU)) {
    failures.emplace_back("push succeeded on a full queue");
  }
  for (std::size_t i = 0; i < cap; ++i) {
    static_cast<void>(q.pop());
  }
  if (q.pop() || q.steal()) {
    failures.emplace_back("the queue is not empty after popping capacity() indices");
  }
  constexpr std::uint32_t n = 8;
  for (std::uint32_t i = 0; i < n && i < cap; ++i) {
    static_cast<void>(q.push(i));
  }
  if (cap >= n) {
    const std::optional<std::uint32_t> first_steal = q.steal();
    const std::optional<std::uint32_t> first_pop = q.pop();
    if (first_steal != 0U) {
      failures.emplace_back("steal did not take the oldest index (FIFO end)");
    }
    if (first_pop != n - 1) {
      failures.emplace_back("pop did not take the newest index (LIFO end)");
    }
    while (q.pop()) {
    }
  }
  q.quiesce();
  if (q.pop() || q.steal()) {
    failures.emplace_back("the queue is not empty after quiesce");
  }
}

void concurrent_rules(jobs::work_queue &q, const queue_contract_options &options,
                      std::vector<std::string> &failures) {
  const std::uint32_t items = options.items;
  std::vector<std::atomic<std::uint32_t>> taken(items);
  std::atomic<std::uint64_t> out_of_range{0};
  std::atomic<bool> stop{false};
  const auto record = [&](std::uint32_t index) noexcept {
    if (index >= items) {
      out_of_range.fetch_add(1);
      return;
    }
    taken[index].fetch_add(1);
  };
  os_thread_factory factory(1);
  thread_group thieves;
  for (std::uint32_t t = 0; t < options.thieves; ++t) {
    auto spawned = thieves.spawn(factory, "qc" + std::to_string(t), [&] noexcept {
      while (!stop.load()) {
        if (auto index = q.steal()) {
          record(*index);
        }
      }
      while (auto index = q.steal()) {
        record(*index);
      }
    });
    if (!spawned) {
      failures.push_back("thief spawn failed: " + spawned.error().message);
      stop.store(true);
      static_cast<void>(thieves.release());
      return;
    }
  }
  std::uint64_t state = options.seed;
  for (std::uint32_t i = 0; i < items; ++i) {
    while (!q.push(i)) {
      if (auto index = q.pop()) {
        record(*index);
      }
    }
    if (splitmix64(state) % 4U == 0) {
      if (auto index = q.pop()) {
        record(*index);
      }
    }
  }
  while (auto index = q.pop()) {
    record(*index);
  }
  stop.store(true);
  static_cast<void>(thieves.release());
  while (auto index = q.steal()) {
    record(*index);
  }
  std::uint32_t lost = 0;
  std::uint32_t duplicated = 0;
  for (std::uint32_t i = 0; i < items; ++i) {
    const std::uint32_t count = taken[i].load();
    lost += count == 0 ? 1U : 0U;
    duplicated += count > 1 ? 1U : 0U;
  }
  if (lost != 0 || duplicated != 0 || out_of_range.load() != 0) {
    failures.push_back("concurrent run: " + std::to_string(lost) + " lost, " +
                       std::to_string(duplicated) + " taken twice, " +
                       std::to_string(out_of_range.load()) + " out of range");
  }
}

} // namespace

std::vector<std::string> run_queue_contract(const jobs::queue_factory &factory,
                                            const queue_contract_options &options) {
  std::vector<std::string> failures;
  if (options.owners != 1) {
    failures.emplace_back("owners must be 1: push and pop belong to one owner worker");
    return failures;
  }
  if (!factory) {
    failures.emplace_back("no queue factory");
    return failures;
  }
  auto sequential = factory(options.capacity);
  if (!sequential || !*sequential) {
    failures.push_back("the factory failed: " +
                       (sequential ? std::string("null queue") : sequential.error().message));
    return failures;
  }
  sequential_rules(**sequential, options.capacity, failures);
  auto concurrent = factory(options.capacity);
  if (!concurrent || !*concurrent) {
    failures.emplace_back("the factory failed on the second queue");
    return failures;
  }
  concurrent_rules(**concurrent, options, failures);
  return failures;
}

} // namespace sl::runtime::testing
