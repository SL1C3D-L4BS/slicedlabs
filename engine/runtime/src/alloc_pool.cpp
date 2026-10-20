// pool_resource (WO-081, SL-ARCH-003 ARCH3-R048, R049, `limits` pool_slab_bytes and
// pool_min_block_bytes, ADR-0023). A slab is [header][padding][block 0][block 1]...; the header
// holds a free bitmap (one bit per block) besides the slab chain. A free block holds the link to
// the next free block in its first bytes and is poisoned whole; the link is unpoisoned only while
// it is read or written. Fresh slabs push their blocks so the lowest address is served first.

#include "sl/core/assert.hpp"
#include "sl/core/error.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/memory.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <source_location>
#include <string>

namespace sl::runtime::mem {

namespace {

constexpr std::size_t round_up(std::size_t v, std::size_t a) noexcept {
  return (v + a - 1) & ~(a - 1);
}

std::uintptr_t address(const void *p) noexcept { return std::bit_cast<std::uintptr_t>(p); }

bool test_bit(const std::uint64_t *bits, std::size_t i) noexcept {
  return ((bits[i / 64U] >> (i % 64U)) & 1U) != 0;
}

void set_bit(std::uint64_t *bits, std::size_t i, bool on) noexcept {
  const std::uint64_t mask = std::uint64_t{1} << (i % 64U);
  if (on) {
    bits[i / 64U] |= mask;
  } else {
    bits[i / 64U] &= ~mask;
  }
}

std::size_t valid_align(std::size_t align) noexcept {
  return effective_alignment(is_valid_alignment(align) ? align : alignof(std::max_align_t));
}

} // namespace

pool_resource::pool_resource(resource &upstream, std::size_t block_size, std::size_t block_align,
                             memory_tag tag)
    : owner_thread_resource("pool", tag), upstream_(&upstream),
      block_size_(std::max(block_size, pool_min_block_bytes)),
      block_align_(valid_align(block_align)), stride_(round_up(block_size_, block_align_)) {
  SL_ASSERT(block_size >= pool_min_block_bytes, "pool: block_size below pool_min_block_bytes");
  SL_ASSERT(is_valid_alignment(block_align), "pool: block_align not a power of two <= 4096");
}

pool_resource::~pool_resource() {
  slab_header *s = slab_list_;
  while (s != nullptr) {
    slab_header *next = s->next;
    const std::size_t bytes = s->bytes;
    unpoison(s, bytes);
    upstream_->release(s, bytes, block_align_);
    s = next;
  }
}

std::byte *pool_resource::read_link(const std::byte *block) noexcept {
  std::byte *next = nullptr;
  unpoison(block, sizeof(next));
  std::memcpy(static_cast<void *>(&next), block, sizeof(next));
  poison(block, sizeof(next));
  return next;
}

void pool_resource::push_free(std::byte *block) noexcept {
  unpoison(block, stride_);
  fill_freed(block, stride_);
  std::memcpy(block, static_cast<const void *>(&free_), sizeof(free_));
  poison(block, stride_);
  free_ = block;
  ++free_count_;
}

result<void> pool_resource::grow(const std::source_location &site) noexcept {
  const std::size_t first = round_up(sizeof(slab_header), block_align_);
  std::size_t bytes = pool_slab_bytes;
  if (first + stride_ > bytes) {
    bytes = round_up(first + stride_, block_align_);
  }
  const std::size_t blocks = std::min((bytes - first) / stride_, max_slab_blocks);
  auto got = upstream_->try_allocate(bytes, block_align_, default_tag(), site);
  if (!got.has_value()) {
    return std::unexpected(got.error());
  }
  auto *s = std::construct_at(static_cast<slab_header *>(*got));
  s->next = slab_list_;
  s->bytes = bytes;
  s->first = first;
  s->blocks = blocks;
  slab_list_ = s;
  ++slabs_;
  total_blocks_ += blocks;
  auto *base = static_cast<std::byte *>(*got);
  poison(base + sizeof(slab_header), bytes - sizeof(slab_header));
  for (std::size_t i = blocks; i > 0; --i) {
    set_bit(s->free_bits.data(), i - 1, true);
    push_free(base + first + ((i - 1) * stride_));
  }
  return {};
}

result<void *> pool_resource::try_allocate(std::size_t bytes, std::size_t align, memory_tag tag,
                                           std::source_location site) noexcept {
  static_cast<void>(tag);
  check_owner(true);
  if (!is_valid_alignment(align)) {
    return make_error(errc::bad_alignment, "pool: alignment " + std::to_string(align));
  }
  if (bytes > block_size_ || effective_alignment(align) > block_align_) {
    return make_error(errc::invalid_argument, "pool: " + std::to_string(bytes) + " bytes, align " +
                                                  std::to_string(align) + " exceed the block of " +
                                                  std::to_string(block_size_) + " bytes, align " +
                                                  std::to_string(block_align_));
  }
  if (free_ == nullptr) {
    if (auto grown = grow(site); !grown.has_value()) {
      return std::unexpected(grown.error());
    }
  }
  std::byte *p = free_;
  free_ = read_link(p);
  --free_count_;
  slab_header *s = slab_of(p);
  if (s != nullptr) {
    set_bit(s->free_bits.data(), index_in(*s, p), false);
  }
  const std::size_t size = effective_size(bytes);
  unpoison(p, size);
  fill_fresh(p, size);
  return static_cast<void *>(p);
}

pool_resource::slab_header *pool_resource::slab_of(const void *p) const noexcept {
  const std::uintptr_t at = address(p);
  for (slab_header *s = slab_list_; s != nullptr; s = s->next) {
    const std::uintptr_t base = address(s);
    if (at >= base + s->first && at < base + s->first + (s->blocks * stride_)) {
      return s;
    }
  }
  return nullptr;
}

std::size_t pool_resource::index_in(const slab_header &s, const void *p) const noexcept {
  return static_cast<std::size_t>(address(p) - address(&s) - s.first) / stride_;
}

void pool_resource::release(void *p, std::size_t bytes, std::size_t align) noexcept {
  static_cast<void>(bytes);
  static_cast<void>(align);
  check_owner(false);
  if (p == nullptr) {
    return;
  }
  slab_header *s = slab_of(p);
  if (s == nullptr || (address(p) - address(s) - s->first) % stride_ != 0) {
    SL_DEBUG_ASSERT(false, "pool: release of a block it does not own");
    return;
  }
  const std::size_t i = index_in(*s, p);
  if (test_bit(s->free_bits.data(), i)) {
    SL_DEBUG_ASSERT(false, "pool: double release");
    return;
  }
  set_bit(s->free_bits.data(), i, true);
  push_free(static_cast<std::byte *>(p));
}

free_space pool_resource::free_space_stats() const noexcept {
  check_owner(false);
  free_space f;
  f.free_bytes = static_cast<std::uint64_t>(free_count_) * stride_;
  for (const slab_header *s = slab_list_; s != nullptr; s = s->next) {
    std::uint64_t run = 0;
    for (std::size_t i = 0; i < s->blocks; ++i) {
      run = test_bit(s->free_bits.data(), i) ? run + 1 : 0;
      f.largest_free_run = std::max(f.largest_free_run, run * stride_);
    }
  }
  return f;
}

bool pool_resource::check_free_list() const noexcept {
  check_owner(false);
  std::size_t marked = 0;
  for (slab_header *s = slab_list_; s != nullptr; s = s->next) {
    s->seen_bits.fill(0);
    for (std::size_t i = 0; i < s->blocks; ++i) {
      marked += test_bit(s->free_bits.data(), i) ? 1U : 0U;
    }
  }
  std::size_t walked = 0;
  for (const std::byte *b = free_; b != nullptr; b = read_link(b)) {
    if (walked == total_blocks_) {
      return false; // longer than the pool: a cycle
    }
    slab_header *s = slab_of(b);
    if (s == nullptr || (address(b) - address(s) - s->first) % stride_ != 0) {
      return false;
    }
    const std::size_t i = index_in(*s, b);
    if (!test_bit(s->free_bits.data(), i) || test_bit(s->seen_bits.data(), i)) {
      return false;
    }
    set_bit(s->seen_bits.data(), i, true);
    ++walked;
  }
  return walked == marked && walked == free_count_;
}

} // namespace sl::runtime::mem
