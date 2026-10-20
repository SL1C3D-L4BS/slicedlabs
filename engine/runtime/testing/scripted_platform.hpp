#pragma once

// The scripted test platform (WO-078, SL-ARCH-003 ARCH3-R036, R039): a manual clock and wall
// clock, scripted input, a thread factory that fails the k-th spawn or arms RFP1, file_access over
// sl::fabric::testing's faulty vfs (over a vfs_memory, root "/"), a headless window and a
// recording sink whose log serializes to canonical bytes, so two runs can be compared bit for bit.
// Owner-thread (`thread_safety`, testing::*) except manual_clock::now, which any thread may call.
// Include it by path: "../../../engine/runtime/testing/scripted_platform.hpp".

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/headless_platform.hpp"
#include "sl/runtime/platform.hpp"

#include "../../fabric/testing/faulty_vfs.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sl::runtime::testing {

/// A clock that moves only when told; set and advance may go backward. now() is any-thread.
class manual_clock final : public clock_source {
public:
  explicit manual_clock(nanos start = nanos{0}) noexcept : ns_(start.count()) {}

  [[nodiscard]] nanos now() const noexcept override { return nanos{ns_.load()}; }
  void set(nanos t) noexcept { ns_.store(t.count()); }
  void advance(nanos d) noexcept { ns_.fetch_add(d.count()); }

private:
  std::atomic<nanos::rep> ns_;
};

/// A wall clock that moves only when told (nanoseconds since the system_clock epoch).
class manual_wall_clock final : public wall_clock {
public:
  [[nodiscard]] std::chrono::system_clock::time_point now() const noexcept override {
    return std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(nanos{ns_.load()})};
  }
  void set(nanos since_epoch) noexcept { ns_.store(since_epoch.count()); }

private:
  std::atomic<nanos::rep> ns_{0};
};

/// Input events pushed by the test; poll returns them in push order.
class scripted_input final : public input_source {
public:
  void push(const input_event &event);
  [[nodiscard]] std::size_t poll(std::span<input_event> out) noexcept override;
  [[nodiscard]] std::size_t pending() const noexcept { return queue_.size(); }

private:
  std::deque<input_event> queue_;
};

/// Spawns real threads through os_thread_factory unless told to fail: fail_at(k) makes the k-th
/// spawn from now (1-based) return thread_spawn_failed without starting a thread; arm_rfp1(k)
/// arms RFP1 to fire at its k-th visit from now, which the os factory turns into
/// thread_spawn_failed.
class failing_thread_factory final : public thread_factory {
public:
  explicit failing_thread_factory(std::uint32_t instance_id) noexcept : inner_(instance_id) {}

  [[nodiscard]] result<thread_handle> spawn(std::string name, std::function<void()> body,
                                            affinity_hint hint = {}) noexcept override;
  /// 0 disarms.
  void fail_at(std::uint64_t k) noexcept;
  /// testing::arm(rfp1, k, sticky); unsupported when the failpoints are compiled out.
  [[nodiscard]] static result<void> arm_rfp1(std::uint64_t k, bool sticky = false);
  /// Spawn calls so far, failed ones included.
  [[nodiscard]] std::uint64_t spawns() const noexcept { return spawns_; }

private:
  os_thread_factory inner_;
  std::uint64_t spawns_ = 0;
  std::uint64_t fail_at_ = 0; // absolute spawn number; 0 = never
};

enum class record_kind : std::uint8_t { wall_sample = 1, input = 2, file_read = 3, external = 4 };

/// One record_sink call as recording_sink saw it.
struct record {
  record_kind kind = record_kind::wall_sample;
  std::uint64_t frame = 0;
  std::uint64_t tick = 0;      // 0 for a wall sample
  std::uint32_t code = 0;      // the input type or the external kind
  std::int64_t sample = 0;     // the wall sample in nanoseconds
  std::string path;            // the file read
  std::vector<std::byte> data; // input payload[0, size), the file digest, the external bytes

  friend bool operator==(const record &, const record &) = default;
};

/// Keeps every call in order; serialize() is the canonical little-endian byte form of the log.
class recording_sink final : public record_sink {
public:
  void on_wall_sample(std::uint64_t frame, nanos sample) noexcept override;
  void on_input(std::uint64_t frame, std::uint64_t tick,
                const input_event &event) noexcept override;
  void on_file_read(std::uint64_t frame, std::uint64_t tick, std::string_view path,
                    const sl::fabric::digest &content) noexcept override;
  void on_external(std::uint64_t frame, std::uint64_t tick, std::uint32_t kind,
                   std::span<const std::byte> bytes) noexcept override;

  [[nodiscard]] const std::vector<record> &records() const noexcept { return records_; }
  [[nodiscard]] std::vector<std::byte> serialize() const;
  void clear() noexcept { records_.clear(); }

private:
  std::vector<record> records_;
};

/// Owns every scripted member; get() is valid for its lifetime. Not copyable or movable.
class scripted_platform {
public:
  explicit scripted_platform(std::uint32_t instance_id = 1);
  scripted_platform(const scripted_platform &) = delete;
  scripted_platform(scripted_platform &&) = delete;
  scripted_platform &operator=(const scripted_platform &) = delete;
  scripted_platform &operator=(scripted_platform &&) = delete;
  ~scripted_platform() = default;

  [[nodiscard]] platform &get() noexcept { return platform_; }
  [[nodiscard]] manual_clock &clock() noexcept { return clock_; }
  [[nodiscard]] manual_wall_clock &wall() noexcept { return wall_; }
  [[nodiscard]] scripted_input &input() noexcept { return input_; }
  [[nodiscard]] failing_thread_factory &threads() noexcept { return threads_; }
  [[nodiscard]] sl::fabric::testing::faulty_vfs &vfs() noexcept { return vfs_; }
  [[nodiscard]] file_access &files() noexcept { return files_; }
  [[nodiscard]] recording_sink &sink() noexcept { return sink_; }

private:
  manual_clock clock_;
  manual_wall_clock wall_;
  failing_thread_factory threads_;
  scripted_input input_;
  sl::fabric::testing::faulty_vfs vfs_;
  file_access files_;
  headless_window win_;
  recording_sink sink_;
  platform platform_;
};

} // namespace sl::runtime::testing
