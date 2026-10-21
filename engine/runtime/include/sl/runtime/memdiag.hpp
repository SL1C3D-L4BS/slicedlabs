#pragma once

// Allocation diagnostics (WO-085, SL-ARCH-003 ARCH3-R052 to R056, ADR-0023; episode 085).
// A diagnostics object reads the per-tag counters of the tracking_resources added to it, the free
// space of the allocators added as fragmentation sources and the external bytes reported for the
// reserved tags (fabric.projection, gpu.*), and makes memory_report values: per tag live bytes and
// blocks, peak bytes, allocation, free and failure counts; per allocator free bytes, largest free
// run and fragmentation in per-mille; the external tags separately (never in tracked live bytes);
// the leak report sorted by (tag, site, size). to_json and from_json write and read
// sl-runtime-memreport/1 (schemas/runtime/memreport.schema.json) byte-identically for equal
// reports. on_frame_end emits one runtime.mem log event every report_interval frames. Layer L2:
// includes only L0 and L1 runtime headers.
// Thread safety (`thread_safety`): make_report, report_external, to_json and from_json are
// any-thread and never block allocation (make_report does relaxed loads only); add, on_frame_end
// and leaks are owner-thread (add before reports are read from other threads; leaks at shutdown).

#include "sl/core/error.hpp"
#include "sl/runtime/memory.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sl::runtime::mem {

// --- limits (SL-ARCH-003 `limits`) ---

/// The frames between two runtime.mem log events (`limits` report_interval_frames).
inline constexpr std::uint32_t report_interval_frames = 600;
/// The format tag of a serialized memory report.
inline constexpr std::string_view memreport_format = "sl-runtime-memreport/1";

// --- report values (ARCH3-R052) ---

/// The counters of one tracked tag, summed over every tracking_resource of the diagnostics.
struct tag_stats {
  std::string tag;
  std::uint64_t live_bytes = 0;
  std::uint64_t live_blocks = 0;
  std::uint64_t peak_bytes = 0;
  std::uint64_t allocs = 0;
  std::uint64_t frees = 0;
  std::uint64_t failures = 0;
  friend bool operator==(const tag_stats &, const tag_stats &) = default;
};

/// The free space of one allocator and its fragmentation in per-mille.
struct allocator_stats {
  std::string name;
  std::uint64_t free_bytes = 0;
  std::uint64_t largest_free_run = 0;
  std::uint32_t fragmentation_permille = 0;
  friend bool operator==(const allocator_stats &, const allocator_stats &) = default;
};

/// The bytes last reported through report_external for one external tag.
struct external_stats {
  std::string tag;
  std::uint64_t bytes = 0;
  friend bool operator==(const external_stats &, const external_stats &) = default;
};

/// One live block at shutdown: its tag, its site ("file:line"; "unknown" in Release) and size.
struct leak_entry {
  std::string tag;
  std::string site;
  std::uint64_t size = 0;
  friend bool operator==(const leak_entry &, const leak_entry &) = default;
};

/// A memory report (a value owned by the caller). make_report sorts tags by name, allocators by
/// name, external by tag and leaves leaks empty; the shutdown report takes leaks from leaks().
struct memory_report {
  std::uint32_t instance = 0;
  std::uint64_t frame = 0;
  std::vector<tag_stats> tags;
  std::vector<allocator_stats> allocators;
  std::vector<external_stats> external;
  std::vector<leak_entry> leaks;
  std::uint64_t upstream_allocs_last_frame = 0;
  friend bool operator==(const memory_report &, const memory_report &) = default;
};

/// 1000 - (1000 x largest_free_run) / free_bytes in integers, 0 with no free space; a run larger
/// than the free bytes counts as the free bytes (0). Values too large for 1000 x run are halved
/// together until the product fits.
[[nodiscard]] constexpr std::uint32_t
fragmentation_permille(std::uint64_t free_bytes, std::uint64_t largest_free_run) noexcept {
  if (free_bytes == 0 || largest_free_run >= free_bytes) {
    return 0;
  }
  constexpr std::uint64_t fits = ~std::uint64_t{0} / 1000U;
  while (largest_free_run > fits) {
    largest_free_run >>= 1U;
    free_bytes >>= 1U;
  }
  return static_cast<std::uint32_t>(1000U - ((1000U * largest_free_run) / free_bytes));
}

// --- diagnostics (ARCH3-R052 to R056) ---

/// The memory diagnostics of one runtime instance. Neither copyable nor movable.
class diagnostics {
public:
  /// `tags` names every tag and outlives the diagnostics; `instance` is the runtime instance (1 or
  /// more; the correlation scope is runtime/<instance>); a report_interval of 0 means
  /// report_interval_frames.
  diagnostics(const tag_registry &tags, std::uint32_t instance,
              std::uint32_t report_interval = report_interval_frames);
  diagnostics(const diagnostics &) = delete;
  diagnostics(diagnostics &&) = delete;
  diagnostics &operator=(const diagnostics &) = delete;
  diagnostics &operator=(diagnostics &&) = delete;
  ~diagnostics() = default;

  /// Adds a counter source (owner thread, before reports are read elsewhere); it outlives the
  /// diagnostics. Its counters are summed per tag with those of the other tracking resources.
  void add(const tracking_resource &tracked);
  /// Adds a fragmentation source named `name` (owner thread, as above); it outlives the
  /// diagnostics. Its free_space_stats is sampled now and at every on_frame_end, on the owner
  /// thread, so an owner-thread allocator need not be wrapped.
  void add(std::string name, const resource &source);

  /// Sets the bytes of an external tag (any thread; relaxed store). invalid_argument unless `tag`
  /// was registered through reserve_external. The bytes never enter tracked live bytes.
  [[nodiscard]] result<void> report_external(memory_tag tag, std::uint64_t bytes) noexcept;

  /// A report of frame `frame` (any thread; relaxed loads only, never blocks allocation): every
  /// registered non-external tag (and untagged when it has counters) by name, the last allocator
  /// samples by name, every external tag by name, no leaks. live <= peak and frees <= allocs hold
  /// in every report, and the peak never decreases between two reports.
  [[nodiscard]] memory_report make_report(std::uint64_t frame) const;

  /// Owner thread, end_frame slot diagnostics: samples the allocators, records the allocations of
  /// the tracking resources since the previous call (upstream_allocs_last_frame) and, when `frame`
  /// is a non-zero multiple of report_interval, emits one info event of system runtime.mem under
  /// the correlation scope runtime/<instance> with tick `frame`.
  void on_frame_end(std::uint64_t frame);

  /// The live blocks of every tracking resource sorted by (tag, site, size) (owner thread, at
  /// shutdown). Release builds keep no sites: one entry per tag with live blocks, site "unknown",
  /// size its live bytes.
  [[nodiscard]] std::vector<leak_entry> leaks() const;

  [[nodiscard]] std::uint32_t instance() const noexcept { return instance_; }
  [[nodiscard]] std::uint32_t report_interval() const noexcept { return report_interval_; }
  /// The correlation scope of the log events: "runtime/<instance>".
  [[nodiscard]] std::string_view scope() const noexcept { return scope_; }

private:
  struct allocator_source {
    std::string name;
    const resource *source = nullptr;
    std::atomic<std::uint64_t> free_bytes{0};
    std::atomic<std::uint64_t> largest_free_run{0};
  };

  void sample_allocators() noexcept;
  [[nodiscard]] std::uint64_t tracked_allocs() const noexcept;
  [[nodiscard]] std::string tag_name(memory_tag tag) const;

  const tag_registry *tags_;
  std::uint32_t instance_;
  std::uint32_t report_interval_;
  std::string scope_;
  std::vector<const tracking_resource *> tracked_;
  std::vector<std::unique_ptr<allocator_source>> allocators_;
  std::array<std::atomic<std::uint64_t>, max_memory_tags + 1> external_{};
  // The largest peak any report has shown per tag: keeps reported peaks monotone.
  mutable std::array<std::atomic<std::uint64_t>, max_memory_tags + 1> peak_seen_{};
  std::uint64_t allocs_at_frame_end_ = 0;
  std::atomic<std::uint64_t> upstream_last_frame_{0};
};

// --- sl-runtime-memreport/1 (ARCH3-R053) ---

/// The report as sl-runtime-memreport/1: keys in schema order, two-space indentation, integers
/// only, one trailing newline; tags, allocators, external and leaks are written sorted (leaks by
/// (tag, site, size)), so equal reports give identical bytes.
[[nodiscard]] std::string to_json(const memory_report &report);
/// Parses sl-runtime-memreport/1: invalid_argument naming the key (a JSON pointer such as
/// /tags/0/live_bytes) for a missing, unknown, duplicate or mistyped key or a value out of the
/// schema's range, or naming the offset of malformed JSON.
[[nodiscard]] result<memory_report> from_json(std::string_view text);

} // namespace sl::runtime::mem
