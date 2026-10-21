// Memory reports (WO-085, SL-ARCH-003 ARCH3-R052, R054 to R056, ADR-0023). make_report reads
// relaxed atomics only: the counters of the tracking resources, the allocator samples taken on the
// owner thread and the external bytes. Each counter is read once per report, so a reader sees
// every monotone counter non-decreasing (read-read coherence); the per-tag peak is kept monotone
// across reports by a relaxed CAS max in peak_seen_.

#include "sl/core/assert.hpp"
#include "sl/core/error.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/memdiag.hpp"
#include "sl/runtime/memory.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace sl::runtime::mem {

namespace {

// Raises `slot` to at least `value` and returns the result (relaxed CAS max).
std::uint64_t raise_to(std::atomic<std::uint64_t> &slot, std::uint64_t value) noexcept {
  // pairs: raise_to (relaxed CAS max; the peak publishes nothing else)
  std::uint64_t seen = slot.load(std::memory_order_relaxed);
  while (seen < value &&
         // pairs: raise_to (relaxed CAS max)
         !slot.compare_exchange_weak(seen, value, std::memory_order_relaxed,
                                     // pairs: raise_to (relaxed CAS max)
                                     std::memory_order_relaxed)) {
  }
  return seen < value ? value : seen;
}

void add_counters(tag_counters &sum, const tag_counters &c) noexcept {
  sum.live_bytes += c.live_bytes;
  sum.live_blocks += c.live_blocks;
  sum.peak_bytes += c.peak_bytes;
  sum.allocs += c.allocs;
  sum.frees += c.frees;
  sum.failures += c.failures;
}

bool any_counter(const tag_counters &c) noexcept {
  return c.live_bytes != 0 || c.live_blocks != 0 || c.peak_bytes != 0 || c.allocs != 0 ||
         c.frees != 0 || c.failures != 0;
}

} // namespace

diagnostics::diagnostics(const tag_registry &tags, std::uint32_t instance,
                         std::uint32_t report_interval)
    : tags_(&tags), instance_(instance),
      report_interval_(report_interval == 0 ? report_interval_frames : report_interval),
      scope_("runtime/" + std::to_string(instance)) {
  SL_DEBUG_ASSERT(instance != 0, "diagnostics: the runtime instance starts at 1");
}

void diagnostics::add(const tracking_resource &tracked) {
  tracked_.push_back(&tracked);
  allocs_at_frame_end_ += tracked.totals().allocs;
}

void diagnostics::add(std::string name, const resource &source) {
  auto entry = std::make_unique<allocator_source>();
  entry->name = std::move(name);
  entry->source = &source;
  allocators_.push_back(std::move(entry));
  sample_allocators();
}

result<void> diagnostics::report_external(memory_tag tag, std::uint64_t bytes) noexcept {
  if (!tags_->contains(tag) || !tags_->is_external(tag)) {
    return make_error(errc::invalid_argument,
                      "report_external: tag " + std::to_string(tag.id) +
                          " is not an external tag (reserve_external: gpu.* or fabric.*)");
  }
  // pairs: make_report (relaxed statistics; the bytes publish nothing else)
  std::span(external_)[tag.id].store(bytes, std::memory_order_relaxed);
  return {};
}

void diagnostics::sample_allocators() noexcept {
  for (const auto &a : allocators_) {
    const free_space f = a->source->free_space_stats();
    // pairs: make_report (relaxed statistics)
    a->free_bytes.store(f.free_bytes, std::memory_order_relaxed);
    // pairs: make_report (relaxed statistics)
    a->largest_free_run.store(f.largest_free_run, std::memory_order_relaxed);
  }
}

std::uint64_t diagnostics::tracked_allocs() const noexcept {
  return std::accumulate(
      tracked_.begin(), tracked_.end(), std::uint64_t{0},
      [](std::uint64_t sum, const tracking_resource *r) { return sum + r->totals().allocs; });
}

std::string diagnostics::tag_name(memory_tag tag) const { return std::string(tags_->name(tag)); }

memory_report diagnostics::make_report(std::uint64_t frame) const {
  memory_report report;
  report.instance = instance_;
  report.frame = frame;
  const std::size_t registered = tags_->size();
  for (std::size_t id = 0; id <= registered && id <= max_memory_tags; ++id) {
    const memory_tag tag{static_cast<std::uint16_t>(id)};
    if (id != 0 && tags_->is_external(tag)) {
      continue; // external tags are reported separately, never as tracked bytes (ARCH3-R056)
    }
    tag_counters sum;
    for (const tracking_resource *r : tracked_) {
      add_counters(sum, r->counters(tag));
    }
    if (id == 0 && !any_counter(sum)) {
      continue; // untagged appears only when something was counted under it
    }
    // A reader may see a live count before the peak CAS or a free before its allocation count.
    const std::uint64_t peak =
        raise_to(std::span(peak_seen_)[id], std::max(sum.peak_bytes, sum.live_bytes));
    report.tags.push_back(tag_stats{.tag = tag_name(tag),
                                    .live_bytes = sum.live_bytes,
                                    .live_blocks = sum.live_blocks,
                                    .peak_bytes = peak,
                                    .allocs = std::max(sum.allocs, sum.frees),
                                    .frees = sum.frees,
                                    .failures = sum.failures});
  }
  std::ranges::sort(report.tags, {}, &tag_stats::tag);

  for (const auto &a : allocators_) {
    // pairs: sample_allocators (relaxed statistics)
    const std::uint64_t free_bytes = a->free_bytes.load(std::memory_order_relaxed);
    // pairs: sample_allocators (relaxed statistics)
    const std::uint64_t run = a->largest_free_run.load(std::memory_order_relaxed);
    report.allocators.push_back(
        allocator_stats{.name = a->name,
                        .free_bytes = free_bytes,
                        .largest_free_run = run,
                        .fragmentation_permille = fragmentation_permille(free_bytes, run)});
  }
  std::ranges::stable_sort(report.allocators, {}, &allocator_stats::name);

  for (std::size_t id = 1; id <= registered && id <= max_memory_tags; ++id) {
    const memory_tag tag{static_cast<std::uint16_t>(id)};
    if (tags_->is_external(tag)) {
      // pairs: report_external (relaxed statistics)
      const std::uint64_t bytes = std::span(external_)[id].load(std::memory_order_relaxed);
      report.external.push_back(external_stats{.tag = tag_name(tag), .bytes = bytes});
    }
  }
  std::ranges::sort(report.external, {}, &external_stats::tag);

  // pairs: on_frame_end (relaxed statistics)
  report.upstream_allocs_last_frame = upstream_last_frame_.load(std::memory_order_relaxed);
  return report;
}

std::vector<leak_entry> diagnostics::leaks() const {
  std::vector<leak_entry> out;
  for (const tracking_resource *r : tracked_) {
    if constexpr (debug_memory) {
      std::ranges::transform(r->leaks(), std::back_inserter(out), [&](const leak_record &l) {
        return leak_entry{.tag = tags_->contains(l.tag) ? tag_name(l.tag) : r->tag_label(l.tag),
                          .site = std::string(l.file) + ":" + std::to_string(l.line),
                          .size = l.size};
      });
    } else {
      for (std::size_t id = 0; id <= max_memory_tags; ++id) {
        const memory_tag tag{static_cast<std::uint16_t>(id)};
        const tag_counters c = r->counters(tag);
        if (c.live_blocks != 0) {
          out.push_back(leak_entry{.tag = tags_->contains(tag) ? tag_name(tag) : r->tag_label(tag),
                                   .site = "unknown",
                                   .size = c.live_bytes});
        }
      }
    }
  }
  std::ranges::sort(out, [](const leak_entry &a, const leak_entry &b) {
    return std::tie(a.tag, a.site, a.size) < std::tie(b.tag, b.site, b.size);
  });
  return out;
}

} // namespace sl::runtime::mem
