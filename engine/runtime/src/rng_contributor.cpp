// runtime.rng, version 1 (WO-084, SL-ARCH-003 ARCH3-R091, `determinism` row rng_stream state:
// snapshotted (contributor), hashed). The bytes are a u32le stream count, then per stream in
// registration order a u32le length and the rng_stream::capture text. Restore parses every stream
// into a copy first and commits only when all of them parse, so corrupt bytes change nothing; the
// hash is the capture text of each stream through state_hasher::str.

#include "sl/runtime/error.hpp"
#include "sl/runtime/replay.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sl::runtime::det {

namespace {

void put_u32(std::vector<std::byte> &out, std::uint32_t v) {
  for (unsigned i = 0; i < 4U; ++i) {
    out.push_back(static_cast<std::byte>((v >> (8U * i)) & 0xFFU));
  }
}

bool get_u32(std::span<const std::byte> in, std::size_t &pos, std::uint32_t &v) noexcept {
  if (in.size() - pos < 4) {
    return false;
  }
  v = 0;
  for (unsigned i = 0; i < 4U; ++i) {
    v |= std::to_integer<std::uint32_t>(in[pos + i]) << (8U * i);
  }
  pos += 4;
  return true;
}

} // namespace

rng_contributor::rng_contributor(std::span<rng_stream *const> streams)
    : streams_(streams.begin(), streams.end()) {}

result<void> rng_contributor::capture(std::vector<std::byte> &out) const {
  put_u32(out, static_cast<std::uint32_t>(streams_.size()));
  for (const rng_stream *s : streams_) {
    const std::string text = s->capture();
    put_u32(out, static_cast<std::uint32_t>(text.size()));
    const std::span<const std::byte> raw = std::as_bytes(std::span(text));
    out.insert(out.end(), raw.begin(), raw.end());
  }
  return {};
}

result<void> rng_contributor::restore(std::span<const std::byte> in) {
  std::size_t pos = 0;
  std::uint32_t count = 0;
  if (!get_u32(in, pos, count) || count != streams_.size()) {
    return make_error(errc::snapshot_corrupt, "runtime.rng: the stream count does not match");
  }
  std::vector<rng_stream> parsed;
  parsed.reserve(streams_.size());
  for (const rng_stream *s : streams_) {
    std::uint32_t length = 0;
    if (!get_u32(in, pos, length) || in.size() - pos < length) {
      return make_error(errc::snapshot_corrupt, "runtime.rng: a stream state is truncated");
    }
    std::string text(length, '\0');
    for (std::size_t i = 0; i < length; ++i) {
      text[i] = static_cast<char>(std::to_integer<unsigned char>(in[pos + i]));
    }
    pos += length;
    rng_stream copy = *s;
    if (auto restored = copy.restore(text); !restored) {
      return restored;
    }
    parsed.push_back(copy);
  }
  if (pos != in.size()) {
    return make_error(errc::snapshot_corrupt, "runtime.rng: trailing bytes after the streams");
  }
  for (std::size_t i = 0; i < streams_.size(); ++i) {
    *streams_[i] = parsed[i];
  }
  return {};
}

void rng_contributor::hash(state_hasher &h) const {
  h.u32(static_cast<std::uint32_t>(streams_.size()));
  for (const rng_stream *s : streams_) {
    h.str(s->capture());
  }
}

} // namespace sl::runtime::det
