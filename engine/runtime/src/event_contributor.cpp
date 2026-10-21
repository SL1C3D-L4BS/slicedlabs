// runtime.events, version 1 (WO-092, SL-ARCH-003 ARCH3-R091, `determinism` pending events:
// snapshotted (contributor), hashed without correlation; correlation ids are transient-diagnostic).
// The bytes are a u32le channel count, then per channel in ascending type id its type id and both
// frame lists (previous, current): a u64le frame index, a u32le count and per event the envelope
// (type u32, seq u32, system_index u16, tick u64, a cause flag u8 and, when set, the cause frame
// u64, type u32 and seq u32) and a u32le-length payload of the fields flagged snapshot or hashed.
// No correlation is written, so perturbed correlation ids change no byte. Restore stages every
// channel first and commits only when all of them parse (snapshot_corrupt otherwise, nothing
// changes); restored correlations are the default {0, "process", 0}.

#include "sl/runtime/events.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sl::runtime {

namespace detail {

namespace {

void put_le(std::vector<std::byte> &out, std::uint64_t v, unsigned width) noexcept {
  for (unsigned i = 0; i < width; ++i) {
    out.push_back(static_cast<std::byte>((v >> (8U * i)) & 0xFFU));
  }
}

template <class T>
bool get_le(std::span<const std::byte> in, std::size_t &pos, T &v, unsigned width) noexcept {
  if (pos > in.size() || in.size() - pos < width) {
    return false;
  }
  std::uint64_t wide = 0;
  for (unsigned i = 0; i < width; ++i) {
    wide |= std::to_integer<std::uint64_t>(in[pos + i]) << (8U * i);
  }
  v = static_cast<T>(wide);
  pos += width;
  return true;
}

} // namespace

void put_u32(std::vector<std::byte> &out, std::uint32_t v) noexcept { put_le(out, v, 4); }

void put_u64(std::vector<std::byte> &out, std::uint64_t v) noexcept { put_le(out, v, 8); }

bool get_u32(std::span<const std::byte> in, std::size_t &pos, std::uint32_t &v) noexcept {
  return get_le(in, pos, v, 4);
}

bool get_u64(std::span<const std::byte> in, std::size_t &pos, std::uint64_t &v) noexcept {
  return get_le(in, pos, v, 8);
}

void put_envelope(const event_envelope &e, std::vector<std::byte> &out) noexcept {
  put_u32(out, e.type.value);
  put_u32(out, e.seq);
  put_le(out, e.system_index, 2);
  put_u64(out, e.tick);
  out.push_back(std::byte{e.cause.has_value() ? std::uint8_t{1} : std::uint8_t{0}});
  if (e.cause) {
    put_u64(out, e.cause->frame);
    put_u32(out, e.cause->type.value);
    put_u32(out, e.cause->seq);
  }
}

bool get_envelope(std::span<const std::byte> in, std::size_t &pos, event_envelope &e) noexcept {
  event_envelope parsed;
  std::uint8_t flag = 0;
  if (!get_u32(in, pos, parsed.type.value) || !get_u32(in, pos, parsed.seq) ||
      !get_le(in, pos, parsed.system_index, 2) || !get_u64(in, pos, parsed.tick) ||
      !get_le(in, pos, flag, 1) || flag > 1) {
    return false;
  }
  if (flag == 1) {
    event_ref cause;
    if (!get_u64(in, pos, cause.frame) || !get_u32(in, pos, cause.type.value) ||
        !get_u32(in, pos, cause.seq)) {
      return false;
    }
    parsed.cause = cause;
  }
  e = parsed;
  return true;
}

void hash_envelope(const event_envelope &e, det::state_hasher &h) noexcept {
  h.u32(e.type.value);
  h.u32(e.seq);
  h.u16(e.system_index);
  h.u64(e.tick);
  h.boolean(e.cause.has_value());
  if (e.cause) {
    h.u64(e.cause->frame);
    h.u32(e.cause->type.value);
    h.u32(e.cause->seq);
  }
}

} // namespace detail

namespace {

bool by_type(const event_stream *a, const event_stream *b) noexcept {
  return a->type() < b->type();
}

} // namespace

events_contributor::events_contributor(std::span<event_stream *const> channels)
    : channels_(channels.begin(), channels.end()) {
  std::ranges::sort(channels_, &by_type);
}

result<void> events_contributor::capture(std::vector<std::byte> &out) const {
  detail::put_u32(out, static_cast<std::uint32_t>(channels_.size()));
  for (const event_stream *s : channels_) {
    s->capture(out);
  }
  return {};
}

result<void> events_contributor::restore(std::span<const std::byte> in) {
  std::size_t pos = 0;
  std::uint32_t count = 0;
  if (!detail::get_u32(in, pos, count) || count != channels_.size()) {
    return make_error(errc::snapshot_corrupt, "runtime.events: the channel count does not match");
  }
  // all_of stops at the first channel that does not parse.
  const bool ok =
      std::ranges::all_of(channels_, [&](event_stream *s) { return s->stage(in, pos); }) &&
      pos == in.size();
  if (!ok) {
    for (event_stream *s : channels_) {
      s->discard_staged();
    }
    return make_error(errc::snapshot_corrupt, "runtime.events: the event lists do not parse");
  }
  for (event_stream *s : channels_) {
    s->commit_staged();
  }
  return {};
}

void events_contributor::hash(det::state_hasher &h) const {
  h.u32(static_cast<std::uint32_t>(channels_.size()));
  for (const event_stream *s : channels_) {
    s->hash(h);
  }
}

} // namespace sl::runtime
