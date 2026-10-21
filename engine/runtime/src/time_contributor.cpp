// runtime.clock, version 1 (WO-084, SL-ARCH-003 ARCH3-R091, `determinism` row logical tick,
// fixed-stepper accumulator, frame index: hashed (tick), snapshotted (all)). The bytes are 58,
// little-endian: tick u64, stepper accumulator u64, dropped ticks u64, frame clock previous sample
// i64, has_previous u8, frozen u8, non_monotonic u64, dropped_ns u64, frame index u64. Restore
// checks the length, the two flags and that the accumulator is below one tick before it changes
// anything.

#include "sl/runtime/error.hpp"
#include "sl/runtime/replay.hpp"
#include "sl/runtime/time.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sl::runtime::det {

namespace {

constexpr std::size_t clock_bytes = 58;

void put_u64(std::vector<std::byte> &out, std::uint64_t v) {
  for (unsigned i = 0; i < 8U; ++i) {
    out.push_back(static_cast<std::byte>((v >> (8U * i)) & 0xFFU));
  }
}

class reader {
public:
  explicit reader(std::span<const std::byte> in) noexcept : in_(in) {}

  std::uint64_t u64() noexcept {
    std::uint64_t v = 0;
    for (unsigned i = 0; i < 8U; ++i) {
      v |= std::to_integer<std::uint64_t>(in_[pos_ + i]) << (8U * i);
    }
    pos_ += 8;
    return v;
  }

  std::uint8_t u8() noexcept { return std::to_integer<std::uint8_t>(in_[pos_++]); }

private:
  std::span<const std::byte> in_;
  std::size_t pos_ = 0;
};

} // namespace

clock_contributor::clock_contributor(time::fixed_stepper &stepper, time::logical_clock &logical,
                                     time::frame_clock &frame) noexcept
    : stepper_(&stepper), logical_(&logical), frame_(&frame) {}

result<void> clock_contributor::capture(std::vector<std::byte> &out) const {
  const time::fixed_stepper::state s = stepper_->capture();
  const time::frame_clock::state f = frame_->capture();
  out.reserve(out.size() + clock_bytes);
  put_u64(out, logical_->now());
  put_u64(out, s.accumulator);
  put_u64(out, s.dropped_ticks);
  put_u64(out, static_cast<std::uint64_t>(f.previous_ns));
  out.push_back(f.has_previous ? std::byte{1} : std::byte{0});
  out.push_back(f.frozen ? std::byte{1} : std::byte{0});
  put_u64(out, f.non_monotonic);
  put_u64(out, f.dropped_ns);
  put_u64(out, f.frame_index);
  return {};
}

result<void> clock_contributor::restore(std::span<const std::byte> in) {
  if (in.size() != clock_bytes) {
    return make_error(errc::snapshot_corrupt, "runtime.clock: the state is not 58 bytes");
  }
  reader r(in);
  const time::tick t = r.u64();
  time::fixed_stepper::state s;
  s.accumulator = r.u64();
  s.dropped_ticks = r.u64();
  time::frame_clock::state f;
  f.previous_ns = static_cast<std::int64_t>(r.u64());
  const std::uint8_t has_previous = r.u8();
  const std::uint8_t frozen = r.u8();
  f.non_monotonic = r.u64();
  f.dropped_ns = r.u64();
  f.frame_index = r.u64();
  if (has_previous > 1 || frozen > 1) {
    return make_error(errc::snapshot_corrupt, "runtime.clock: a flag byte is not 0 or 1");
  }
  if (s.accumulator >= stepper_->remainder().den) {
    return make_error(errc::snapshot_corrupt,
                      "runtime.clock: the accumulator is not below one tick of this rate");
  }
  f.has_previous = has_previous == 1;
  f.frozen = frozen == 1;
  logical_->restore(t);
  stepper_->restore(s);
  frame_->restore(f);
  return {};
}

void clock_contributor::hash(state_hasher &h) const { h.u64(logical_->now()); }

} // namespace sl::runtime::det
