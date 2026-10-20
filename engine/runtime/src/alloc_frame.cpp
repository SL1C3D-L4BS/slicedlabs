// frame_arena (WO-081, SL-ARCH-003 ARCH3-R050, R051, `limits` frame_arena_block_bytes,
// `thread_safety` frame_arena rows, ADR-0023). Two sets of workers + 1 arena_resource; set index
// current_ is the frame's. flip switches to the other set, which still holds the frame before the
// one that just ended, and resets it (fill 0xDD, poison): frame-N memory lives until the flip that
// ends N+1. The per-worker arenas of the reset set are unbound so the first allocation after the
// flip binds their owner; the owner arenas stay bound to the frame_arena's owner.

#include "sl/core/assert.hpp"
#include "sl/core/error.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/memory.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <source_location>

namespace sl::runtime::mem {

frame_arena::frame_arena(resource &upstream, std::uint32_t workers, std::size_t block_bytes,
                         memory_tag tag)
    : owner_thread_resource("frame_arena", tag), workers_(workers) {
  for (auto &set : sets_) {
    set.reserve(static_cast<std::size_t>(workers) + 1);
    for (std::uint32_t k = 0; k <= workers; ++k) {
      set.push_back(std::make_unique<arena_resource>(upstream, block_bytes, 0, tag));
      if (k < workers) {
        set.back()->unbind_owner();
      }
    }
  }
}

arena_resource &frame_arena::arena(std::uint32_t set, std::uint32_t k) const noexcept {
  const auto &arenas = (set & 1U) == 0 ? sets_[0] : sets_[1];
  return *arenas[k];
}

resource &frame_arena::for_worker(std::uint32_t k) noexcept {
  SL_ASSERT(k <= workers_, "frame_arena: for_worker index above workers()");
  return arena(current_.load(), k <= workers_ ? k : workers_);
}

result<void *> frame_arena::try_allocate(std::size_t bytes, std::size_t align, memory_tag tag,
                                         std::source_location site) noexcept {
  static_cast<void>(tag);
  check_owner(true);
  return arena(current_.load(), workers_).allocate_unchecked(bytes, align, site);
}

void frame_arena::release(void *p, std::size_t bytes, std::size_t align) noexcept {
  static_cast<void>(align);
  check_owner(false);
  const std::uint32_t c = current_.load();
  arena_resource &now = arena(c, workers_);
  arena_resource &before = arena(c + 1, workers_);
  if (!now.owns(p) && before.owns(p)) {
    before.release_unchecked(p, bytes);
    return;
  }
  now.release_unchecked(p, bytes);
}

void frame_arena::flip() noexcept {
  check_owner(false);
  const std::uint32_t next = (current_.load() + 1) & 1U;
  for (std::uint32_t k = 0; k <= workers_; ++k) {
    arena_resource &a = arena(next, k);
    a.rewind_unchecked(arena_resource::marker{.block = 0, .offset = 0});
    if (k < workers_) {
      a.unbind_owner();
    }
  }
  current_.store(next);
  flips_.fetch_add(1);
}

std::uint64_t frame_arena::upstream_allocations() const noexcept {
  std::uint64_t total = 0;
  for (const auto &set : sets_) {
    for (const auto &a : set) {
      total += a->upstream_allocations();
    }
  }
  return total;
}

std::size_t frame_arena::used() const noexcept {
  std::size_t total = 0;
  for (const auto &a : (current_.load() & 1U) == 0 ? sets_[0] : sets_[1]) {
    total += a->used_unchecked();
  }
  return total;
}

} // namespace sl::runtime::mem
