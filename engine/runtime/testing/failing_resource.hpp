#pragma once

// testing::failing_resource (WO-079, SL-ARCH-003 ARCH3-R041 and `failpoints` RFP0, ADR-0023): a
// mem::resource over an upstream that fails one chosen try_allocate, or every one of one tag,
// with out_of_memory, so a test injects an allocation failure into exactly one resource. It does
// not visit the process-wide RFP0 (tracking_resource does). Owner-thread (`thread_safety`,
// testing::*); the counters are atomic, the upstream decides the rest. Include it by path:
// "../../../engine/runtime/testing/failing_resource.hpp".

#include "sl/core/error.hpp"
#include "sl/runtime/memory.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <source_location>

namespace sl::runtime::testing {

class failing_resource final : public mem::resource {
public:
  /// Fails the fail_at-th try_allocate (1-based; 0 never) with out_of_memory or, when `only` is
  /// set, every try_allocate of that tag (fail_at is then ignored); the rest go to `upstream`.
  failing_resource(std::pmr::memory_resource &upstream, std::uint64_t fail_at,
                   std::optional<mem::memory_tag> only = std::nullopt);
  failing_resource(const failing_resource &) = delete;
  failing_resource(failing_resource &&) = delete;
  failing_resource &operator=(const failing_resource &) = delete;
  failing_resource &operator=(failing_resource &&) = delete;
  ~failing_resource() override = default;

  [[nodiscard]] result<void *>
  try_allocate(std::size_t bytes, std::size_t align, mem::memory_tag tag,
               std::source_location site = std::source_location::current()) noexcept override;
  void release(void *p, std::size_t bytes, std::size_t align) noexcept override;

  /// try_allocate calls so far, and how many of them were failed on purpose.
  [[nodiscard]] std::uint64_t calls() const noexcept { return calls_.load(); }
  [[nodiscard]] std::uint64_t failed() const noexcept { return failed_.load(); }

private:
  std::pmr::memory_resource *upstream_resource_;
  std::uint64_t fail_at_;
  std::optional<mem::memory_tag> only_;
  std::atomic<std::uint64_t> calls_{0};
  std::atomic<std::uint64_t> failed_{0};
};

} // namespace sl::runtime::testing
