#pragma once

// Fault injection for memory_backend (WO-042, FAB-INV-STO-01 and STO-03). Include as
// "../../../engine/fabric/testing/store_faults.hpp" from tests/unit/fabric (WO-042 may add files
// only under engine/fabric/testing/, which is not on the include path).
//
//   store_faults::fail_after(n)  steps 0..n-1 of later commits pass, step n fails with store_io
//   store_faults::flip_on_read   xor a byte of one object's frame copy on every read
//   steps(), trace()             every commit step seen, in order

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/memory_backend.hpp"

namespace sl::fabric::testing {

class store_faults final : public memory_backend_faults {
public:
  /// Arms a failure at the commit step with index `n`, counted from now (0 fails the next step).
  void fail_after(std::uint64_t n);
  /// Disarms the step failure; the step count keeps running.
  void disarm();
  /// Every later read of object `id` sees byte `offset` of its frame xor `mask`.
  void flip_on_read(const digest &id, std::size_t offset, std::uint8_t mask);
  void clear_flips();

  /// The number of commit steps seen so far.
  [[nodiscard]] std::uint64_t steps() const;
  /// The names of the commit steps seen so far, in order.
  [[nodiscard]] std::vector<std::string> trace() const;
  /// The number of step failures injected so far.
  [[nodiscard]] std::uint64_t injected() const;

  [[nodiscard]] result<void> before_step(std::string_view step) noexcept override;
  void on_read(const digest &id, std::span<std::uint8_t> frame) const noexcept override;

private:
  struct flip {
    std::size_t offset = 0;
    std::uint8_t mask = 0;
  };

  mutable std::mutex mutex_;
  std::optional<std::uint64_t> fail_at_; // absolute step index
  std::uint64_t steps_ = 0;
  std::uint64_t injected_ = 0;
  std::vector<std::string> trace_;
  std::map<digest, flip> flips_;
};

} // namespace sl::fabric::testing
