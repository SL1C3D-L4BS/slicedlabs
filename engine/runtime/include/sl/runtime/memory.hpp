#pragma once

// Memory allocation contracts (WO-079, SL-ARCH-003 ARCH3-R041 to R047, ADR-0023; episode 083).
// Every runtime allocator derives from mem::resource, a std::pmr::memory_resource whose fallible
// path is the noexcept try_allocate returning sl::result; do_allocate (the pmr path) forwards to a
// configured upstream or SL_ASSERT-aborts with a tag report and never throws. Tags come from a
// tag_registry that freezes at runtime start; gpu.* and fabric.* are reserved for external
// accounting. tracking_resource counts live bytes per tag and, in Debug and sanitizer builds,
// remembers the site of every live block for the leak report. Global operator new and delete are
// not replaced (ARCH3-R047, seam V16-3). Layer L1: includes only L0 runtime headers.
// Thread safety (`thread_safety`): tag_registry add, reserve_external and freeze are owner-thread
// (before freeze); find and name are any-thread after freeze; tracking_resource try_allocate and
// release are any-thread iff its backing resource is thread-safe (counters are atomic).

#include "sl/core/error.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/faults.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory_resource>
#include <mutex>
#include <set>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

#if defined(__SANITIZE_ADDRESS__)
#define SL_RUNTIME_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define SL_RUNTIME_ASAN 1
#endif
#endif
#ifndef SL_RUNTIME_ASAN
#define SL_RUNTIME_ASAN 0
#endif

#if SL_RUNTIME_ASAN
#include <sanitizer/asan_interface.h>
#endif

namespace sl::runtime::mem {

// --- limits (SL-ARCH-003 `limits`) ---

/// The largest alignment a resource honours (`limits` max_alignment).
inline constexpr std::size_t max_alignment = 4096;
/// The most names a tag_registry holds (`limits` max_memory_tags).
inline constexpr std::size_t max_memory_tags = 256;
/// The Debug fill of fresh memory (`limits` debug_fill_fresh).
inline constexpr std::byte debug_fill_fresh{0xCD};
/// The Debug fill of freed memory (`limits` debug_fill_freed).
inline constexpr std::byte debug_fill_freed{0xDD};

/// True in Debug and sanitizer builds (NDEBUG unset or the failpoints compiled in): the Debug
/// fills run and tracking_resource keeps its site table.
#if SL_RUNTIME_FAULTS || !defined(NDEBUG)
inline constexpr bool debug_memory = true;
#else
inline constexpr bool debug_memory = false;
#endif

/// True when the build runs under AddressSanitizer: poison and unpoison take effect.
inline constexpr bool asan_enabled = SL_RUNTIME_ASAN != 0;

// --- alignment (ARCH3-R043) ---

/// A power of two no greater than max_alignment.
[[nodiscard]] constexpr bool is_valid_alignment(std::size_t align) noexcept {
  return std::has_single_bit(align) && align <= max_alignment;
}

/// The alignment a resource actually returns: max(align, alignof(std::max_align_t)).
[[nodiscard]] constexpr std::size_t effective_alignment(std::size_t align) noexcept {
  return align > alignof(std::max_align_t) ? align : alignof(std::max_align_t);
}

/// The size a resource actually serves: a zero-byte request is one byte.
[[nodiscard]] constexpr std::size_t effective_size(std::size_t bytes) noexcept {
  return bytes == 0 ? 1 : bytes;
}

// --- Debug fills and ASan poisoning (ARCH3-R046) ---

/// Fills a fresh block with 0xCD (Debug and sanitizer builds; a no-op in Release).
inline void fill_fresh(void *p, std::size_t size) noexcept {
  if constexpr (debug_memory) {
    if (p != nullptr && size != 0) {
      std::memset(p, std::to_integer<int>(debug_fill_fresh), size);
    }
  }
}

/// Fills a freed block with 0xDD (Debug and sanitizer builds; a no-op in Release).
inline void fill_freed(void *p, std::size_t size) noexcept {
  if constexpr (debug_memory) {
    if (p != nullptr && size != 0) {
      std::memset(p, std::to_integer<int>(debug_fill_freed), size);
    }
  }
}

/// Poisons [p, p + size) under ASan (ASAN_POISON_MEMORY_REGION); a no-op elsewhere. Recycling
/// resources poison freed and reset ranges.
inline void poison(const void *p, std::size_t size) noexcept {
#if SL_RUNTIME_ASAN
  ASAN_POISON_MEMORY_REGION(p, size);
#else
  static_cast<void>(p);
  static_cast<void>(size);
#endif
}

/// Unpoisons [p, p + size) under ASan before a recycled range is handed out; a no-op elsewhere.
inline void unpoison(const void *p, std::size_t size) noexcept {
#if SL_RUNTIME_ASAN
  ASAN_UNPOISON_MEMORY_REGION(p, size);
#else
  static_cast<void>(p);
  static_cast<void>(size);
#endif
}

// --- tags (ARCH3-R045) ---

/// A memory tag; id 0 is untagged, registered tags are 1..max_memory_tags.
struct memory_tag {
  std::uint16_t id = 0;
  friend constexpr bool operator==(memory_tag, memory_tag) noexcept = default;
};

/// The tag names. Names match ^[a-z][a-z0-9_]*(\.[a-z0-9_]+)*$; the first segments gpu and fabric
/// are reserved for reserve_external. Owner-thread until freeze, read-only (any-thread) after.
class tag_registry {
public:
  tag_registry();
  tag_registry(const tag_registry &) = delete;
  tag_registry(tag_registry &&) = delete;
  tag_registry &operator=(const tag_registry &) = delete;
  tag_registry &operator=(tag_registry &&) = delete;
  ~tag_registry() = default;

  /// Registers a runtime tag: tag_registry_frozen after freeze, invalid_argument for a malformed
  /// name, tag_reserved for gpu.* or fabric.*, already_exists for a duplicate, out_of_range past
  /// max_memory_tags.
  [[nodiscard]] result<memory_tag> add(std::string_view name);
  /// Registers an external tag (gpu.* or fabric.* only; invalid_argument otherwise); the other
  /// errors as add.
  [[nodiscard]] result<memory_tag> reserve_external(std::string_view name);
  /// The tag of `name`; not_found when it is not registered.
  [[nodiscard]] result<memory_tag> find(std::string_view name) const;
  /// The name of `tag`; "untagged" for id 0, "unknown" for an id that is not registered.
  [[nodiscard]] std::string_view name(memory_tag tag) const noexcept;
  /// Freezes the registry (runtime start); a second freeze succeeds and changes nothing.
  [[nodiscard]] result<void> freeze() noexcept;
  [[nodiscard]] bool frozen() const noexcept;
  /// True for a tag registered through reserve_external.
  [[nodiscard]] bool is_external(memory_tag tag) const noexcept;
  /// True for a registered tag (id 1..size()).
  [[nodiscard]] bool contains(memory_tag tag) const noexcept;
  /// The number of registered tags.
  [[nodiscard]] std::size_t size() const noexcept;

private:
  [[nodiscard]] result<memory_tag> insert(std::string_view name, bool external);

  std::vector<std::string> names_; // reserved to max_memory_tags: views stay valid
  std::array<bool, max_memory_tags + 1> external_{};
  std::atomic<std::size_t> count_{0};
  std::atomic<bool> frozen_{false};
};

/// The tags WO-096 registers at runtime start, in order (through add).
inline constexpr std::array<std::string_view, 9> runtime_tag_names = {
    "runtime.root", "jobs.records", "ecs.chunks", "ecs.entities", "frame.arena",
    "events",       "snapshots",    "reflect",    "link"};
/// The external tags WO-096 reserves at runtime start (through reserve_external).
inline constexpr std::array<std::string_view, 1> runtime_external_tag_names = {"fabric.projection"};

// --- resources (ARCH3-R041, R042) ---

/// Free memory of a resource; WO-081 overrides free_space_stats for fragmentation.
struct free_space {
  std::uint64_t free_bytes = 0;
  std::uint64_t largest_free_run = 0;
};

/// The base of every runtime allocator: a std::pmr::memory_resource with a noexcept, fallible
/// try_allocate. is_equal is identity; neither copyable nor movable. The pmr path (allocate)
/// calls try_allocate with the default tag and, on error, the upstream set by set_upstream, or
/// SL_ASSERT-aborts with a tag report when there is none.
class resource : public std::pmr::memory_resource {
public:
  resource(const resource &) = delete;
  resource(resource &&) = delete;
  resource &operator=(const resource &) = delete;
  resource &operator=(resource &&) = delete;
  ~resource() override = default;

  /// Allocates `bytes` (0 serves 1) aligned to max(align, alignof(std::max_align_t)):
  /// bad_alignment unless is_valid_alignment(align), out_of_memory when exhausted.
  [[nodiscard]] virtual result<void *>
  try_allocate(std::size_t bytes, std::size_t align, memory_tag tag,
               std::source_location site = std::source_location::current()) noexcept = 0;
  /// Returns a block of try_allocate with the same bytes and align.
  virtual void release(void *p, std::size_t bytes, std::size_t align) noexcept = 0;
  /// Free memory; zero for resources that do not know it.
  [[nodiscard]] virtual free_space free_space_stats() const noexcept { return {}; }

  /// The fallback of the pmr path (nullptr: none, the default). Owner-thread, before use.
  void set_upstream(std::pmr::memory_resource *fallback) noexcept { upstream_ = fallback; }
  [[nodiscard]] std::pmr::memory_resource *upstream() const noexcept { return upstream_; }
  [[nodiscard]] memory_tag default_tag() const noexcept { return default_tag_; }
  [[nodiscard]] std::string_view name() const noexcept { return name_; }
  /// The registry that names the tags (nullptr: none).
  [[nodiscard]] const tag_registry *tags() const noexcept { return tags_; }
  /// The name of `tag` for reports: the registry's name, else "untagged" or "tag#<id>".
  [[nodiscard]] std::string tag_label(memory_tag tag) const;

  /// The body of do_allocate (the pmr path): try_allocate(default tag), then the upstream, then
  /// SL_ASSERT(false, "<name>: <tag> exhausted ...") and nullptr when the assert handler returns.
  /// Public so a test can observe the nullptr that std::pmr::memory_resource::allocate may not
  /// return.
  [[nodiscard]] void *forward_allocate(std::size_t bytes, std::size_t align) noexcept;

protected:
  explicit resource(std::string_view name, memory_tag default_tag = {},
                    const tag_registry *tags = nullptr);

private:
  void *do_allocate(std::size_t bytes, std::size_t align) noexcept final;
  void do_deallocate(void *p, std::size_t bytes, std::size_t align) noexcept final;
  [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource &other) const noexcept final;

  std::string name_;
  memory_tag default_tag_;
  const tag_registry *tags_;
  std::pmr::memory_resource *upstream_ = nullptr;
  // Blocks the pmr path took from the upstream; do_deallocate returns them there.
  std::mutex upstream_mutex_;
  std::set<std::uintptr_t> upstream_blocks_;
  std::atomic<std::size_t> upstream_live_{0};
};

/// Allocates from any memory resource without letting an exception out: a mem::resource through
/// try_allocate, std::pmr::new_delete_resource() through nothrow operator new (out_of_memory on
/// failure), any other resource through allocate (an exception it throws terminates). `bytes` and
/// `align` are passed as given.
[[nodiscard]] result<void *>
try_allocate_from(std::pmr::memory_resource &from, std::size_t bytes, std::size_t align,
                  memory_tag tag,
                  std::source_location site = std::source_location::current()) noexcept;
/// Returns a block of try_allocate_from to `to`.
void release_to(std::pmr::memory_resource &to, void *p, std::size_t bytes,
                std::size_t align) noexcept;

// --- tracking (ARCH3-R044, R045) ---

/// A snapshot of one tag's counters (or of the totals).
struct tag_counters {
  std::uint64_t live_bytes = 0;
  std::uint64_t live_blocks = 0;
  std::uint64_t peak_bytes = 0;
  std::uint64_t allocs = 0;
  std::uint64_t frees = 0;
  std::uint64_t failures = 0;
};

/// One live block of a tracking_resource; file and line are empty and 0 in Release.
struct leak_record {
  memory_tag tag;
  std::size_t size = 0;
  std::string_view file;
  std::uint32_t line = 0;
};

/// Wraps a backing resource (default std::pmr::new_delete_resource()) and counts every block per
/// tag. Each block carries a 16-byte header in front of it (tag and size, so release knows the
/// tag); Debug and sanitizer builds also keep a site table for leaks(). RFP0 is visited at the top
/// of try_allocate. The destructor logs one runtime.mem error event per live block.
class tracking_resource final : public resource {
public:
  explicit tracking_resource(std::string_view name, memory_tag default_tag = {},
                             const tag_registry *tags = nullptr,
                             std::pmr::memory_resource *backing = std::pmr::new_delete_resource());
  tracking_resource(const tracking_resource &) = delete;
  tracking_resource(tracking_resource &&) = delete;
  tracking_resource &operator=(const tracking_resource &) = delete;
  tracking_resource &operator=(tracking_resource &&) = delete;
  ~tracking_resource() override;

  /// As resource::try_allocate; also invalid_argument for a tag above max_memory_tags or, with a
  /// registry, a tag it does not contain (id 0 included).
  [[nodiscard]] result<void *>
  try_allocate(std::size_t bytes, std::size_t align, memory_tag tag,
               std::source_location site = std::source_location::current()) noexcept override;
  void release(void *p, std::size_t bytes, std::size_t align) noexcept override;

  /// The counters of `tag` (zeros for an id above max_memory_tags). Any thread.
  [[nodiscard]] tag_counters counters(memory_tag tag) const noexcept;
  /// The counters over every tag. Any thread.
  [[nodiscard]] tag_counters totals() const noexcept;
  /// The live blocks sorted by (tag name, file, line, size); empty in Release (no site table).
  [[nodiscard]] std::vector<leak_record> leaks() const;
  [[nodiscard]] std::pmr::memory_resource *backing() const noexcept { return backing_; }

private:
  struct slot {
    std::atomic<std::uint64_t> live_bytes{0};
    std::atomic<std::uint64_t> live_blocks{0};
    std::atomic<std::uint64_t> peak_bytes{0};
    std::atomic<std::uint64_t> allocs{0};
    std::atomic<std::uint64_t> frees{0};
    std::atomic<std::uint64_t> failures{0};
  };
  struct site_entry {
    memory_tag tag;
    std::size_t size = 0;
    std::source_location site;
  };

  static void count_alloc(slot &s, std::uint64_t bytes) noexcept;
  static void count_free(slot &s, std::uint64_t bytes) noexcept;
  static tag_counters read(const slot &s) noexcept;

  std::pmr::memory_resource *backing_;
  std::array<slot, max_memory_tags + 1> slots_{};
  slot total_{};
  mutable std::mutex sites_mutex_;
  std::map<std::uintptr_t, site_entry> sites_;
};

} // namespace sl::runtime::mem
