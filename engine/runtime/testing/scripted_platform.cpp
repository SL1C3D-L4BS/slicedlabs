// The scripted test platform (WO-078, SL-ARCH-003 ARCH3-R036, R039). See scripted_platform.hpp.

#include "scripted_platform.hpp"

#include "faults_arm.hpp"

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/faults.hpp"
#include "sl/runtime/platform.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sl::runtime::testing {

namespace {

void put_u64(std::vector<std::byte> &out, std::uint64_t v) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<std::byte>((v >> static_cast<unsigned>(shift)) & 0xFFU));
  }
}

void put_bytes(std::vector<std::byte> &out, std::span<const std::byte> bytes) {
  put_u64(out, bytes.size());
  out.insert(out.end(), bytes.begin(), bytes.end());
}

} // namespace

void scripted_input::push(const input_event &event) { queue_.push_back(event); }

std::size_t scripted_input::poll(std::span<input_event> out) noexcept {
  const std::size_t count = std::min(out.size(), queue_.size());
  for (std::size_t i = 0; i < count; ++i) {
    out[i] = queue_.front();
    queue_.pop_front();
  }
  return count;
}

result<thread_handle> failing_thread_factory::spawn(std::string name, std::function<void()> body,
                                                    affinity_hint hint) noexcept {
  ++spawns_;
  if (fail_at_ != 0 && spawns_ == fail_at_) {
    return make_error(errc::thread_spawn_failed, "failing_thread_factory: scripted failure");
  }
  return inner_.spawn(std::move(name), std::move(body), hint);
}

void failing_thread_factory::fail_at(std::uint64_t k) noexcept {
  fail_at_ = k == 0 ? 0 : spawns_ + k;
}

result<void> failing_thread_factory::arm_rfp1(std::uint64_t k, bool sticky) {
  return arm(failpoint::rfp1, k, sticky);
}

void recording_sink::on_wall_sample(std::uint64_t frame, nanos sample) noexcept {
  record r;
  r.kind = record_kind::wall_sample;
  r.frame = frame;
  r.sample = sample.count();
  records_.push_back(std::move(r));
}

void recording_sink::on_input(std::uint64_t frame, std::uint64_t tick,
                              const input_event &event) noexcept {
  const std::size_t size = std::min<std::size_t>(event.size, event.payload.size());
  record r;
  r.kind = record_kind::input;
  r.frame = frame;
  r.tick = tick;
  r.code = event.type;
  r.data.assign(event.payload.begin(), event.payload.begin() + static_cast<std::ptrdiff_t>(size));
  records_.push_back(std::move(r));
}

void recording_sink::on_file_read(std::uint64_t frame, std::uint64_t tick, std::string_view path,
                                  const sl::fabric::digest &content) noexcept {
  std::vector<std::byte> data;
  data.reserve(content.bytes.size() + 1);
  data.push_back(static_cast<std::byte>(content.algo));
  for (const std::uint8_t b : content.bytes) {
    data.push_back(std::byte{b});
  }
  record r;
  r.kind = record_kind::file_read;
  r.frame = frame;
  r.tick = tick;
  r.path = std::string(path);
  r.data = std::move(data);
  records_.push_back(std::move(r));
}

void recording_sink::on_external(std::uint64_t frame, std::uint64_t tick, std::uint32_t kind,
                                 std::span<const std::byte> bytes) noexcept {
  record r;
  r.kind = record_kind::external;
  r.frame = frame;
  r.tick = tick;
  r.code = kind;
  r.data.assign(bytes.begin(), bytes.end());
  records_.push_back(std::move(r));
}

std::vector<std::byte> recording_sink::serialize() const {
  std::vector<std::byte> out;
  for (const record &r : records_) {
    out.push_back(static_cast<std::byte>(r.kind));
    put_u64(out, r.frame);
    put_u64(out, r.tick);
    put_u64(out, r.code);
    put_u64(out, static_cast<std::uint64_t>(r.sample));
    put_bytes(out, std::as_bytes(std::span<const char>(r.path.data(), r.path.size())));
    put_bytes(out, r.data);
  }
  return out;
}

scripted_platform::scripted_platform(std::uint32_t instance_id)
    : threads_(instance_id), files_(vfs_, "/"), platform_{.clock = clock_,
                                                          .wall = wall_,
                                                          .threads = threads_,
                                                          .input = input_,
                                                          .files = files_,
                                                          .win = win_,
                                                          .sink = sink_,
                                                          .instance_id = instance_id} {}

} // namespace sl::runtime::testing
