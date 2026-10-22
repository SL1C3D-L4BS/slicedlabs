// event_channel<E> support (WO-092, SL-ARCH-003 ARCH3-R139 to R141, `failpoints` RFP7, `limits`
// event_capacity_default, ADR-0026): the capacity check and drop counters every channel shares,
// the once-per-frame overflow warning (system runtime.events, at end_frame only, so steady frames
// log nothing), the Debug payload check (every f32 and f64 field finite) and attach, which ties a
// channel to the frame scheduler's phase end hooks and its arena_flip boundary slot. Once the
// frame is marked faulted (frame_scheduler::frame_faulted, ARCH3-R028) the hooks skip deliver and
// the flip keeps the older list and drops the undelivered writes instead (WO-109).

#include "sl/runtime/events.hpp"

#include "sl/core/log.hpp"
#include "sl/runtime/faults.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace sl::runtime {

namespace {

// The phase end hook: the frame's events become readable, unless the frame faulted.
struct deliver_hook {
  event_stream *stream = nullptr;
  const frame_scheduler *fs = nullptr;
  void operator()(phase /*ph*/, time::tick /*t*/) const noexcept {
    if (!fs->frame_faulted()) {
      stream->deliver(fs->current().frame_index);
    }
  }
};

// The arena_flip slot: the older frame list is dropped; a faulted frame keeps it.
struct flip_hook {
  event_stream *stream = nullptr;
  const frame_scheduler *fs = nullptr;
  void operator()(const boundary_context & /*ctx*/) const noexcept {
    if (fs->frame_faulted()) {
      stream->drop_pending();
    } else {
      stream->end_frame();
    }
  }
};

} // namespace

event_stream::event_stream(event_type_id id, std::string_view name, const type_info &info) noexcept
    : type_(id), name_(name), info_(&info) {}

bool event_stream::refuse(std::size_t delivered, std::size_t pending) noexcept {
  bool full = delivered + pending >= event_capacity_default;
#if SL_RUNTIME_FAULTS
  if (visit(failpoint::rfp7).fire) {
    full = true; // RFP7: the channel behaves as full
  }
#endif
  if (full) {
    count_drop();
  }
  return full;
}

void event_stream::count_drop() noexcept {
  dropped_.fetch_add(1);
  frame_dropped_.fetch_add(1);
}

void event_stream::close_frame(std::uint64_t frame) noexcept {
  const std::uint64_t n = frame_dropped_.exchange(0);
  if (n != 0) {
    sl::log::emit(sl::log::level::warn, "runtime.events", "event overflow",
                  {{"type", std::string_view{name_}}, {"frame", frame}, {"dropped", n}});
  }
}

namespace detail {

namespace {

// True unless `f` is a f32 or f64 field of the object at `base` that is NaN or infinite.
bool finite_field(const std::byte *base, const field_info &f) noexcept {
  if (f.kind == field_kind::f32) {
    float v = 0.0F;
    std::memcpy(&v, base + f.offset, sizeof v);
    return std::isfinite(v);
  }
  if (f.kind == field_kind::f64) {
    double v = 0.0;
    std::memcpy(&v, base + f.offset, sizeof v);
    return std::isfinite(v);
  }
  return true;
}

} // namespace

bool payload_valid(const type_info &t, const void *obj) noexcept {
  const auto *base = static_cast<const std::byte *>(obj);
  return std::ranges::all_of(
      t.fields, [base](const field_info &f) noexcept { return finite_field(base, f); });
}

} // namespace detail

void attach(event_stream &stream, frame_scheduler &fs) {
  fs.add_phase_end_hook(deliver_hook{.stream = &stream, .fs = &fs});
  fs.add_boundary_hook(boundary_slot::arena_flip, flip_hook{.stream = &stream, .fs = &fs});
}

} // namespace sl::runtime
