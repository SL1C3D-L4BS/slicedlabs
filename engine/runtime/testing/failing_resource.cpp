// testing::failing_resource (WO-079): see failing_resource.hpp.

#include "failing_resource.hpp"

#include "sl/core/error.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/memory.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <source_location>
#include <string>

namespace sl::runtime::testing {

failing_resource::failing_resource(std::pmr::memory_resource &upstream, std::uint64_t fail_at,
                                   std::optional<mem::memory_tag> only)
    : mem::resource("failing_resource"), upstream_resource_(&upstream), fail_at_(fail_at),
      only_(only) {}

result<void *> failing_resource::try_allocate(std::size_t bytes, std::size_t align,
                                              mem::memory_tag tag,
                                              std::source_location site) noexcept {
  const std::uint64_t call = calls_.fetch_add(1) + 1;
  const bool fire = only_.has_value() ? tag == *only_ : (fail_at_ != 0 && call == fail_at_);
  if (fire) {
    failed_.fetch_add(1);
    return make_error(errc::out_of_memory, "failing_resource: injected out_of_memory at call " +
                                               std::to_string(call) + ", tag " +
                                               std::to_string(tag.id));
  }
  if (!mem::is_valid_alignment(align)) {
    return make_error(errc::bad_alignment, "failing_resource: alignment " + std::to_string(align) +
                                               " is not a power of two up to 4096");
  }
  return mem::try_allocate_from(*upstream_resource_, mem::effective_size(bytes),
                                mem::effective_alignment(align), tag, site);
}

void failing_resource::release(void *p, std::size_t bytes, std::size_t align) noexcept {
  if (p == nullptr) {
    return;
  }
  mem::release_to(*upstream_resource_, p, mem::effective_size(bytes),
                  mem::effective_alignment(align));
}

} // namespace sl::runtime::testing
