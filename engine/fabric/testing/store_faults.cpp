// Fault injection for memory_backend (WO-042).

#include "store_faults.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"

namespace sl::fabric::testing {

void store_faults::fail_after(std::uint64_t n) {
  const std::scoped_lock lock(mutex_);
  fail_at_ = steps_ + n;
}

void store_faults::disarm() {
  const std::scoped_lock lock(mutex_);
  fail_at_.reset();
}

void store_faults::flip_on_read(const digest &id, std::size_t offset, std::uint8_t mask) {
  const std::scoped_lock lock(mutex_);
  flips_.insert_or_assign(id, flip{.offset = offset, .mask = mask});
}

void store_faults::clear_flips() {
  const std::scoped_lock lock(mutex_);
  flips_.clear();
}

std::uint64_t store_faults::steps() const {
  const std::scoped_lock lock(mutex_);
  return steps_;
}

std::vector<std::string> store_faults::trace() const {
  const std::scoped_lock lock(mutex_);
  return trace_;
}

std::uint64_t store_faults::injected() const {
  const std::scoped_lock lock(mutex_);
  return injected_;
}

result<void> store_faults::before_step(std::string_view step) noexcept {
  const std::scoped_lock lock(mutex_);
  const std::uint64_t index = steps_++;
  trace_.emplace_back(step);
  if (fail_at_ && *fail_at_ == index) {
    fail_at_.reset();
    ++injected_;
    return make_error(errc::store_io, "injected fault at commit step " + std::to_string(index) +
                                          " (" + std::string(step) + ")");
  }
  return {};
}

void store_faults::on_read(const digest &id, std::span<std::uint8_t> frame) const noexcept {
  const std::scoped_lock lock(mutex_);
  const auto it = flips_.find(id);
  if (it != flips_.end() && it->second.offset < frame.size()) {
    frame[it->second.offset] ^= it->second.mask;
  }
}

} // namespace sl::fabric::testing
