#pragma once

// The work_queue contract suite (WO-082, SL-ARCH-003 ARCH3-R070, ADR-0024): every queue the job
// system accepts (locked_deque now, ws_deque in WO-086) must pass it. It checks an empty queue,
// the capacity bound, LIFO pop and FIFO steal, quiesce, and an owner-plus-thieves run in which
// every pushed index is taken exactly once. Threads come from the platform thread factory
// (os_thread_factory). Owner-thread (`thread_safety`, testing::*). Include it by path:
// "../../../engine/runtime/testing/queue_contract.hpp".

#include "sl/runtime/work_queue.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sl::runtime::testing {

struct queue_contract_options {
  /// Threads that push and pop (the contract allows exactly one owner).
  std::uint32_t owners = 1;
  /// Threads that only steal.
  std::uint32_t thieves = 3;
  /// Indices pushed by the owner in the concurrent run.
  std::uint32_t items = 100000;
  /// Seeds the owner's push and pop interleaving (splitmix64).
  std::uint64_t seed = 1;
  /// The capacity asked from the factory.
  std::size_t capacity = 1024;
};

/// Runs the suite against queues built by `factory`; returns one line per violated rule (empty
/// when the queue passes).
[[nodiscard]] std::vector<std::string> run_queue_contract(const jobs::queue_factory &factory,
                                                          const queue_contract_options &options);

} // namespace sl::runtime::testing
