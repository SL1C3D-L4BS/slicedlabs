// The runtime.mem log event (WO-085, SL-ARCH-003 ARCH3-R053, master plan section 3 F6).
// on_frame_end runs on the owner thread in the end_frame slot diagnostics: it samples the
// allocators, records the tracked allocations of the frame and every report_interval frames emits
// one info event under the correlation scope runtime/<instance>. sl::log::emit takes a fixed field
// list, so the per-tag live and peak bytes travel in one string field, `tags`, as
// "<tag>=<live>/<peak>" joined by "," in tag order, and the external bytes in `external` as
// "<tag>=<bytes>".

#include "sl/core/log.hpp"
#include "sl/runtime/memdiag.hpp"
#include "sl/runtime/memory.hpp"

#include <atomic>
#include <cstdint>
#include <string>

namespace sl::runtime::mem {

void diagnostics::on_frame_end(std::uint64_t frame) {
  sample_allocators();
  const std::uint64_t allocs = tracked_allocs();
  const std::uint64_t made = allocs >= allocs_at_frame_end_ ? allocs - allocs_at_frame_end_ : 0;
  allocs_at_frame_end_ = allocs;
  // pairs: make_report (relaxed statistics)
  upstream_last_frame_.store(made, std::memory_order_relaxed);
  if (frame == 0 || frame % report_interval_ != 0) {
    return;
  }

  const memory_report report = make_report(frame);
  std::uint64_t live_bytes = 0;
  std::uint64_t live_blocks = 0;
  std::uint64_t peak_bytes = 0;
  std::string tags;
  for (const tag_stats &t : report.tags) {
    live_bytes += t.live_bytes;
    live_blocks += t.live_blocks;
    peak_bytes += t.peak_bytes;
    if (!tags.empty()) {
      tags += ',';
    }
    tags += t.tag + '=' + std::to_string(t.live_bytes) + '/' + std::to_string(t.peak_bytes);
  }
  std::uint64_t external_bytes = 0;
  std::string external;
  for (const external_stats &e : report.external) {
    external_bytes += e.bytes;
    if (!external.empty()) {
      external += ',';
    }
    external += e.tag + '=' + std::to_string(e.bytes);
  }

  const sl::log::correlation_scope correlation(scope_, frame);
  sl::log::emit(sl::log::level::info, "runtime.mem", "memory report",
                {{"instance", instance_},
                 {"frame", frame},
                 {"live_bytes", live_bytes},
                 {"live_blocks", live_blocks},
                 {"peak_bytes", peak_bytes},
                 {"external_bytes", external_bytes},
                 {"upstream_allocs_last_frame", report.upstream_allocs_last_frame},
                 {"tags", tags},
                 {"external", external}});
}

} // namespace sl::runtime::mem
