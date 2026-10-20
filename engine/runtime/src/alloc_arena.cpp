// arena_resource (WO-081, SL-ARCH-003 ARCH3-R048, R049, ADR-0023). Blocks come from the upstream
// with a 32-byte header in front ([header][data]); the chain is intrusive, so the arena allocates
// nothing besides its blocks. Blocks after the current one are empty (end == header_bytes), so a
// marker {block index, offset} names every later allocation. Freed ranges are filled 0xDD and
// poisoned; fresh blocks are poisoned and each allocation unpoisons exactly its bytes.

#include "sl/core/assert.hpp"
#include "sl/core/error.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/memory.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <source_location>
#include <string>

namespace sl::runtime::mem {

namespace {

constexpr std::size_t granule = alignof(std::max_align_t);

constexpr std::size_t round_up(std::size_t v, std::size_t a) noexcept {
  return (v + a - 1) & ~(a - 1);
}

std::uintptr_t address(const void *p) noexcept { return std::bit_cast<std::uintptr_t>(p); }

std::byte *byte_ptr(void *p) noexcept { return static_cast<std::byte *>(p); }

// Fills [p, p + n) 0xDD and poisons it (n rounded up to the granule: the next block starts on one).
void scrub(std::byte *p, std::size_t n) noexcept {
  if (n == 0) {
    return;
  }
  const std::size_t span = round_up(n, granule);
  unpoison(p, span);
  fill_freed(p, span);
  poison(p, span);
}

} // namespace

arena_resource::arena_resource(resource &upstream, std::size_t block_bytes,
                               std::size_t capacity_bytes, memory_tag tag)
    : owner_thread_resource("arena", tag), upstream_(&upstream),
      block_bytes_(round_up(std::max(block_bytes, header_bytes + granule), granule)),
      capacity_bytes_(capacity_bytes) {}

arena_resource::~arena_resource() {
  block_header *b = first_;
  while (b != nullptr) {
    block_header *next = b->next;
    const std::size_t size = b->size;
    unpoison(b, size);
    upstream_->release(b, size, granule);
    b = next;
  }
}

result<arena_resource::block_header *>
arena_resource::grow(std::size_t need, const std::source_location &site) noexcept {
  std::size_t size = std::max(block_bytes_, round_up(need, granule));
  if (capacity_bytes_ != 0) {
    const std::size_t left = capacity_bytes_ > reserved_ ? capacity_bytes_ - reserved_ : 0;
    if (size > left) {
      if (need > left) {
        return make_error(errc::arena_exhausted,
                          "arena: capacity " + std::to_string(capacity_bytes_) +
                              " bytes reached (" + std::to_string(reserved_) + " reserved, " +
                              std::to_string(need) + " needed)");
      }
      size = left & ~(granule - 1);
    }
  }
  auto got = upstream_->try_allocate(size, granule, default_tag(), site);
  if (!got.has_value()) {
    return std::unexpected(got.error());
  }
  upstream_allocations_.fetch_add(1);
  auto *b = std::construct_at(
      static_cast<block_header *>(*got),
      block_header{.next = nullptr, .size = size, .index = blocks_, .end = header_bytes});
  poison(static_cast<std::byte *>(*got) + header_bytes, size - header_bytes);
  if (last_ != nullptr) {
    last_->next = b;
  } else {
    first_ = b;
  }
  last_ = b;
  ++blocks_;
  reserved_ += size;
  return b;
}

result<void *> arena_resource::try_allocate(std::size_t bytes, std::size_t align, memory_tag tag,
                                            std::source_location site) noexcept {
  static_cast<void>(tag);
  check_owner(true);
  return allocate_unchecked(bytes, align, site);
}

result<void *> arena_resource::allocate_unchecked(std::size_t bytes, std::size_t align,
                                                  const std::source_location &site) noexcept {
  if (!is_valid_alignment(align)) {
    return make_error(errc::bad_alignment, "arena: alignment " + std::to_string(align));
  }
  const std::size_t size = effective_size(bytes);
  const std::size_t a = effective_alignment(align);
  if (size > (static_cast<std::size_t>(-1) >> 2U)) {
    return make_error(errc::out_of_memory, "arena: " + std::to_string(size) + " bytes");
  }
  block_header *b = current_ != nullptr ? current_ : first_;
  std::uintptr_t at = 0;
  for (; b != nullptr; b = b->next) {
    const std::uintptr_t base = address(b);
    at = round_up(base + b->end, a);
    if (at + size <= base + b->size) {
      break;
    }
  }
  if (b == nullptr) {
    auto grown = grow(header_bytes + size + a, site);
    if (!grown.has_value()) {
      return std::unexpected(grown.error());
    }
    b = *grown;
    at = round_up(address(b) + b->end, a);
  }
  current_ = b;
  auto *p = std::bit_cast<std::byte *>(at);
  tail_ = p;
  tail_prev_end_ = b->end;
  b->end = static_cast<std::size_t>(at - address(b)) + size;
  unpoison(p, size);
  fill_fresh(p, size);
  return static_cast<void *>(p);
}

void arena_resource::release(void *p, std::size_t bytes, std::size_t align) noexcept {
  static_cast<void>(align);
  check_owner(false);
  release_unchecked(p, bytes);
}

void arena_resource::release_unchecked(void *p, std::size_t bytes) noexcept {
  if (p == nullptr) {
    return;
  }
  auto *bp = static_cast<std::byte *>(p);
  const std::size_t size = effective_size(bytes);
  if (bp == tail_ && current_ != nullptr) {
    current_->end = tail_prev_end_;
    tail_ = nullptr;
    scrub(bp, size);
    return;
  }
  if constexpr (debug_memory) {
    if (!owns(p)) {
      SL_DEBUG_ASSERT(false, "arena: release of a block it does not own");
      return;
    }
    scrub(bp, size);
  }
}

bool arena_resource::owns(const void *p) const noexcept {
  const std::uintptr_t at = address(p);
  for (const block_header *b = first_; b != nullptr; b = b->next) {
    const std::uintptr_t base = address(b);
    if (at >= base + header_bytes && at < base + b->size) {
      return true;
    }
  }
  return false;
}

arena_resource::marker arena_resource::mark() const noexcept {
  check_owner(false);
  if (current_ == nullptr) {
    return marker{.block = 0, .offset = header_bytes};
  }
  return marker{.block = current_->index, .offset = current_->end};
}

void arena_resource::rewind(marker m) noexcept {
  check_owner(false);
  rewind_unchecked(m);
}

void arena_resource::rewind_unchecked(marker m) noexcept {
  if (current_ == nullptr) {
    return;
  }
  [[maybe_unused]] const bool behind =
      m.block < current_->index || (m.block == current_->index && m.offset <= current_->end);
  SL_DEBUG_ASSERT(behind, "arena: rewind to a marker past the current position");
  block_header *b = first_;
  while (b != nullptr && b->index < m.block) {
    b = b->next;
  }
  if (b == nullptr) {
    return;
  }
  block_header *target = b;
  const std::size_t keep = std::clamp(m.offset, header_bytes, b->end);
  const std::size_t stop = current_->index;
  for (; b != nullptr && b->index <= stop; b = b->next) {
    const std::size_t from = b == target ? keep : header_bytes;
    if (b->end > from) {
      scrub(byte_ptr(b) + from, b->end - from);
    }
    b->end = from;
  }
  current_ = target;
  tail_ = nullptr;
}

void arena_resource::reset() noexcept {
  check_owner(false);
  rewind_unchecked(marker{.block = 0, .offset = header_bytes});
}

std::size_t arena_resource::used() const noexcept {
  check_owner(false);
  return used_unchecked();
}

std::size_t arena_resource::used_unchecked() const noexcept {
  std::size_t total = 0;
  for (const block_header *b = first_; b != nullptr; b = b->next) {
    total += b->end - header_bytes;
  }
  return total;
}

free_space arena_resource::free_space_stats() const noexcept {
  check_owner(false);
  free_space f;
  const block_header *b = current_ != nullptr ? current_ : first_;
  for (; b != nullptr; b = b->next) {
    const std::uint64_t left = b->size - b->end;
    f.free_bytes += left;
    f.largest_free_run = std::max(f.largest_free_run, left);
  }
  return f;
}

} // namespace sl::runtime::mem
