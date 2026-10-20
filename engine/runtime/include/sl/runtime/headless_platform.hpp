#pragma once

// The Linux headless platform (WO-078, SL-ARCH-003 ARCH3-R036, ADR-0006): the steady clock, the
// system wall clock, pthread workers, no input device, files over the POSIX vfs under
// platform_options::root_dir and a headless window (width and height 0). No display, window or
// input device is opened. Construction is owner-thread (`thread_safety`). Layer L1.

#include "sl/core/error.hpp"
#include "sl/runtime/platform.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace sl::runtime {

struct platform_options {
  std::string root_dir = "build/runtime/work";
  std::uint32_t instance_id = 1;
};

/// std::chrono::steady_clock (src/platform_clock.cpp).
class steady_clock_source final : public clock_source {
public:
  [[nodiscard]] nanos now() const noexcept override;
};

/// std::chrono::system_clock, for log stamps (src/platform_clock.cpp).
class system_wall_clock final : public wall_clock {
public:
  [[nodiscard]] std::chrono::system_clock::time_point now() const noexcept override;
};

/// pthread threads named slrt<instance>-<name> with pthread_setname_np (src/platform_threads.cpp).
class os_thread_factory final : public thread_factory {
public:
  explicit os_thread_factory(std::uint32_t instance_id) noexcept : instance_id_(instance_id) {}

  [[nodiscard]] result<thread_handle> spawn(std::string name, std::function<void()> body,
                                            affinity_hint hint = {}) noexcept override;
  [[nodiscard]] std::uint32_t instance_id() const noexcept { return instance_id_; }

private:
  std::uint32_t instance_id_;
};

/// No input device: poll returns 0 events.
class null_input_source final : public input_source {
public:
  [[nodiscard]] std::size_t poll(std::span<input_event> /*out*/) noexcept override { return 0; }
};

/// The headless window: headless, width and height 0.
class headless_window final : public window {
public:
  [[nodiscard]] bool headless() const noexcept override { return true; }
  [[nodiscard]] std::uint32_t width() const noexcept override { return 0; }
  [[nodiscard]] std::uint32_t height() const noexcept override { return 0; }
};

/// Owns every platform member; get() is valid for its lifetime. Members are destroyed in reverse
/// declaration order. Not copyable or movable (the platform view refers to the members).
class headless_platform {
public:
  headless_platform(const platform_options &options, file_access files) noexcept;
  headless_platform(const headless_platform &) = delete;
  headless_platform(headless_platform &&) = delete;
  headless_platform &operator=(const headless_platform &) = delete;
  headless_platform &operator=(headless_platform &&) = delete;
  ~headless_platform() = default;

  [[nodiscard]] platform &get() noexcept { return platform_; }

private:
  steady_clock_source clock_;
  system_wall_clock wall_;
  os_thread_factory threads_;
  null_input_source input_;
  file_access files_;
  headless_window win_;
  null_record_sink sink_;
  platform platform_;
};

/// Creates root_dir (and its parents) through the POSIX vfs and the platform over it;
/// platform_unavailable when root_dir cannot be created.
[[nodiscard]] result<std::unique_ptr<headless_platform>>
make_headless_platform(const platform_options &options) noexcept;

} // namespace sl::runtime
