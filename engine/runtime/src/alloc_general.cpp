// general_resource (WO-081, SL-ARCH-003 ARCH3-R048, R043, ADR-0023): the upstream adapter. It
// keeps no state of its own, so it is as thread-safe as its upstream; blocks go through
// try_allocate_from and release_to with the effective size and alignment.

#include "sl/core/error.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/memory.hpp"

#include <cstddef>
#include <memory_resource>
#include <source_location>
#include <string>

namespace sl::runtime::mem {

general_resource::general_resource(std::pmr::memory_resource *upstream, memory_tag tag)
    : resource("general", tag),
      upstream_resource_(upstream != nullptr ? upstream : std::pmr::new_delete_resource()) {}

result<void *> general_resource::try_allocate(std::size_t bytes, std::size_t align, memory_tag tag,
                                              std::source_location site) noexcept {
  if (!is_valid_alignment(align)) {
    return make_error(errc::bad_alignment, "general: alignment " + std::to_string(align));
  }
  const std::size_t size = effective_size(bytes);
  auto got = try_allocate_from(*upstream_resource_, size, effective_alignment(align), tag, site);
  if (!got.has_value()) {
    return got;
  }
  fill_fresh(*got, size);
  return got;
}

void general_resource::release(void *p, std::size_t bytes, std::size_t align) noexcept {
  if (p == nullptr) {
    return;
  }
  const std::size_t size = effective_size(bytes);
  fill_freed(p, size);
  release_to(*upstream_resource_, p, size, effective_alignment(align));
}

} // namespace sl::runtime::mem
