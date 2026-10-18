#pragma once

// The injected clock (WO-037, ADR-0019, SL-ARCH-002 ARCH2-R11). Fabric code never reads the wall
// clock directly: time comes from a `clock` so replay can feed recorded values. Times are
// microseconds since the Unix epoch (commit_time_us, event time); they never enter a hashed body
// (ADR-0012).

#include <cstdint>

namespace sl::fabric {

class clock {
public:
  virtual ~clock() = default;
  /// Microseconds since 1970-01-01T00:00:00Z.
  [[nodiscard]] virtual std::int64_t now_us() const noexcept = 0;

protected:
  clock() = default;
  clock(const clock &) = default;
  clock &operator=(const clock &) = default;
  clock(clock &&) = default;
  clock &operator=(clock &&) = default;
};

/// std::chrono::system_clock.
class system_clock_source final : public clock {
public:
  [[nodiscard]] std::int64_t now_us() const noexcept override;
};

/// A clock that moves only when told to (tests and replay). Not synchronised: one thread drives it.
class manual_clock final : public clock {
public:
  explicit manual_clock(std::int64_t start_us = 0) noexcept : now_(start_us) {}

  [[nodiscard]] std::int64_t now_us() const noexcept override { return now_; }
  void set_us(std::int64_t t) noexcept { now_ = t; }
  void advance_us(std::int64_t dt) noexcept { now_ += dt; }

private:
  std::int64_t now_;
};

} // namespace sl::fabric
