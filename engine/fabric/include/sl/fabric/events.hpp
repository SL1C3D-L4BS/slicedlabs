#pragma once

// Fabric event envelopes and the in-process bus (WO-047, SL-ARCH-001 ARCH1-R46 to ARCH1-R49;
// master plan section 3, F6).
//
// The envelope is the generated fabric.event.event_envelope
// (schemas/fabric/fabric.event.sltype.json, WO-045): id, payload schema ref, type string, time_us
// from the injected clock, per-source seq, actor, exactly one of subject id or scope tag (a
// variant, so "exactly one" is by construction; an empty scope or a null subject is refused),
// correlation {id, scope, tick} with an owned scope string, causality {cause, tx, revision} and an
// optional SLB/1 payload.
//
// Event ids derive from (parent, kind, seq): derive(event, tx id, kind + ":" + seq) for an event
// caused by a transaction, with the event source's own id as parent when there is none. Neither
// time nor correlation enters the id, so replay reproduces it (ARCH1-R47).
//
// event_bus is an in-process, ordered bus. publish() validates an envelope, then delivers it to
// every subscriber whose prefix matches its type, in seq order: a seq at or below the last
// accepted one is refused with `event_invalid`, publish_batch() sorts a batch by seq first, and a
// publish from inside a handler is queued and delivered after the current event. Handlers are
// `noexcept` by type, so nothing can throw across the bus boundary (ADR-0009); a handler failure
// is the handler's own business. A JSON Lines sink (one schema-valid object per line,
// schemas/sl-fabric-event.schema.json) is enabled by the environment variable
// SL_FABRIC_EVENT_JSON_PATH, and a bridge forwards every event to an sl::log logger as a
// schema-valid sl-log-event.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/core/log.hpp"
#include "sl/fabric/clock.hpp"
#include "sl/fabric/gen/fabric.event.gen.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

/// The generated envelope and its parts (fabric.event, version 1.0).
using event_envelope = gen::fabric_event::event_envelope;
using event_schema_ref = gen::fabric_event::schema_ref;
using event_about = gen::fabric_event::about;
using event_causality = gen::fabric_event::causality;
/// {id, scope, tick} with an owned scope string (ARCH1-R48).
using correlation_record = gen::fabric_event::correlation;

/// The environment variable that names the JSON Lines dump file.
inline constexpr std::string_view event_json_env = "SL_FABRIC_EVENT_JSON_PATH";
/// The value of the `format` member of every dumped line.
inline constexpr std::string_view event_json_format = "sl-fabric-event/1";

// --- correlation (ARCH1-R48) ---

/// The record's fields as an sl::log::correlation, field for field. The scope view points into
/// `c` and is valid while `c` lives and is not modified.
[[nodiscard]] sl::log::correlation to_log(const correlation_record &c) noexcept;
/// A record with the fields of `c`; the scope is copied, so the record owns it.
[[nodiscard]] correlation_record from_log(const sl::log::correlation &c);
/// from_log(sl::log::current_correlation()): the calling thread's innermost correlation_scope,
/// else {0, process scope, 0}.
[[nodiscard]] correlation_record current_correlation_record();

// --- ids (ARCH1-R47) ---

/// derive(event, parent, kind + ":" + seq). Pure: no time, no correlation.
[[nodiscard]] record_id event_id_for(const record_id &parent, std::string_view kind,
                                     std::uint64_t seq);

// --- validation (ARCH1-R46) ---

/// `event_invalid` (the message names the member) unless the envelope is complete: an event-class
/// id, a non-empty type string of printable ASCII, a type-class payload schema id, seq >= 1, an
/// actor-class actor, a non-null subject or a non-empty scope tag, a non-empty correlation scope,
/// and cause / tx ids of the event / tx class when present.
[[nodiscard]] result<void> validate(const event_envelope &e);

// --- sources ---

/// What a caller says about an event; the source adds id, seq and time.
struct event_spec {
  std::string type;
  event_schema_ref schema;
  record_id actor;
  event_about about;
  /// Empty: current_correlation_record().
  std::optional<correlation_record> correlation;
  event_causality causality;
  /// Encoded as SLB/1 into the envelope payload.
  std::optional<value> payload;
};

/// Stamps envelopes for one source: a per-source seq starting at 1, time from the injected clock
/// and ids derived from the transaction in the causality, else from `source` (an id of the
/// source's own, such as a well-known entity). Not synchronised: one thread drives a source.
class event_source {
public:
  /// The clock must outlive the source.
  event_source(record_id source, const clock &time) noexcept : source_(source), clock_(&time) {}

  /// The next envelope, validated (`event_invalid`), or the error of encoding the payload. The seq
  /// is spent only when an envelope is returned.
  [[nodiscard]] result<event_envelope> make(event_spec spec);

  /// The seq the next envelope gets.
  [[nodiscard]] std::uint64_t next_seq() const noexcept { return next_seq_; }
  [[nodiscard]] const record_id &source() const noexcept { return source_; }

private:
  record_id source_;
  const clock *clock_;
  std::uint64_t next_seq_ = 1;
};

// --- JSON Lines and the log bridge (ARCH1-R49) ---

/// One line (no newline) of the dump: the object schemas/sl-fabric-event.schema.json describes.
/// `event_invalid` when `e` is not valid; digests that have no text form fail as digest_text does.
[[nodiscard]] result<std::string> to_json_line(const event_envelope &e);

/// Logs `e` to `target`: system "fabric.event", message = type, fields event_id / seq / subject or
/// scope / tx, the correlation of the envelope (to_log) and the envelope's time. The resulting
/// event validates against schemas/sl-log-event.schema.json.
void log_event(sl::log::logger &target, const event_envelope &e,
               sl::log::level lvl = sl::log::level::info);

// --- the bus ---

/// Called for each delivered event; must not throw (noexcept by type).
using event_handler = std::move_only_function<void(const event_envelope &) noexcept>;

namespace detail {
struct bus_core;
} // namespace detail

/// RAII handle of one subscription; destroying or reset()ing it unsubscribes (also while the bus
/// is delivering; the handler may still be running on another thread at that moment). It may
/// outlive the bus.
class subscription {
public:
  subscription() noexcept = default;
  subscription(const subscription &) = delete;
  subscription &operator=(const subscription &) = delete;
  subscription(subscription &&other) noexcept;
  subscription &operator=(subscription &&other) noexcept;
  ~subscription();

  /// Unsubscribes; harmless when already inactive.
  void reset() noexcept;
  /// True until reset() or the bus is destroyed.
  [[nodiscard]] bool active() const noexcept;

private:
  friend class event_bus;
  subscription(std::weak_ptr<detail::bus_core> bus, std::uint64_t token) noexcept
      : bus_(std::move(bus)), token_(token) {}

  std::weak_ptr<detail::bus_core> bus_;
  std::uint64_t token_ = 0;
};

class event_bus {
public:
  event_bus();
  ~event_bus();
  event_bus(const event_bus &) = delete;
  event_bus &operator=(const event_bus &) = delete;
  event_bus(event_bus &&) = delete;
  event_bus &operator=(event_bus &&) = delete;

  /// Calls `handler` for every event whose type string starts with `type_prefix` ("" matches all),
  /// after the call returns. Subscribers are called in subscription order.
  [[nodiscard]] subscription subscribe(std::string_view type_prefix, event_handler handler);

  /// validate(), then queue and deliver. `event_invalid` for an invalid envelope or a seq at or
  /// below the last accepted seq; nothing is delivered then.
  [[nodiscard]] result<void> publish(event_envelope e);
  /// All or nothing: every envelope valid and the batch strictly after the last accepted seq with
  /// no duplicate; delivered in seq order whatever the order of `batch`.
  [[nodiscard]] result<void> publish_batch(std::vector<event_envelope> batch);

  /// The highest seq accepted so far (0 when none).
  [[nodiscard]] std::uint64_t last_seq() const noexcept;
  /// Events delivered so far (once each, not per subscriber).
  [[nodiscard]] std::uint64_t delivered() const noexcept;
  [[nodiscard]] std::size_t subscriber_count() const noexcept;

  /// Appends one line per delivered event to `dump_path`. `io_error` when it cannot be opened.
  [[nodiscard]] result<void> enable_json_lines(const std::string &dump_path);
  /// enable_json_lines(getenv(SL_FABRIC_EVENT_JSON_PATH)); false when the variable is unset or
  /// empty.
  [[nodiscard]] result<bool> enable_json_from_env();
  void disable_json_lines() noexcept;
  /// Lines that could not be written (the dump never fails a publish).
  [[nodiscard]] std::uint64_t json_failures() const noexcept;

  /// Forwards every delivered event to `target` with log_event(); nullptr stops. The logger must
  /// outlive the bus or be cleared first.
  void forward_to_log(sl::log::logger *target) noexcept;

private:
  std::shared_ptr<detail::bus_core> core_;
};

} // namespace sl::fabric
