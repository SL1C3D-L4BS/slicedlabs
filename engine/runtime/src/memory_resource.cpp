// mem::resource, the pmr path and the nothrow upstream helpers (WO-079, SL-ARCH-003 ARCH3-R041 to
// R043, ADR-0023, ADR-0009). do_allocate never lets an exception out: try_allocate first, then the
// configured upstream, else SL_ASSERT with a tag report (the default handler aborts).

#include "sl/core/assert.hpp"
#include "sl/core/error.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/memory.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <mutex>
#include <new>
#include <source_location>
#include <string>
#include <string_view>

namespace sl::runtime::mem {

resource::resource(std::string_view name, memory_tag default_tag, const tag_registry *tags)
    : name_(name), default_tag_(default_tag), tags_(tags) {}

std::string resource::tag_label(memory_tag tag) const {
  if (tags_ != nullptr && (tag.id == 0 || tags_->contains(tag))) {
    return std::string(tags_->name(tag));
  }
  if (tag.id == 0) {
    return "untagged";
  }
  return "tag#" + std::to_string(tag.id);
}

void *resource::forward_allocate(std::size_t bytes, std::size_t align) noexcept {
  auto block = try_allocate(bytes, align, default_tag_);
  if (block.has_value()) {
    return *block;
  }
  if (upstream_ != nullptr) {
    // An exception from the upstream ends at this noexcept boundary (ADR-0023: OOM aborts).
    void *p = upstream_->allocate(bytes, align);
    const std::scoped_lock lock(upstream_mutex_);
    upstream_blocks_.insert(std::bit_cast<std::uintptr_t>(p));
    upstream_live_.fetch_add(1); // seq_cst: read by do_deallocate without the lock
    return p;
  }
  const std::string report = name_ + ": " + tag_label(default_tag_) + " exhausted (" +
                             std::to_string(bytes) + " bytes, align " + std::to_string(align) +
                             ", " + std::string(to_string(block.error().code)) + ": " +
                             block.error().message + ") and no upstream";
  SL_ASSERT(false, report);
  return nullptr;
}

void *resource::do_allocate(std::size_t bytes, std::size_t align) noexcept {
  return forward_allocate(bytes, align);
}

void resource::do_deallocate(void *p, std::size_t bytes, std::size_t align) noexcept {
  if (upstream_live_.load() != 0) {
    std::unique_lock lock(upstream_mutex_);
    const auto it = upstream_blocks_.find(std::bit_cast<std::uintptr_t>(p));
    if (it != upstream_blocks_.end()) {
      upstream_blocks_.erase(it);
      upstream_live_.fetch_sub(1);
      lock.unlock();
      upstream_->deallocate(p, bytes, align);
      return;
    }
  }
  release(p, bytes, align);
}

bool resource::do_is_equal(const std::pmr::memory_resource &other) const noexcept {
  return this == &other;
}

result<void *> try_allocate_from(std::pmr::memory_resource &from, std::size_t bytes,
                                 std::size_t align, memory_tag tag,
                                 std::source_location site) noexcept {
  if (auto *runtime = dynamic_cast<resource *>(&from); runtime != nullptr) {
    return runtime->try_allocate(bytes, align, tag, site);
  }
  if (&from == std::pmr::new_delete_resource()) {
    void *p = ::operator new(bytes, std::align_val_t{align}, std::nothrow);
    if (p == nullptr) {
      return make_error(errc::out_of_memory, "operator new: " + std::to_string(bytes) +
                                                 " bytes, align " + std::to_string(align));
    }
    return p;
  }
  // Any other pmr resource reports exhaustion by throwing, which terminates here.
  return from.allocate(bytes, align);
}

void release_to(std::pmr::memory_resource &to, void *p, std::size_t bytes,
                std::size_t align) noexcept {
  if (auto *runtime = dynamic_cast<resource *>(&to); runtime != nullptr) {
    runtime->release(p, bytes, align);
    return;
  }
  if (&to == std::pmr::new_delete_resource()) {
    ::operator delete(p, std::align_val_t{align});
    return;
  }
  to.deallocate(p, bytes, align);
}

} // namespace sl::runtime::mem
