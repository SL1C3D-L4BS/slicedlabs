#pragma once

// Foundational allocators (WO-081, SL-ARCH-003 ARCH3-R048 to R051, ADR-0023, ADR-0024; episode
// 084). Six mem::resource classes, each passing testing::run_allocator_contract: arena_resource
// (blocks, markers, rewind, reset), stack_resource (one buffer, LIFO markers), pool_resource (fixed
// blocks from 64 KiB slabs, intrusive LIFO free list), frame_arena (two buffer sets of per-worker
// arenas, flipped at end_frame), synchronized_resource (a std::mutex around another resource) and
// general_resource (an adapter over any std::pmr upstream). Layer L2: includes only L0 and L1
// runtime headers.
// Thread safety (`thread_safety`): arena_resource, stack_resource, pool_resource and frame_arena
// are owner-thread: they capture the constructing thread (a per-worker frame arena: the thread of
// its first allocation after a flip) and SL_DEBUG_ASSERT it on every call; frame_arena::flip is
// owner-thread (end_frame slot arena_flip). synchronized_resource is any-thread and lifts the owner
// check of the resource it wraps (its mutex orders every call); general_resource is any-thread iff
// its upstream is. Recycling resources fill fresh blocks 0xCD and released or reset ranges 0xDD
// (Debug and sanitizer builds) and ASan-poison released and reset ranges.

#include "sl/core/error.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/memory.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <source_location>
#include <string_view>
#include <thread>
#include <vector>

namespace sl::runtime::mem {

// --- limits (SL-ARCH-003 `limits`) ---

/// The bytes of one pool slab (`limits` pool_slab_bytes).
inline constexpr std::size_t pool_slab_bytes = 65536;
/// The smallest pool block (`limits` pool_min_block_bytes).
inline constexpr std::size_t pool_min_block_bytes = 16;
/// The default arena block (`limits` arena_block_bytes_default).
inline constexpr std::size_t arena_block_bytes_default = 65536;
/// The default frame arena block per worker per buffer (`limits` frame_arena_block_bytes).
inline constexpr std::size_t frame_arena_block_bytes = 1048576;

class frame_arena;
class synchronized_resource;

/// The base of the owner-thread allocators: the owner check of ARCH3-R049. The owner is the
/// constructing thread; an unbound owner (std::thread::id{}) is bound by the next allocation.
/// Compiled out with NDEBUG, like SL_DEBUG_ASSERT.
class owner_thread_resource : public resource {
public:
  owner_thread_resource(const owner_thread_resource &) = delete;
  owner_thread_resource(owner_thread_resource &&) = delete;
  owner_thread_resource &operator=(const owner_thread_resource &) = delete;
  owner_thread_resource &operator=(owner_thread_resource &&) = delete;
  ~owner_thread_resource() override = default;

  /// The owner thread; std::thread::id{} while unbound.
  [[nodiscard]] std::thread::id owner() const noexcept { return owner_.load(); }
  /// True once synchronized_resource wraps this resource: the owner check is lifted for good.
  [[nodiscard]] bool shared() const noexcept { return shared_.load(); }

protected:
  owner_thread_resource(std::string_view name, memory_tag default_tag);
  /// SL_DEBUG_ASSERT that the calling thread is the owner; `bind` first binds an unbound owner to
  /// the caller. A no-op once shared or with NDEBUG.
  void check_owner(bool bind) const noexcept;
  /// Unbinds the owner: the next allocation binds it (per-worker frame arenas after a flip).
  void unbind_owner() noexcept { owner_.store(std::thread::id{}); }
  /// Lifts the owner check (synchronized_resource).
  void share() noexcept { shared_.store(true); }

private:
  friend class synchronized_resource;

  mutable std::atomic<std::thread::id> owner_;
  std::atomic<bool> shared_{false};
};

// --- arena_resource ---

/// A bump allocator over blocks from `upstream` (block_bytes each, larger for a larger request).
/// release frees only the most recent block (the LIFO tail); every other release only fills and
/// poisons the range. rewind and reset keep the blocks for reuse. With capacity_bytes != 0 the
/// blocks taken from upstream never exceed it (arena_exhausted); 0 grows without bound.
class arena_resource final : public owner_thread_resource {
public:
  /// A position: the index of the current block and the offset of the first free byte in it.
  struct marker {
    std::size_t block = 0;
    std::size_t offset = 0;
  };

  explicit arena_resource(resource &upstream, std::size_t block_bytes = arena_block_bytes_default,
                          std::size_t capacity_bytes = 0, memory_tag tag = {});
  arena_resource(const arena_resource &) = delete;
  arena_resource(arena_resource &&) = delete;
  arena_resource &operator=(const arena_resource &) = delete;
  arena_resource &operator=(arena_resource &&) = delete;
  ~arena_resource() override;

  /// As resource::try_allocate; arena_exhausted past capacity_bytes, out_of_memory (or the error
  /// of the upstream) when the upstream refuses a block.
  [[nodiscard]] result<void *>
  try_allocate(std::size_t bytes, std::size_t align, memory_tag tag,
               std::source_location site = std::source_location::current()) noexcept override;
  void release(void *p, std::size_t bytes, std::size_t align) noexcept override;
  /// The free bytes from the current position on and the largest free tail of one block.
  [[nodiscard]] free_space free_space_stats() const noexcept override;

  /// The current position.
  [[nodiscard]] marker mark() const noexcept;
  /// Frees every allocation made after `m` (fill 0xDD, poison); the blocks stay.
  void rewind(marker m) noexcept;
  /// Frees every allocation (usage 0); the blocks stay.
  void reset() noexcept;
  /// The bytes in use, alignment padding included.
  [[nodiscard]] std::size_t used() const noexcept;
  /// The bytes taken from the upstream (the blocks).
  [[nodiscard]] std::size_t reserved() const noexcept { return reserved_; }
  /// The number of blocks taken from the upstream so far (WO-089: zero growth after warm-up).
  [[nodiscard]] std::uint64_t upstream_allocations() const noexcept {
    return upstream_allocations_.load();
  }
  /// True when `p` lies inside one of the blocks.
  [[nodiscard]] bool owns(const void *p) const noexcept;

private:
  friend class frame_arena;
  struct block_header {
    block_header *next = nullptr;
    std::size_t size = 0;  // bytes of the block, header included
    std::size_t index = 0; // position in the chain
    std::size_t end = 0;   // offset of the first free byte
  };
  static constexpr std::size_t header_bytes = 32;
  static_assert(sizeof(block_header) <= header_bytes);

  [[nodiscard]] result<void *> allocate_unchecked(std::size_t bytes, std::size_t align,
                                                  const std::source_location &site) noexcept;
  void release_unchecked(void *p, std::size_t bytes) noexcept;
  void rewind_unchecked(marker m) noexcept;
  [[nodiscard]] std::size_t used_unchecked() const noexcept;
  [[nodiscard]] result<block_header *> grow(std::size_t need,
                                            const std::source_location &site) noexcept;

  resource *upstream_;
  std::size_t block_bytes_;
  std::size_t capacity_bytes_;
  block_header *first_ = nullptr;
  block_header *last_ = nullptr;
  block_header *current_ = nullptr;
  std::size_t blocks_ = 0;
  std::size_t reserved_ = 0;
  std::byte *tail_ = nullptr;     // the most recent allocation (nullptr after a release of it)
  std::size_t tail_prev_end_ = 0; // current_->end before it
  std::atomic<std::uint64_t> upstream_allocations_{0};
};

// --- stack_resource ---

/// A stack over one buffer of capacity_bytes from `upstream` (taken at the first allocation).
/// Each block carries a 48-byte entry in front of it. push_marker opens a scope and pop_marker
/// frees everything allocated since. Inside a scope release must be LIFO: releasing any block but
/// the top one is SL_ASSERT (assertions list) and the block stays allocated. Outside every scope a
/// release that is not the top marks the block free; its space returns when the blocks above it
/// are released (the allocator contract releases out of order).
class stack_resource final : public owner_thread_resource {
public:
  stack_resource(resource &upstream, std::size_t capacity_bytes, memory_tag tag = {});
  stack_resource(const stack_resource &) = delete;
  stack_resource(stack_resource &&) = delete;
  stack_resource &operator=(const stack_resource &) = delete;
  stack_resource &operator=(stack_resource &&) = delete;
  ~stack_resource() override;

  /// As resource::try_allocate; arena_exhausted when the buffer is full, the upstream's error
  /// when it refuses the buffer.
  [[nodiscard]] result<void *>
  try_allocate(std::size_t bytes, std::size_t align, memory_tag tag,
               std::source_location site = std::source_location::current()) noexcept override;
  void release(void *p, std::size_t bytes, std::size_t align) noexcept override;
  [[nodiscard]] free_space free_space_stats() const noexcept override;

  /// Opens a scope; arena_exhausted when the buffer has no room for the marker entry.
  [[nodiscard]] result<void> push_marker() noexcept;
  /// Closes the innermost scope and frees every block allocated in it; SL_ASSERT without one.
  void pop_marker() noexcept;
  /// The open scopes.
  [[nodiscard]] std::size_t depth() const noexcept { return depth_; }
  /// The bytes in use, entries and padding included.
  [[nodiscard]] std::size_t used() const noexcept { return top_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
  struct entry {
    std::size_t prev_top = 0;   // top_ before this entry
    std::size_t prev_entry = 0; // the entry below (npos: none)
    std::size_t prev_floor = 0; // a marker: the enclosing marker (npos: none)
    std::size_t size = 0;       // a block: its served size
    std::uint32_t kind = 0;     // 1 block, 2 marker
    std::uint32_t freed = 0;    // a block released out of order outside every scope
  };
  static constexpr std::size_t entry_bytes = 48;
  static constexpr std::size_t npos = static_cast<std::size_t>(-1);
  static_assert(sizeof(entry) <= entry_bytes);

  [[nodiscard]] result<void> acquire(const std::source_location &site) noexcept;
  [[nodiscard]] result<std::size_t> push_entry(std::size_t size, std::size_t align,
                                               std::uint32_t kind) noexcept;
  [[nodiscard]] entry read_entry(std::size_t at) const noexcept;
  void write_entry(std::size_t at, const entry &e) noexcept;
  void pop_top() noexcept;
  void pop_freed() noexcept;

  resource *upstream_;
  std::size_t capacity_;
  std::byte *base_ = nullptr;
  std::size_t top_ = 0;
  std::size_t top_entry_ = npos;
  std::size_t floor_ = npos;
  std::size_t depth_ = 0;
};

// --- pool_resource ---

/// Fixed blocks of block_size (at least 16) aligned to block_align (at most 4096), carved from
/// 64 KiB slabs of `upstream` and recycled through an intrusive LIFO free list. A request larger
/// than block_size or aligned beyond block_align is invalid_argument. Each slab header keeps a
/// free bitmap: free_space_stats reports the free bytes and the largest run of contiguous free
/// blocks in one slab, check_free_list proves every free block is on the list exactly once.
class pool_resource final : public owner_thread_resource {
public:
  pool_resource(resource &upstream, std::size_t block_size, std::size_t block_align,
                memory_tag tag = {});
  pool_resource(const pool_resource &) = delete;
  pool_resource(pool_resource &&) = delete;
  pool_resource &operator=(const pool_resource &) = delete;
  pool_resource &operator=(pool_resource &&) = delete;
  ~pool_resource() override;

  [[nodiscard]] result<void *>
  try_allocate(std::size_t bytes, std::size_t align, memory_tag tag,
               std::source_location site = std::source_location::current()) noexcept override;
  void release(void *p, std::size_t bytes, std::size_t align) noexcept override;
  /// Free bytes (free blocks x stride) and the largest contiguous free run of one slab, in bytes.
  [[nodiscard]] free_space free_space_stats() const noexcept override;

  /// The block size and alignment served, and the distance between blocks.
  [[nodiscard]] std::size_t block_size() const noexcept { return block_size_; }
  [[nodiscard]] std::size_t block_align() const noexcept { return block_align_; }
  [[nodiscard]] std::size_t stride() const noexcept { return stride_; }
  [[nodiscard]] std::size_t slabs() const noexcept { return slabs_; }
  [[nodiscard]] std::size_t total_blocks() const noexcept { return total_blocks_; }
  [[nodiscard]] std::size_t free_blocks() const noexcept { return free_count_; }
  /// Walks the free list: true when every entry is a free block of a slab, no block appears twice
  /// and the list holds exactly the blocks the bitmaps mark free.
  [[nodiscard]] bool check_free_list() const noexcept;

private:
  static constexpr std::size_t max_slab_blocks = pool_slab_bytes / pool_min_block_bytes;
  struct slab_header {
    slab_header *next = nullptr;
    std::size_t bytes = 0;
    std::size_t first = 0; // offset of block 0
    std::size_t blocks = 0;
    std::array<std::uint64_t, max_slab_blocks / 64> free_bits{};
    std::array<std::uint64_t, max_slab_blocks / 64> seen_bits{};
  };

  [[nodiscard]] result<void> grow(const std::source_location &site) noexcept;
  [[nodiscard]] slab_header *slab_of(const void *p) const noexcept;
  [[nodiscard]] std::size_t index_in(const slab_header &s, const void *p) const noexcept;
  void push_free(std::byte *block) noexcept;
  [[nodiscard]] static std::byte *read_link(const std::byte *block) noexcept;

  resource *upstream_;
  std::size_t block_size_;
  std::size_t block_align_;
  std::size_t stride_;
  slab_header *slab_list_ = nullptr;
  std::byte *free_ = nullptr;
  std::size_t slabs_ = 0;
  std::size_t total_blocks_ = 0;
  std::size_t free_count_ = 0;
};

// --- frame_arena ---

/// Double-buffered frame memory: buffer sets A and B, each with one arena_resource per worker plus
/// one for the owner thread (for_worker(workers)). flip (end_frame slot arena_flip) makes the other
/// set current and resets it, filling 0xDD and poisoning what it held: memory of frame N stays
/// valid through the end_frame of N+1 (ARCH3-R050). The resource interface allocates from the
/// owner arena of the current set. Growth beyond block_bytes takes upstream blocks, counted by
/// upstream_allocations (zero after warm-up, ARCH3-R051).
class frame_arena final : public owner_thread_resource {
public:
  frame_arena(resource &upstream, std::uint32_t workers,
              std::size_t block_bytes = frame_arena_block_bytes, memory_tag tag = {});
  frame_arena(const frame_arena &) = delete;
  frame_arena(frame_arena &&) = delete;
  frame_arena &operator=(const frame_arena &) = delete;
  frame_arena &operator=(frame_arena &&) = delete;
  ~frame_arena() override = default;

  [[nodiscard]] result<void *>
  try_allocate(std::size_t bytes, std::size_t align, memory_tag tag,
               std::source_location site = std::source_location::current()) noexcept override;
  void release(void *p, std::size_t bytes, std::size_t align) noexcept override;

  /// The arena of worker k in the current set (k = workers(): the owner thread's); worker-only:
  /// its owner is the thread of its first allocation after a flip. SL_ASSERT for k > workers().
  [[nodiscard]] resource &for_worker(std::uint32_t k) noexcept;
  /// Owner thread, end_frame: makes the other set current and resets it (fill 0xDD, poison).
  void flip() noexcept;

  [[nodiscard]] std::uint32_t workers() const noexcept { return workers_; }
  /// The current set: 0 (A) or 1 (B).
  [[nodiscard]] std::uint32_t current_set() const noexcept { return current_.load(); }
  /// The flips so far.
  [[nodiscard]] std::uint64_t flips() const noexcept { return flips_.load(); }
  /// The upstream blocks taken by every arena of both sets.
  [[nodiscard]] std::uint64_t upstream_allocations() const noexcept;
  /// The bytes in use in the current set.
  [[nodiscard]] std::size_t used() const noexcept;

private:
  [[nodiscard]] arena_resource &arena(std::uint32_t set, std::uint32_t k) const noexcept;

  std::uint32_t workers_;
  std::array<std::vector<std::unique_ptr<arena_resource>>, 2> sets_;
  std::atomic<std::uint32_t> current_{0};
  std::atomic<std::uint64_t> flips_{0};
};

// --- synchronized_resource ---

/// A std::mutex around `inner`: any thread. Lifts the owner check of an owner-thread inner
/// resource, so the mutex orders every call; the default tag is the inner's.
class synchronized_resource final : public resource {
public:
  explicit synchronized_resource(resource &inner);
  synchronized_resource(const synchronized_resource &) = delete;
  synchronized_resource(synchronized_resource &&) = delete;
  synchronized_resource &operator=(const synchronized_resource &) = delete;
  synchronized_resource &operator=(synchronized_resource &&) = delete;
  ~synchronized_resource() override = default;

  [[nodiscard]] result<void *>
  try_allocate(std::size_t bytes, std::size_t align, memory_tag tag,
               std::source_location site = std::source_location::current()) noexcept override;
  void release(void *p, std::size_t bytes, std::size_t align) noexcept override;
  [[nodiscard]] free_space free_space_stats() const noexcept override;

  [[nodiscard]] resource &inner() const noexcept { return *inner_; }

private:
  resource *inner_;
  mutable std::mutex mutex_;
};

// --- general_resource ---

/// An adapter over any std::pmr upstream (default std::pmr::new_delete_resource()): validates the
/// alignment, serves max(bytes, 1) at max(align, alignof(std::max_align_t)) through
/// try_allocate_from with the caller's tag, and fills 0xCD and 0xDD. Any thread iff the upstream
/// is.
class general_resource final : public resource {
public:
  explicit general_resource(std::pmr::memory_resource *upstream = std::pmr::new_delete_resource(),
                            memory_tag tag = {});
  general_resource(const general_resource &) = delete;
  general_resource(general_resource &&) = delete;
  general_resource &operator=(const general_resource &) = delete;
  general_resource &operator=(general_resource &&) = delete;
  ~general_resource() override = default;

  [[nodiscard]] result<void *>
  try_allocate(std::size_t bytes, std::size_t align, memory_tag tag,
               std::source_location site = std::source_location::current()) noexcept override;
  void release(void *p, std::size_t bytes, std::size_t align) noexcept override;

  [[nodiscard]] std::pmr::memory_resource *upstream_resource() const noexcept {
    return upstream_resource_;
  }

private:
  std::pmr::memory_resource *upstream_resource_;
};

} // namespace sl::runtime::mem
