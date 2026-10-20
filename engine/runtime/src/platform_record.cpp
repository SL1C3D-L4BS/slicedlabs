// record_sink defaults and the recording helpers (WO-078, SL-ARCH-003 ARCH3-R039): a wall sample,
// each polled input event and each file read pass the sink exactly once, in arrival order, with
// the frame and tick the caller (the frame scheduler, WO-089) passes.

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/platform.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace sl::runtime {

void record_sink::on_wall_sample(std::uint64_t /*frame*/, nanos /*sample*/) noexcept {}

void record_sink::on_input(std::uint64_t /*frame*/, std::uint64_t /*tick*/,
                           const input_event & /*event*/) noexcept {}

void record_sink::on_file_read(std::uint64_t /*frame*/, std::uint64_t /*tick*/,
                               std::string_view /*path*/,
                               const sl::fabric::digest & /*content*/) noexcept {}

void record_sink::on_external(std::uint64_t /*frame*/, std::uint64_t /*tick*/,
                              std::uint32_t /*kind*/,
                              std::span<const std::byte> /*bytes*/) noexcept {}

sl::fabric::digest content_digest(std::span<const std::byte> bytes) noexcept {
  std::vector<std::uint8_t> data(bytes.size());
  std::ranges::transform(bytes, data.begin(),
                         [](std::byte b) noexcept { return std::to_integer<std::uint8_t>(b); });
  return sl::fabric::digest{.algo = sl::fabric::hash_algo::sha256,
                            .bytes = sl::fabric::sha256::hash(sl::fabric::byte_span{data})};
}

nanos record_wall_sample(const clock_source &clock, record_sink &sink,
                         std::uint64_t frame) noexcept {
  const nanos sample = clock.now();
  sink.on_wall_sample(frame, sample);
  return sample;
}

std::size_t record_input(input_source &input, record_sink &sink, std::uint64_t frame,
                         std::uint64_t tick, std::span<input_event> buffer) noexcept {
  const std::size_t count = std::min(input.poll(buffer), buffer.size());
  for (const input_event &event : buffer.first(count)) {
    sink.on_input(frame, tick, event);
  }
  return count;
}

result<std::vector<std::byte>> record_file_read(const file_access &files, record_sink &sink,
                                                std::uint64_t frame, std::uint64_t tick,
                                                std::string_view path) noexcept {
  auto bytes = files.read(path);
  if (bytes) {
    sink.on_file_read(frame, tick, path, content_digest(*bytes));
  }
  return bytes;
}

} // namespace sl::runtime
