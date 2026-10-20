#pragma once

// The platform boundary (WO-078, SL-ARCH-003 ARCH3-R035 to R040, episode 082, ADR-0006). The
// runtime reaches the OS only through `platform`: clock_source (monotonic nanoseconds), wall_clock
// (log stamps only), thread_factory (named, move-only thread handles), input_source, file_access
// (over sl::fabric::vfs, so the faulty vfs reaches runtime I/O), window (headless only in Volume
// 05) and record_sink (every external input, stamped with the consuming frame and tick). Only
// src/platform_*.cpp calls a clock's now(), creates a thread or touches a file (runtime-lint
// R-RT-PLATFORM); vfs.hpp is included only by src/platform_files.cpp, so this header forward
// declares sl::fabric::vfs. Layer L1.
//
// Thread safety (SL-ARCH-003 `thread_safety`): clock_source::now and wall_clock::now any thread;
// thread_factory::spawn and thread_group on the owner thread; input_source::poll and window on the
// owner thread; file_access any thread, one call per path at a time; record_sink on the owner
// thread (begin_frame). Every fallible call returns sl::result with runtime codes
// (thread_spawn_failed, platform_unavailable) or core io_error, not_found, invalid_argument.

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sl::fabric {
class vfs;
} // namespace sl::fabric

namespace sl::runtime {

/// The runtime's duration type (time.hpp reuses it).
using nanos = std::chrono::nanoseconds;

/// The common base of the platform interfaces: not copyable, not movable, virtual destructor.
class platform_interface {
public:
  platform_interface() = default;
  platform_interface(const platform_interface &) = delete;
  platform_interface(platform_interface &&) = delete;
  platform_interface &operator=(const platform_interface &) = delete;
  platform_interface &operator=(platform_interface &&) = delete;
  virtual ~platform_interface() = default;
};

/// A monotonic clock: now() never goes backward on one thread or across threads (ARCH3-R079).
/// Any thread.
class clock_source : public platform_interface {
public:
  [[nodiscard]] virtual nanos now() const noexcept = 0;
};

/// The calendar clock, for log stamps only; never enters the simulation. Any thread.
class wall_clock : public platform_interface {
public:
  [[nodiscard]] virtual std::chrono::system_clock::time_point now() const noexcept = 0;
};

class os_thread_factory;

/// One OS thread: move-only RAII; the destructor (and a move assignment onto a running handle)
/// joins it. A default-constructed or moved-from handle is not joinable and has an empty name.
class thread_handle {
public:
  thread_handle() noexcept;
  thread_handle(const thread_handle &) = delete;
  thread_handle &operator=(const thread_handle &) = delete;
  thread_handle(thread_handle &&other) noexcept;
  thread_handle &operator=(thread_handle &&other) noexcept;
  ~thread_handle();

  /// True until join() returned.
  [[nodiscard]] bool joinable() const noexcept;
  /// The full name, slrt<instance>-<suffix> (the OS name is its first 15 bytes).
  [[nodiscard]] std::string_view name() const noexcept;
  /// Waits for the thread to finish; a no-op when not joinable.
  void join() noexcept;

private:
  friend class os_thread_factory;
  struct state;
  explicit thread_handle(std::unique_ptr<state> s) noexcept;
  std::unique_ptr<state> state_;
};

/// A core to prefer; accepted and ignored (ARCH3-R040).
struct affinity_hint {
  std::int32_t core = -1;
};

/// Creates named OS threads (ARCH3-R037, R040). Owner thread. A spawn visits RFP1 first and
/// returns thread_spawn_failed when it fires or the OS refuses, with no thread started.
class thread_factory : public platform_interface {
public:
  /// Starts `body` on a new thread named slrt<instance>-<name> (callers pass w<k>); the OS name is
  /// truncated to 15 bytes. invalid_argument for an empty name or body.
  [[nodiscard]] virtual result<thread_handle> spawn(std::string name, std::function<void()> body,
                                                    affinity_hint hint = {}) noexcept = 0;
};

/// Handles acquired in order and released in reverse acquisition order (ARCH3-R037). Move-only;
/// the destructor releases. Owner thread.
class thread_group {
public:
  thread_group() noexcept = default;
  thread_group(const thread_group &) = delete;
  thread_group &operator=(const thread_group &) = delete;
  thread_group(thread_group &&other) noexcept = default;
  /// Releases this group's threads first.
  thread_group &operator=(thread_group &&other) noexcept;
  ~thread_group();

  /// Spawns through `factory` and keeps the handle; on an error nothing is added.
  [[nodiscard]] result<void> spawn(thread_factory &factory, std::string name,
                                   std::function<void()> body, affinity_hint hint = {}) noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return handles_.size(); }
  /// Joins every handle, the newest first; returns their names in release order.
  std::vector<std::string> release() noexcept;

private:
  std::vector<thread_handle> handles_;
};

/// The payload capacity of one input event.
inline constexpr std::size_t input_payload_bytes = 48;

/// One external input event: a type, `size` meaningful payload bytes.
struct input_event {
  std::uint32_t type = 0;
  std::uint8_t size = 0;
  std::array<std::byte, input_payload_bytes> payload{};

  friend bool operator==(const input_event &, const input_event &) noexcept = default;
};

/// External input (devices in later volumes; none in Volume 05). Owner thread.
class input_source : public platform_interface {
public:
  /// Moves up to out.size() pending events into `out`, in arrival order; returns the count.
  [[nodiscard]] virtual std::size_t poll(std::span<input_event> out) noexcept = 0;
};

/// Files through a sl::fabric::vfs (ARCH3-R038). Relative paths resolve against root(); a path
/// that starts with '/' is used as given. vfs errors map to not_found (a missing file or
/// directory) and io_error (every other failure). Any thread, one call per path at a time.
/// Move-only; the vfs it borrows must outlive it.
class file_access {
public:
  /// Over `fs` (borrowed), resolving relative paths against `root` ("" means as given).
  explicit file_access(sl::fabric::vfs &fs, std::string root = {}) noexcept;
  /// Over an owned POSIX vfs; creates `root` and its missing parents first (io_error otherwise).
  [[nodiscard]] static result<file_access> make_posix(std::string root) noexcept;
  file_access(const file_access &) = delete;
  file_access &operator=(const file_access &) = delete;
  file_access(file_access &&other) noexcept;
  file_access &operator=(file_access &&other) noexcept;
  ~file_access();

  /// The whole file.
  [[nodiscard]] result<std::vector<std::byte>> read(std::string_view path) const noexcept;
  /// Replaces the file: writes <path>.tmp, fsyncs it, renames it over <path> and fsyncs the
  /// directory; after a crash at any primitive the file holds the old or the new bytes. The
  /// directory must exist.
  [[nodiscard]] result<void> atomic_write(std::string_view path,
                                          std::span<const std::byte> bytes) const noexcept;
  /// Removes a file; not_found when absent.
  [[nodiscard]] result<void> remove(std::string_view path) const noexcept;
  [[nodiscard]] result<bool> exists(std::string_view path) const noexcept;
  [[nodiscard]] const std::string &root() const noexcept { return root_; }

private:
  [[nodiscard]] std::string resolve(std::string_view path) const;

  std::unique_ptr<sl::fabric::vfs> owned_;
  sl::fabric::vfs *fs_ = nullptr;
  std::string root_;
};

/// The presentation surface. Volume 05 has only the headless window (ADR-0006). Owner thread.
class window : public platform_interface {
public:
  [[nodiscard]] virtual bool headless() const noexcept = 0;
  [[nodiscard]] virtual std::uint32_t width() const noexcept = 0;
  [[nodiscard]] virtual std::uint32_t height() const noexcept = 0;
};

/// Receives every value that enters the simulation from outside, exactly once, in arrival order,
/// stamped with the frame and tick that consume it (ARCH3-R039; the Volume 17 recording seam). The
/// caller (the frame scheduler, WO-089) passes frame and tick. Every member defaults to a no-op.
/// Owner thread (begin_frame).
class record_sink : public platform_interface {
public:
  virtual void on_wall_sample(std::uint64_t frame, nanos sample) noexcept;
  virtual void on_input(std::uint64_t frame, std::uint64_t tick, const input_event &event) noexcept;
  virtual void on_file_read(std::uint64_t frame, std::uint64_t tick, std::string_view path,
                            const sl::fabric::digest &content) noexcept;
  virtual void on_external(std::uint64_t frame, std::uint64_t tick, std::uint32_t kind,
                           std::span<const std::byte> bytes) noexcept;
};

/// The sink that records nothing.
class null_record_sink final : public record_sink {};

// NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members): a view of objects the host owns
/// The platform a runtime uses; the host owns every member and keeps them alive longer than the
/// runtime (`ownership`, platform).
struct platform {
  clock_source &clock;
  wall_clock &wall;
  thread_factory &threads;
  input_source &input;
  file_access &files;
  window &win;
  record_sink &sink;
  std::uint32_t instance_id;
};
// NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)

/// The digest record_file_read passes: SHA-256 of the bytes.
[[nodiscard]] sl::fabric::digest content_digest(std::span<const std::byte> bytes) noexcept;

/// Samples `clock` once, passes the sample to `sink` stamped with `frame` and returns it.
nanos record_wall_sample(const clock_source &clock, record_sink &sink,
                         std::uint64_t frame) noexcept;

/// Polls `input` once into `buffer` and passes each event to `sink` in arrival order, stamped with
/// `frame` and `tick`; returns the number of events.
std::size_t record_input(input_source &input, record_sink &sink, std::uint64_t frame,
                         std::uint64_t tick, std::span<input_event> buffer) noexcept;

/// Reads `path` through `files` and passes its content digest to `sink` stamped with `frame` and
/// `tick`; a failed read passes nothing and returns the error.
[[nodiscard]] result<std::vector<std::byte>>
record_file_read(const file_access &files, record_sink &sink, std::uint64_t frame,
                 std::uint64_t tick, std::string_view path) noexcept;

} // namespace sl::runtime
