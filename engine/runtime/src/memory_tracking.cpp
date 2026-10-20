// tracking_resource (WO-079, SL-ARCH-003 ARCH3-R042 to R046, ADR-0023). Each block is
// [header][user bytes] inside one backing allocation of size + effective alignment: the 16-byte
// header just before the user pointer holds the size, the tag and the alignment, so release
// counts the right tag in every build. Counters are relaxed atomics (statistics: no other data is
// published through them); the Debug site table is under a mutex.

#include "sl/core/assert.hpp"
#include "sl/core/error.hpp"
#include "sl/core/log.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/faults.hpp"
#include "sl/runtime/memory.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory_resource>
#include <mutex>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace sl::runtime::mem {

namespace {

constexpr std::uint32_t header_magic = 0x534C4D45U; // "SLME"

struct block_header {
  std::uint64_t size = 0;
  std::uint16_t tag = 0;
  std::uint16_t align_log2 = 0;
  std::uint32_t magic = 0;
};
static_assert(sizeof(block_header) == 16 && sizeof(block_header) <= alignof(std::max_align_t));

block_header read_header(const std::byte *user) noexcept {
  block_header h;
  std::memcpy(&h, user - sizeof(block_header), sizeof(block_header));
  return h;
}

void write_header(std::byte *user, const block_header &h) noexcept {
  std::memcpy(user - sizeof(block_header), &h, sizeof(block_header));
}

} // namespace

tracking_resource::tracking_resource(std::string_view name, memory_tag default_tag,
                                     const tag_registry *tags, std::pmr::memory_resource *backing)
    : resource(name, default_tag, tags),
      backing_(backing != nullptr ? backing : std::pmr::new_delete_resource()) {}

tracking_resource::~tracking_resource() {
  if constexpr (debug_memory) {
    for (const leak_record &r : leaks()) {
      const std::string label = tag_label(r.tag);
      sl::log::emit(sl::log::level::error, "runtime.mem", "live block at resource destruction",
                    {{"resource", name()},
                     {"tag", label},
                     {"tag_id", r.tag.id},
                     {"size", r.size},
                     {"file", r.file},
                     {"line", r.line}});
    }
  } else {
    for (std::size_t id = 0; id < slots_.size(); ++id) {
      const tag_counters c = read(std::span(slots_)[id]);
      if (c.live_blocks != 0) {
        const std::string label = tag_label(memory_tag{static_cast<std::uint16_t>(id)});
        sl::log::emit(sl::log::level::error, "runtime.mem", "live blocks at resource destruction",
                      {{"resource", name()},
                       {"tag", label},
                       {"tag_id", id},
                       {"blocks", c.live_blocks},
                       {"size", c.live_bytes}});
      }
    }
  }
}

void tracking_resource::count_alloc(slot &s, std::uint64_t bytes) noexcept {
  // pairs: counters() (relaxed statistics; the block itself is published by the caller)
  s.allocs.fetch_add(1, std::memory_order_relaxed);
  // pairs: counters() (relaxed statistics)
  s.live_blocks.fetch_add(1, std::memory_order_relaxed);
  // pairs: counters() (relaxed statistics)
  const std::uint64_t live = s.live_bytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
  // pairs: counters() (relaxed CAS max; the peak publishes nothing else)
  std::uint64_t peak = s.peak_bytes.load(std::memory_order_relaxed);
  while (peak < live &&
         // pairs: counters() (relaxed CAS max)
         !s.peak_bytes.compare_exchange_weak(peak, live, std::memory_order_relaxed,
                                             // pairs: counters() (relaxed CAS max)
                                             std::memory_order_relaxed)) {
  }
}

void tracking_resource::count_free(slot &s, std::uint64_t bytes) noexcept {
  // pairs: counters() (relaxed statistics)
  s.frees.fetch_add(1, std::memory_order_relaxed);
  // pairs: counters() (relaxed statistics)
  s.live_blocks.fetch_sub(1, std::memory_order_relaxed);
  // pairs: counters() (relaxed statistics)
  s.live_bytes.fetch_sub(bytes, std::memory_order_relaxed);
}

tag_counters tracking_resource::read(const slot &s) noexcept {
  tag_counters c;
  // pairs: count_alloc, count_free (relaxed statistics)
  c.live_bytes = s.live_bytes.load(std::memory_order_relaxed);
  // pairs: count_alloc, count_free (relaxed statistics)
  c.live_blocks = s.live_blocks.load(std::memory_order_relaxed);
  // pairs: count_alloc (relaxed CAS max)
  c.peak_bytes = s.peak_bytes.load(std::memory_order_relaxed);
  // pairs: count_alloc (relaxed statistics)
  c.allocs = s.allocs.load(std::memory_order_relaxed);
  // pairs: count_free (relaxed statistics)
  c.frees = s.frees.load(std::memory_order_relaxed);
  // pairs: try_allocate failure paths (relaxed statistics)
  c.failures = s.failures.load(std::memory_order_relaxed);
  return c;
}

result<void *> tracking_resource::try_allocate(std::size_t bytes, std::size_t align, memory_tag tag,
                                               std::source_location site) noexcept {
#if SL_RUNTIME_FAULTS // compiled out in Release, where visit() never fires
  if (visit(failpoint::rfp0).fire) {
    return make_error(errc::out_of_memory,
                      std::string(name()) + ": RFP0 alloc.fail injected out_of_memory");
  }
#endif
  if (tag.id > max_memory_tags || (tags() != nullptr && !tags()->contains(tag))) {
    return make_error(errc::invalid_argument, std::string(name()) + ": tag " +
                                                  std::to_string(tag.id) +
                                                  " is not a registered memory tag");
  }
  slot &s = std::span(slots_)[tag.id];
  const auto fail = [this, &s](errc code, const std::string &message) noexcept {
    // pairs: counters() (relaxed statistics)
    s.failures.fetch_add(1, std::memory_order_relaxed);
    // pairs: counters() (relaxed statistics)
    total_.failures.fetch_add(1, std::memory_order_relaxed);
    return make_error(code, std::string(name()) + ": " + message);
  };
  if (!is_valid_alignment(align)) {
    return fail(errc::bad_alignment,
                "alignment " + std::to_string(align) + " is not a power of two up to 4096");
  }
  const std::size_t size = effective_size(bytes);
  const std::size_t eff = effective_alignment(align);
  if (size > std::numeric_limits<std::size_t>::max() - eff) {
    return fail(errc::out_of_memory, std::to_string(bytes) + " bytes cannot be served");
  }
  auto base = try_allocate_from(*backing_, size + eff, eff, tag, site);
  if (!base.has_value()) {
    return fail(errc::out_of_memory, tag_label(tag) + " exhausted: " + base.error().message);
  }
  std::byte *user = static_cast<std::byte *>(*base) + eff;
  write_header(user, block_header{.size = size,
                                  .tag = tag.id,
                                  .align_log2 = static_cast<std::uint16_t>(std::countr_zero(eff)),
                                  .magic = header_magic});
  if constexpr (debug_memory) {
    const std::scoped_lock lock(sites_mutex_);
    sites_.insert_or_assign(std::bit_cast<std::uintptr_t>(user),
                            site_entry{.tag = tag, .size = size, .site = site});
  }
  count_alloc(s, size);
  count_alloc(total_, size);
  fill_fresh(user, size);
  return static_cast<void *>(user);
}

void tracking_resource::release(void *p, std::size_t bytes, std::size_t align) noexcept {
  if (p == nullptr) {
    return;
  }
  auto *user = static_cast<std::byte *>(p);
  const block_header h = read_header(user);
  const bool ours = h.magic == header_magic && h.tag <= max_memory_tags;
  SL_ASSERT(ours, "tracking_resource: release of a block this resource does not hold");
  if (!ours) {
    return;
  }
  const auto size = static_cast<std::size_t>(h.size);
  const std::size_t eff = std::size_t{1} << h.align_log2;
  const bool same_request = size == effective_size(bytes) && eff == effective_alignment(align);
  SL_DEBUG_ASSERT(same_request,
                  "tracking_resource: release with another size or alignment than try_allocate");
  if constexpr (debug_memory) {
    const std::scoped_lock lock(sites_mutex_);
    sites_.erase(std::bit_cast<std::uintptr_t>(p));
  }
  count_free(std::span(slots_)[h.tag], size);
  count_free(total_, size);
  fill_freed(user, size);
  write_header(user, block_header{});
  release_to(*backing_, user - eff, size + eff, eff);
}

tag_counters tracking_resource::counters(memory_tag tag) const noexcept {
  if (tag.id > max_memory_tags) {
    return {};
  }
  return read(std::span(slots_)[tag.id]);
}

tag_counters tracking_resource::totals() const noexcept { return read(total_); }

std::vector<leak_record> tracking_resource::leaks() const {
  std::vector<std::pair<std::string, leak_record>> rows;
  if constexpr (debug_memory) {
    const std::scoped_lock lock(sites_mutex_);
    rows.reserve(sites_.size());
    for (const auto &[address, entry] : sites_) {
      rows.emplace_back(tag_label(entry.tag),
                        leak_record{.tag = entry.tag,
                                    .size = entry.size,
                                    .file = entry.site.file_name(),
                                    .line = static_cast<std::uint32_t>(entry.site.line())});
    }
  }
  std::ranges::sort(rows, [](const auto &a, const auto &b) {
    return std::tie(a.first, a.second.file, a.second.line, a.second.size) <
           std::tie(b.first, b.second.file, b.second.line, b.second.size);
  });
  std::vector<leak_record> out;
  out.reserve(rows.size());
  for (const auto &row : rows) {
    out.push_back(row.second);
  }
  return out;
}

} // namespace sl::runtime::mem
