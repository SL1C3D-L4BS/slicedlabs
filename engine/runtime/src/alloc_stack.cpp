// stack_resource (WO-081, SL-ARCH-003 ARCH3-R048, R049, assertions list, ADR-0023). One buffer
// from the upstream, taken at the first use. Every block and marker is an entry: a 48-byte record
// just before the block's offset links to the entry below, so the stack pops without a side
// table. Entries stay unpoisoned while they are on the stack; freed ranges are filled 0xDD and
// poisoned.

#include "sl/core/assert.hpp"
#include "sl/core/error.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/memory.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <source_location>
#include <string>

namespace sl::runtime::mem {

namespace {

constexpr std::size_t granule = alignof(std::max_align_t);
constexpr std::uint32_t kind_block = 1;
constexpr std::uint32_t kind_marker = 2;

constexpr std::size_t round_up(std::size_t v, std::size_t a) noexcept {
  return (v + a - 1) & ~(a - 1);
}

void scrub(std::byte *p, std::size_t n) noexcept {
  if (n == 0) {
    return;
  }
  unpoison(p, n);
  fill_freed(p, n);
  poison(p, n);
}

} // namespace

stack_resource::stack_resource(resource &upstream, std::size_t capacity_bytes, memory_tag tag)
    : owner_thread_resource("stack", tag), upstream_(&upstream),
      capacity_(round_up(capacity_bytes, granule)) {}

stack_resource::~stack_resource() {
  if (base_ != nullptr) {
    unpoison(base_, capacity_);
    upstream_->release(base_, capacity_, granule);
  }
}

result<void> stack_resource::acquire(const std::source_location &site) noexcept {
  if (base_ != nullptr) {
    return {};
  }
  if (capacity_ == 0) {
    return make_error(errc::arena_exhausted, "stack: capacity 0");
  }
  auto got = upstream_->try_allocate(capacity_, granule, default_tag(), site);
  if (!got.has_value()) {
    return std::unexpected(got.error());
  }
  base_ = static_cast<std::byte *>(*got);
  poison(base_, capacity_);
  return {};
}

stack_resource::entry stack_resource::read_entry(std::size_t at) const noexcept {
  entry e;
  std::memcpy(&e, base_ + at - entry_bytes, sizeof(entry));
  return e;
}

void stack_resource::write_entry(std::size_t at, const entry &e) noexcept {
  std::memcpy(base_ + at - entry_bytes, &e, sizeof(entry));
}

result<std::size_t> stack_resource::push_entry(std::size_t size, std::size_t align,
                                               std::uint32_t kind) noexcept {
  // align the address, not the offset: the buffer is only granule-aligned
  const auto base = std::bit_cast<std::uintptr_t>(base_);
  const std::size_t at = round_up(base + top_ + entry_bytes, align) - base;
  if (at > capacity_ || size > capacity_ - at) {
    return make_error(errc::arena_exhausted, "stack: " + std::to_string(size) + " bytes at " +
                                                 std::to_string(top_) + " of " +
                                                 std::to_string(capacity_));
  }
  unpoison(base_ + at - entry_bytes, entry_bytes);
  write_entry(at, entry{.prev_top = top_,
                        .prev_entry = top_entry_,
                        .prev_floor = floor_,
                        .size = size,
                        .kind = kind,
                        .freed = 0});
  top_entry_ = at;
  top_ = at + size;
  return at;
}

result<void *> stack_resource::try_allocate(std::size_t bytes, std::size_t align, memory_tag tag,
                                            std::source_location site) noexcept {
  static_cast<void>(tag);
  check_owner(true);
  if (!is_valid_alignment(align)) {
    return make_error(errc::bad_alignment, "stack: alignment " + std::to_string(align));
  }
  if (auto ready = acquire(site); !ready.has_value()) {
    return std::unexpected(ready.error());
  }
  const std::size_t size = effective_size(bytes);
  auto at = push_entry(size, effective_alignment(align), kind_block);
  if (!at.has_value()) {
    return std::unexpected(at.error());
  }
  std::byte *p = base_ + *at;
  unpoison(p, size);
  fill_fresh(p, size);
  return static_cast<void *>(p);
}

void stack_resource::pop_top() noexcept {
  const entry e = read_entry(top_entry_);
  scrub(base_ + e.prev_top, top_ - e.prev_top);
  top_ = e.prev_top;
  top_entry_ = e.prev_entry;
}

void stack_resource::pop_freed() noexcept {
  while (top_entry_ != npos) {
    const entry e = read_entry(top_entry_);
    if (e.kind != kind_block || e.freed == 0) {
      return;
    }
    pop_top();
  }
}

void stack_resource::release(void *p, std::size_t bytes, std::size_t align) noexcept {
  static_cast<void>(bytes);
  static_cast<void>(align);
  check_owner(false);
  if (p == nullptr) {
    return;
  }
  const auto *bp = static_cast<const std::byte *>(p);
  const auto at_addr = std::bit_cast<std::uintptr_t>(bp);
  const auto base_addr = std::bit_cast<std::uintptr_t>(base_);
  if (base_ == nullptr || at_addr < base_addr + entry_bytes || at_addr > base_addr + top_) {
    SL_DEBUG_ASSERT(false, "stack: release of a block it does not own");
    return;
  }
  const auto at = static_cast<std::size_t>(at_addr - base_addr);
  entry e = read_entry(at);
  if (e.kind != kind_block || e.freed != 0) {
    SL_DEBUG_ASSERT(false, "stack: double release or not a block of this stack");
    return;
  }
  if (at == top_entry_) {
    pop_top();
    pop_freed();
    return;
  }
  if (depth_ != 0) {
    SL_ASSERT(false, "stack: out-of-order release inside a marker scope (release must be LIFO)");
    return;
  }
  e.freed = 1;
  write_entry(at, e);
  scrub(base_ + at, e.size);
}

result<void> stack_resource::push_marker() noexcept {
  check_owner(true);
  if (auto ready = acquire(std::source_location::current()); !ready.has_value()) {
    return std::unexpected(ready.error());
  }
  auto at = push_entry(0, granule, kind_marker);
  if (!at.has_value()) {
    return std::unexpected(at.error());
  }
  floor_ = *at;
  ++depth_;
  return {};
}

void stack_resource::pop_marker() noexcept {
  check_owner(false);
  if (depth_ == 0 || floor_ == npos) {
    SL_ASSERT(false, "stack: pop_marker without a push_marker");
    return;
  }
  const entry m = read_entry(floor_);
  scrub(base_ + m.prev_top, top_ - m.prev_top);
  top_ = m.prev_top;
  top_entry_ = m.prev_entry;
  floor_ = m.prev_floor;
  --depth_;
  if (depth_ == 0) {
    pop_freed();
  }
}

free_space stack_resource::free_space_stats() const noexcept {
  check_owner(false);
  const std::uint64_t left = capacity_ - top_;
  return free_space{.free_bytes = left, .largest_free_run = left};
}

} // namespace sl::runtime::mem
