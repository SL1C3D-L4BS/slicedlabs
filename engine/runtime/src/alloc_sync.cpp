// synchronized_resource and the owner check of the owner-thread allocators (WO-081, SL-ARCH-003
// ARCH3-R049, `thread_safety`, assertions list, ADR-0024). The owner is a std::atomic of
// std::thread::id (seq_cst, the default order): binding, unbinding and checking may happen on
// different threads, and a misuse must stay a captured assertion, not a data race. Wrapping a
// resource in synchronized_resource lifts its owner check; the mutex then orders every call.

#include "sl/core/assert.hpp"
#include "sl/core/error.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/memory.hpp"

#include <cstddef>
#include <mutex>
#include <source_location>
#include <string_view>
#include <thread>

namespace sl::runtime::mem {

owner_thread_resource::owner_thread_resource(std::string_view name, memory_tag default_tag)
    : resource(name, default_tag), owner_(std::this_thread::get_id()) {}

void owner_thread_resource::check_owner(bool bind) const noexcept {
#ifdef NDEBUG
  static_cast<void>(bind);
#else
  if (shared_.load()) {
    return;
  }
  const std::thread::id me = std::this_thread::get_id();
  std::thread::id owner = owner_.load();
  if (owner == std::thread::id{}) {
    if (bind) {
      owner_.compare_exchange_strong(owner, me);
      owner = owner_.load();
    } else {
      return;
    }
  }
  SL_DEBUG_ASSERT(owner == me, "owner-thread allocator used from another thread");
#endif
}

synchronized_resource::synchronized_resource(resource &inner)
    : resource("synchronized", inner.default_tag(), inner.tags()), inner_(&inner) {
  if (auto *owned = dynamic_cast<owner_thread_resource *>(&inner); owned != nullptr) {
    owned->share();
  }
}

result<void *> synchronized_resource::try_allocate(std::size_t bytes, std::size_t align,
                                                   memory_tag tag,
                                                   std::source_location site) noexcept {
  const std::scoped_lock lock(mutex_);
  return inner_->try_allocate(bytes, align, tag, site);
}

void synchronized_resource::release(void *p, std::size_t bytes, std::size_t align) noexcept {
  const std::scoped_lock lock(mutex_);
  inner_->release(p, bytes, align);
}

free_space synchronized_resource::free_space_stats() const noexcept {
  const std::scoped_lock lock(mutex_);
  return inner_->free_space_stats();
}

} // namespace sl::runtime::mem
