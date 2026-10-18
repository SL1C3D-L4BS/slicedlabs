#pragma once

// Structured logging (WO-030, SL-EP-056, master plan 5.1 "Logging and assertions" and 3 F6).
//
// An event carries a level, a system name, a message, typed fields and correlation fields (id,
// scope tag, tick). Every event carries a correlation: inside a correlation_scope it is that
// scope's; outside, the process-wide scope tag (default "process") with id 0 and tick 0 (F6:
// process-wide events carry a scope tag). Sinks: stderr_sink (human text), json_sink (one JSON
// object per line, schemas/sl-log-event.schema.json) and memory_sink (a ring of the last N events,
// for tests). A logger dispatches to its sinks under a mutex. Nothing on this path throws: each
// sink catches at its own boundary (formatting, allocation), drops the event and counts it in
// sink::failures() (ADR-0009, Consequences).

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace sl::log {

enum class level : std::uint8_t { trace, debug, info, warn, error, fatal };

/// "trace", "debug", "info", "warn", "error", "fatal".
[[nodiscard]] constexpr std::string_view to_string(level l) noexcept {
  switch (l) {
  case level::trace:
    return "trace";
  case level::debug:
    return "debug";
  case level::info:
    return "info";
  case level::warn:
    return "warn";
  case level::error:
    return "error";
  case level::fatal:
    return "fatal";
  }
  return "unknown";
}

/// A field value: integer, floating point, boolean or a string view (valid for the duration of the
/// call).
using field_value = std::variant<std::int64_t, double, bool, std::string_view>;

/// One typed key/value pair. Integral values are stored as int64 (an unsigned value above INT64_MAX
/// wraps), floating values as double; a C string is a string, never a bool.
struct field {
  std::string_view key;
  field_value value;

  constexpr field(std::string_view k, field_value v) noexcept : key(k), value(v) {}
  constexpr field(std::string_view k, bool v) noexcept : key(k), value(v) {}
  constexpr field(std::string_view k, std::string_view v) noexcept : key(k), value(v) {}
  constexpr field(std::string_view k, const char *v) noexcept
      : key(k), value(std::string_view{v}) {}
  field(std::string_view k, const std::string &v) noexcept : key(k), value(std::string_view{v}) {}
  template <std::integral T>
    requires(!std::same_as<T, bool>)
  constexpr field(std::string_view k, T v) noexcept : key(k), value(static_cast<std::int64_t>(v)) {}
  template <std::floating_point T>
  constexpr field(std::string_view k, T v) noexcept : key(k), value(static_cast<double>(v)) {}
};

/// Correlation fields (F6). id 0 means "no correlation id"; scope is an entity, job or subsystem
/// tag, or the process-wide tag; tick is the simulation tick (0 until a frame loop exists).
struct correlation {
  std::uint64_t id = 0;
  std::string_view scope = "process";
  std::uint64_t tick = 0;
};

/// One log event. Every view only has to live for the duration of logger::log().
struct event {
  log::level level = log::level::info;
  std::string_view system;
  std::string_view message;
  std::span<const field> fields;
  log::correlation correlation;
  std::source_location where;
  std::chrono::system_clock::time_point ts;
};

// --- correlation ---

/// The process-wide scope tag used when no correlation_scope is active. The view must have static
/// storage duration (a literal); the default is "process".
void set_process_scope(std::string_view scope) noexcept;
[[nodiscard]] std::string_view process_scope() noexcept;

/// A fresh, process-unique correlation id (starts at 1, never 0).
[[nodiscard]] std::uint64_t next_correlation_id() noexcept;

/// The calling thread's current correlation: the innermost live correlation_scope, else {0,
/// process_scope(), 0}.
[[nodiscard]] correlation current_correlation() noexcept;

/// RAII: makes a correlation current on this thread until destruction, then restores the previous
/// one. Scopes nest (LIFO); the scope view must outlive the object.
class correlation_scope {
public:
  explicit correlation_scope(correlation c) noexcept;
  /// A new id from next_correlation_id() with the given scope tag and tick.
  explicit correlation_scope(std::string_view scope, std::uint64_t tick = 0) noexcept;
  ~correlation_scope();
  correlation_scope(const correlation_scope &) = delete;
  correlation_scope &operator=(const correlation_scope &) = delete;
  correlation_scope(correlation_scope &&) = delete;
  correlation_scope &operator=(correlation_scope &&) = delete;

  [[nodiscard]] const correlation &get() const noexcept { return current_; }

private:
  correlation current_;
  const correlation_scope *previous_;
};

// --- sinks ---

class sink {
public:
  virtual ~sink() = default;
  sink(const sink &) = delete;
  sink &operator=(const sink &) = delete;
  sink(sink &&) = delete;
  sink &operator=(sink &&) = delete;

  /// Called by the logger under its mutex, one event at a time.
  virtual void write(const event &e) noexcept = 0;
  virtual void flush() noexcept {}

  /// Events this sink dropped because writing them failed (an exception from formatting or
  /// allocation, caught at the sink boundary); 0 in a healthy run.
  [[nodiscard]] std::uint64_t failures() const noexcept {
    return failures_.load(std::memory_order_relaxed);
  }

protected:
  sink() = default;
  /// Counts one dropped event; called from the catch block of write().
  void record_failure() noexcept { failures_.fetch_add(1, std::memory_order_relaxed); }

private:
  std::atomic<std::uint64_t> failures_{0};
};

/// Human-readable text, one line per event:
/// `2026-10-17T12:00:00.000Z INFO  [core] message key=1 name="x" {scope=process id=0 tick=0}
/// file:line`
class stderr_sink final : public sink {
public:
  explicit stderr_sink(std::FILE *out = stderr) noexcept : out_(out) {}
  void write(const event &e) noexcept override;
  void flush() noexcept override;
  [[nodiscard]] static std::string format(const event &e);

private:
  std::FILE *out_;
};

/// One JSON object per line (JSON Lines), flushed after every line; validates against
/// schemas/sl-log-event.schema.json. Strings are escaped and invalid UTF-8 becomes U+FFFD; a
/// non-finite double is written as null; a repeated field key keeps its first value. The FILE is
/// not owned.
class json_sink final : public sink {
public:
  explicit json_sink(std::FILE *out = stdout) noexcept : out_(out) {}
  void write(const event &e) noexcept override;
  void flush() noexcept override;
  [[nodiscard]] static std::string format(const event &e);

private:
  std::FILE *out_;
};

/// Appends `text` to `out` as a quoted, escaped JSON string (exposed for the assertion report and
/// tests).
void append_json_string(std::string &out, std::string_view text);

/// A ring of the last `capacity` events, deep-copied (for tests and crash context). Thread-safe.
class memory_sink final : public sink {
public:
  using owned_value = std::variant<std::int64_t, double, bool, std::string>;
  struct owned_field {
    std::string key;
    owned_value value;
  };
  struct record {
    log::level level = log::level::info;
    std::string system;
    std::string message;
    std::vector<owned_field> fields;
    std::uint64_t correlation_id = 0;
    std::string scope;
    std::uint64_t tick = 0;
    std::string file;
    std::uint_least32_t line = 0;
    std::string function;
    std::chrono::system_clock::time_point ts;

    /// The value of the first field named `key`, or nullptr.
    [[nodiscard]] const owned_value *find(std::string_view key) const noexcept;
  };

  explicit memory_sink(std::size_t capacity);
  void write(const event &e) noexcept override;

  /// The retained events, oldest first.
  [[nodiscard]] std::vector<record> records() const;
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] std::size_t size() const noexcept;
  /// Events ever written, including the ones the ring has overwritten.
  [[nodiscard]] std::uint64_t total() const noexcept;
  void clear() noexcept;

private:
  void append(const event &e);

  mutable std::mutex mutex_;
  std::size_t capacity_;
  std::vector<record> ring_;
  std::size_t next_ = 0;
  std::uint64_t total_ = 0;
};

// --- logger ---

class logger {
public:
  logger() = default;
  logger(const logger &) = delete;
  logger &operator=(const logger &) = delete;
  logger(logger &&) = delete;
  logger &operator=(logger &&) = delete;
  ~logger() = default;

  void add_sink(std::shared_ptr<sink> s);
  /// Removes `s`; returns false when it was not attached.
  bool remove_sink(const sink *s) noexcept;
  void clear_sinks() noexcept;
  [[nodiscard]] std::size_t sink_count() const noexcept;

  void set_min_level(level l) noexcept { min_level_.store(l, std::memory_order_relaxed); }
  [[nodiscard]] level min_level() const noexcept {
    return min_level_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] bool enabled(level l) const noexcept { return l >= min_level(); }

  /// Dispatches `e` to every sink when `e.level` is at or above the minimum level.
  void log(const event &e) noexcept;
  void flush() noexcept;

private:
  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<sink>> sinks_;
  std::atomic<level> min_level_{level::info};
};

/// The process-wide logger: a stderr_sink at level info until reconfigured.
[[nodiscard]] logger &default_logger() noexcept;

/// Builds an event (current correlation, current time) and logs it to `target`.
void emit(logger &target, level l, std::string_view system, std::string_view message,
          std::initializer_list<field> fields = {},
          std::source_location where = std::source_location::current()) noexcept;

/// Same, to default_logger().
void emit(level l, std::string_view system, std::string_view message,
          std::initializer_list<field> fields = {},
          std::source_location where = std::source_location::current()) noexcept;

} // namespace sl::log

// SL_LOG_INFO("render", "frame done", {"frame", n}, {"ms", 16.6}): one event to the default logger.
#define SL_LOG_AT(lvl, system, message, ...)                                                       \
  ::sl::log::emit((lvl), (system), (message), {__VA_ARGS__})
#define SL_LOG_TRACE(system, message, ...)                                                         \
  SL_LOG_AT(::sl::log::level::trace, system, message __VA_OPT__(, ) __VA_ARGS__)
#define SL_LOG_DEBUG(system, message, ...)                                                         \
  SL_LOG_AT(::sl::log::level::debug, system, message __VA_OPT__(, ) __VA_ARGS__)
#define SL_LOG_INFO(system, message, ...)                                                          \
  SL_LOG_AT(::sl::log::level::info, system, message __VA_OPT__(, ) __VA_ARGS__)
#define SL_LOG_WARN(system, message, ...)                                                          \
  SL_LOG_AT(::sl::log::level::warn, system, message __VA_OPT__(, ) __VA_ARGS__)
#define SL_LOG_ERROR(system, message, ...)                                                         \
  SL_LOG_AT(::sl::log::level::error, system, message __VA_OPT__(, ) __VA_ARGS__)
#define SL_LOG_FATAL(system, message, ...)                                                         \
  SL_LOG_AT(::sl::log::level::fatal, system, message __VA_OPT__(, ) __VA_ARGS__)
