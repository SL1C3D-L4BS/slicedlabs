#pragma once

// Typed runtime events (WO-092, SL-ARCH-003 ARCH3-R137 to R142, R086, R091, episode 097,
// ADR-0026, ADR-0027). An event type is a reflected struct registered in the event_registry; it is
// written and read only through event_channel<E>, held as a world resource under
// event_registry::channel_key. Every delivered event carries an event_envelope {type, seq from 1
// per channel per frame, emitting system_index, tick, cause, correlation}; the correlation is
// diagnostic only: never hashed, captured or used for order. Writers append to per-writer
// ordered_buffers keyed (tick, phase, system_index, chunk_index, local_seq); at each phase end the
// buffers merge with det::merge_ordered into the frame's readable list, so delivery order is
// (phase, system_index, chunk_index, seq) whatever the worker count. An event is readable from
// that boundary until the end of the following frame (two frame lists; the older is dropped when
// the frame closes). A channel holds at most event_capacity_default events per frame: the newest
// beyond it are dropped and counted (one runtime.events warning per frame), try_write returns
// event_overflow, and RFP7 makes the channel behave as full. Debug builds reject payloads with a
// non-finite f32 or f64 field (event_payload_invalid). events_contributor (runtime.events, version
// 1) snapshots both frame lists without correlation; event_exporter turns a frame's events into
// fabric.event envelopes on the runtime's own bus and source (ARCH3-R142). Sources:
// src/event_registry.cpp, event_channel.cpp, event_export.cpp, event_contributor.cpp. Layer L7.
//
// Thread safety (`thread_safety`): event_registry add and freeze are owner-thread (the
// constructing thread; wrong_thread otherwise), before freeze; find, id_of, name and payload_type
// are any-thread after freeze. event_channel<E> write, try_write and read are worker-only (systems
// that declare the event access; also the owner thread outside phases); write and try_write may
// run concurrently from many jobs, one job per writer_key. deliver, end_frame, attach, the
// contributor and the exporter are owner-thread, at phase boundaries and end_frame, with no job of
// the frame in flight. dropped() is any-thread.

#include "sl/core/error.hpp"
#include "sl/core/log.hpp"
#include "sl/fabric/events.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/replay.hpp"
#include "sl/runtime/time.hpp"

#include <atomic>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace sl::runtime {

/// `limits` event_capacity_default: events per channel per frame (RFP7).
inline constexpr std::uint32_t event_capacity_default = 65536;
/// channel_key(id) is {event_channel_key_base + id}: a resource key range kept for event channels.
inline constexpr std::uint32_t event_channel_key_base = 0x4556'0000U;

/// True in Debug builds (NDEBUG unset): write and try_write validate payloads (ARCH3-R140).
#if defined(NDEBUG)
inline constexpr bool event_validation = false;
#else
inline constexpr bool event_validation = true;
#endif

/// The id of a registered event type: 1.. in registration order; 0 is none.
struct event_type_id {
  std::uint32_t value = 0;

  friend constexpr auto operator<=>(const event_type_id &,
                                    const event_type_id &) noexcept = default;
  friend constexpr bool operator==(const event_type_id &, const event_type_id &) noexcept = default;
};

/// A delivered event: the frame of its list, its type and its seq in that frame.
struct event_ref {
  std::uint64_t frame = 0;
  event_type_id type;
  std::uint32_t seq = 0;

  friend constexpr bool operator==(const event_ref &, const event_ref &) noexcept = default;
};

/// The envelope of a delivered event (ARCH3-R138). `correlation` is diagnostic only: never hashed,
/// captured or used for order; its scope view must outlive the frame after the write (the
/// runtime's runtime/<instance> scope does).
struct event_envelope {
  event_type_id type;
  std::uint32_t seq = 0;
  std::uint16_t system_index = 0;
  time::tick tick = 0;
  std::optional<event_ref> cause;
  sl::log::correlation correlation;
};

/// Who writes: the ordered_key of a writer without its local_seq, which the channel assigns per
/// writer (0.. between two boundaries). One job per key at a time.
struct writer_key {
  time::tick tick = 0;
  phase ph = phase::tick;
  std::uint16_t system_index = 0;
  std::uint32_t chunk_index = 0;
};

/// The registry of event types: each a reflected, trivially copyable, standard-layout struct
/// with a stable event name (ARCH3-R137). Neither copyable nor movable (channels point into it).
class event_registry {
public:
  /// The constructing thread is the owner thread of add and freeze.
  event_registry() noexcept;
  event_registry(const event_registry &) = delete;
  event_registry(event_registry &&) = delete;
  event_registry &operator=(const event_registry &) = delete;
  event_registry &operator=(event_registry &&) = delete;
  ~event_registry() = default;

  /// Registers E under the event name `name` with its reflected description `info` (copied into
  /// the registry's own type_registry): wrong_thread off the owner thread, type_registry_frozen
  /// after freeze, invalid_argument for an empty name or one with a byte outside printable ASCII
  /// (0x21..0x7E), or an info whose size or alignment is not E's, already_exists for a repeated
  /// event name or type E; the type_registry's own refusals otherwise.
  template <class E>
  [[nodiscard]] result<event_type_id> add(std::string_view event_name,
                                          const type_info &info) noexcept {
    static_assert(std::is_trivially_copyable_v<E> && std::is_standard_layout_v<E>,
                  "an event type is a trivially copyable, standard-layout reflected struct");
    static_assert(std::is_nothrow_default_constructible_v<E>,
                  "an event type must be nothrow default constructible");
    return add_raw(event_name, info, ecs::detail::type_key_of<E>(), sizeof(E), alignof(E));
  }

  /// event_type_unknown for an unregistered event name.
  [[nodiscard]] result<event_type_id> find(std::string_view event_name) const noexcept;
  /// The id of E; event_type_unknown when E is not registered.
  template <class E> [[nodiscard]] result<event_type_id> id_of() const noexcept {
    return find_key(ecs::detail::type_key_of<E>());
  }
  /// The id registered with `type_key`; event_type_unknown otherwise.
  [[nodiscard]] result<event_type_id> find_key(const void *type_key) const noexcept;
  /// Freezes the registry and its type_registry (owner thread; wrong_thread otherwise). Freezing
  /// twice succeeds.
  [[nodiscard]] result<void> freeze() noexcept;
  [[nodiscard]] bool frozen() const noexcept { return frozen_; }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

  /// The event name of `id`; empty when `id` is not registered.
  [[nodiscard]] std::string_view name(event_type_id id) const noexcept;
  /// The reflected payload type of `id` (stable for the registry's lifetime); nullptr when `id`
  /// is not registered.
  [[nodiscard]] const type_info *payload_type(event_type_id id) const noexcept;

  /// The world resource key of the channel of `id`: {event_channel_key_base + id.value}.
  [[nodiscard]] static constexpr ecs::resource_key channel_key(event_type_id id) noexcept {
    return ecs::resource_key{event_channel_key_base + id.value};
  }

private:
  struct entry {
    std::string name;
    const type_info *info = nullptr;
    const void *type_key = nullptr;
  };

  [[nodiscard]] result<event_type_id> add_raw(std::string_view event_name, const type_info &info,
                                              const void *type_key, std::size_t bytes,
                                              std::size_t alignment) noexcept;

  type_registry types_;
  std::vector<entry> entries_; // index = id - 1
  std::map<std::string, std::uint32_t, std::less<>> by_name_;
  std::thread::id owner_;
  bool frozen_ = false;
};

/// One exported event: its envelope and the reflected bytes of its payload (every flagged field).
struct event_record {
  event_envelope envelope;
  std::vector<std::byte> payload;
};

/// The type-erased side of a channel, for the frame hooks, the contributor and the exporter.
/// Owner-thread except dropped().
class event_stream {
public:
  virtual ~event_stream() = default;

  [[nodiscard]] event_type_id type() const noexcept { return type_; }
  [[nodiscard]] std::string_view name() const noexcept { return name_; }
  [[nodiscard]] const type_info &payload_type() const noexcept { return *info_; }
  /// Events dropped at capacity or by RFP7 since the channel was made (any thread).
  [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_.load(); }

  /// Phase boundary: merges every writer buffer by ordered_key into frame `frame`'s list.
  virtual void deliver(std::uint64_t frame) noexcept = 0;
  /// Frame end: logs the frame's drops once, drops the older list, starts the next frame's list.
  virtual void end_frame() noexcept = 0;
  /// Appends the events delivered in frame `frame` (either list), in delivery order.
  virtual void collect(std::uint64_t frame, std::vector<event_record> &out) const = 0;
  /// Appends both frame lists without correlation (events_contributor).
  virtual void capture(std::vector<std::byte> &out) const = 0;
  /// Parses one capture at `pos` into a staging area (false for bytes that do not parse).
  [[nodiscard]] virtual bool stage(std::span<const std::byte> in, std::size_t &pos) = 0;
  /// Replaces both lists with the staged ones and clears the writer buffers.
  virtual void commit_staged() noexcept = 0;
  virtual void discard_staged() noexcept = 0;
  /// Hashes both lists without correlation: envelope fields and the hashed payload fields.
  virtual void hash(det::state_hasher &h) const = 0;

protected:
  event_stream(event_type_id id, std::string_view name, const type_info &info) noexcept;
  event_stream(const event_stream &) = delete;
  event_stream(event_stream &&) = delete;
  event_stream &operator=(const event_stream &) = delete;
  event_stream &operator=(event_stream &&) = delete;

  /// True when a write must be refused: RFP7 fired, or `delivered` events this frame plus the
  /// writer's `pending` ones reach the capacity. A refusal is counted as a drop.
  [[nodiscard]] bool refuse(std::size_t delivered, std::size_t pending) noexcept;
  /// Counts one event dropped at the boundary merge.
  void count_drop() noexcept;
  /// Logs this frame's drops once (system runtime.events) and resets the frame count.
  void close_frame(std::uint64_t frame) noexcept;

private:
  event_type_id type_;
  std::string name_;
  const type_info *info_;
  std::atomic<std::uint64_t> dropped_{0};
  std::atomic<std::uint64_t> frame_dropped_{0};
};

namespace detail {

/// True unless a f32 or f64 field of `obj` (described by `t`) is NaN or infinite.
[[nodiscard]] bool payload_valid(const type_info &t, const void *obj) noexcept;

/// The fields a capture keeps (snapshot or hashed) and every flagged field (export).
inline constexpr field_flags capture_fields = field_flags::snapshot | field_flags::hashed;
inline constexpr field_flags export_fields =
    field_flags::transient | field_flags::snapshot | field_flags::hashed;

/// The capture codec of event_contributor.cpp (little-endian, fixed width).
void put_u32(std::vector<std::byte> &out, std::uint32_t v) noexcept;
void put_u64(std::vector<std::byte> &out, std::uint64_t v) noexcept;
[[nodiscard]] bool get_u32(std::span<const std::byte> in, std::size_t &pos,
                           std::uint32_t &v) noexcept;
[[nodiscard]] bool get_u64(std::span<const std::byte> in, std::size_t &pos,
                           std::uint64_t &v) noexcept;
/// type u32, seq u32, system_index u16, tick u64, cause flag u8 (+ frame u64, type u32, seq u32);
/// no correlation.
void put_envelope(const event_envelope &e, std::vector<std::byte> &out) noexcept;
/// The inverse; the correlation is left at its default.
[[nodiscard]] bool get_envelope(std::span<const std::byte> in, std::size_t &pos,
                                event_envelope &e) noexcept;
/// The envelope fields of put_envelope through state_hasher; no correlation.
void hash_envelope(const event_envelope &e, det::state_hasher &h) noexcept;

/// The state of one event_channel<E>. Owned through a unique_ptr so the frame hooks and the
/// contributor keep a stable address when the channel moves into the world.
template <class E> class channel_core final : public event_stream {
public:
  struct pending {
    E payload{};
    std::optional<event_ref> cause;
    sl::log::correlation correlation;
  };
  struct delivered {
    event_envelope envelope;
    E payload{};
  };
  using buffer = det::ordered_buffer<pending>;

  channel_core(event_type_id id, std::string_view name, const type_info &info) noexcept
      : event_stream(id, name, info) {}

  [[nodiscard]] result<void> push(const writer_key &w, E &&ev, std::optional<event_ref> cause,
                                  bool report) noexcept {
    const sl::log::correlation corr = sl::log::current_correlation();
    const std::scoped_lock lock(mutex_);
    buffer &b = writers_[slot{.ph = static_cast<std::uint8_t>(w.ph),
                              .system_index = w.system_index,
                              .chunk_index = w.chunk_index}];
    if (refuse(current_.size(), b.size())) {
      if (report) {
        return make_error(errc::event_overflow, "event_channel: the channel is full this frame");
      }
      return {};
    }
    const det::ordered_key key{.tick = w.tick,
                               .phase = static_cast<std::uint8_t>(w.ph),
                               .system_index = w.system_index,
                               .chunk_index = w.chunk_index,
                               .local_seq = static_cast<std::uint32_t>(b.size())};
    b.push(key, pending{.payload = std::move(ev), .cause = cause, .correlation = corr});
    return {};
  }

  template <class F> void read(F &fn) const {
    for (const delivered &d : previous_) {
      std::invoke(fn, std::as_const(d.envelope), std::as_const(d.payload));
    }
    for (const delivered &d : current_) {
      std::invoke(fn, std::as_const(d.envelope), std::as_const(d.payload));
    }
  }

  template <class F> void read_refs(F &fn) const {
    for (const delivered &d : previous_) {
      const event_ref ref{.frame = previous_frame_, .type = type(), .seq = d.envelope.seq};
      std::invoke(fn, ref, std::as_const(d.envelope), std::as_const(d.payload));
    }
    for (const delivered &d : current_) {
      const event_ref ref{.frame = current_frame_, .type = type(), .seq = d.envelope.seq};
      std::invoke(fn, ref, std::as_const(d.envelope), std::as_const(d.payload));
    }
  }

  void deliver(std::uint64_t frame) noexcept override {
    const std::scoped_lock lock(mutex_);
    current_frame_ = frame;
    merge_list_.clear();
    for (auto &[key, b] : writers_) {
      if (!b.empty()) {
        merge_list_.push_back(&b);
      }
    }
    if (merge_list_.empty()) {
      return;
    }
    det::merge_ordered(std::span<buffer *const>(merge_list_), appender{.self = this});
    for (buffer *b : merge_list_) {
      b->clear();
    }
  }

  void end_frame() noexcept override {
    const std::scoped_lock lock(mutex_);
    close_frame(current_frame_);
    previous_.swap(current_);
    previous_frame_ = current_frame_;
    current_.clear();
    current_frame_ = previous_frame_ + 1;
  }

  void collect(std::uint64_t frame, std::vector<event_record> &out) const override {
    const std::scoped_lock lock(mutex_);
    const std::vector<delivered> *list = nullptr;
    if (current_frame_ == frame) {
      list = &current_;
    } else if (previous_frame_ == frame) {
      list = &previous_;
    } else {
      return;
    }
    for (const delivered &d : *list) {
      event_record r{.envelope = d.envelope, .payload = {}};
      encode(payload_type(), std::addressof(d.payload), export_fields, r.payload);
      out.push_back(std::move(r));
    }
  }

  void capture(std::vector<std::byte> &out) const override {
    const std::scoped_lock lock(mutex_);
    put_u32(out, type().value);
    capture_list(previous_frame_, previous_, out);
    capture_list(current_frame_, current_, out);
  }

  [[nodiscard]] bool stage(std::span<const std::byte> in, std::size_t &pos) override {
    std::uint32_t id = 0;
    if (!get_u32(in, pos, id) || id != type().value) {
      return false;
    }
    staged_previous_.clear();
    staged_current_.clear();
    return stage_list(in, pos, staged_previous_frame_, staged_previous_) &&
           stage_list(in, pos, staged_current_frame_, staged_current_);
  }

  void commit_staged() noexcept override {
    const std::scoped_lock lock(mutex_);
    previous_.swap(staged_previous_);
    current_.swap(staged_current_);
    previous_frame_ = staged_previous_frame_;
    current_frame_ = staged_current_frame_;
    for (auto &[key, b] : writers_) {
      b.clear();
    }
    discard_staged();
  }

  void discard_staged() noexcept override {
    staged_previous_.clear();
    staged_current_.clear();
  }

  void hash(det::state_hasher &h) const override {
    const std::scoped_lock lock(mutex_);
    h.u32(type().value);
    hash_list(previous_, h);
    hash_list(current_, h);
  }

private:
  struct slot {
    std::uint8_t ph = 0;
    std::uint16_t system_index = 0;
    std::uint32_t chunk_index = 0;

    friend constexpr auto operator<=>(const slot &, const slot &) noexcept = default;
  };

  struct appender {
    channel_core *self = nullptr;
    void operator()(const det::ordered_key &key, pending &p) const noexcept {
      self->append(key, p);
    }
  };

  void append(const det::ordered_key &key, pending &p) noexcept {
    if (current_.size() >= event_capacity_default) {
      count_drop();
      return;
    }
    current_.push_back(
        delivered{.envelope = event_envelope{.type = type(),
                                             .seq = static_cast<std::uint32_t>(current_.size() + 1),
                                             .system_index = key.system_index,
                                             .tick = key.tick,
                                             .cause = p.cause,
                                             .correlation = p.correlation},
                  .payload = std::move(p.payload)});
  }

  void capture_list(std::uint64_t frame, const std::vector<delivered> &list,
                    std::vector<std::byte> &out) const {
    put_u64(out, frame);
    put_u32(out, static_cast<std::uint32_t>(list.size()));
    const std::size_t payload_bytes = encoded_size(payload_type(), capture_fields);
    for (const delivered &d : list) {
      put_envelope(d.envelope, out);
      put_u32(out, static_cast<std::uint32_t>(payload_bytes));
      encode(payload_type(), std::addressof(d.payload), capture_fields, out);
    }
  }

  [[nodiscard]] bool stage_list(std::span<const std::byte> in, std::size_t &pos,
                                std::uint64_t &frame, std::vector<delivered> &list) const {
    std::uint32_t count = 0;
    if (!get_u64(in, pos, frame) || !get_u32(in, pos, count) || count > event_capacity_default) {
      return false;
    }
    list.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      delivered d{};
      std::uint32_t length = 0;
      if (!get_envelope(in, pos, d.envelope) || d.envelope.type != type() ||
          d.envelope.seq != i + 1 || !get_u32(in, pos, length) || in.size() - pos < length) {
        return false;
      }
      if (!decode(payload_type(), in.subspan(pos, length), capture_fields,
                  std::addressof(d.payload))) {
        return false;
      }
      pos += length;
      list.push_back(d);
    }
    return true;
  }

  void hash_list(const std::vector<delivered> &list, det::state_hasher &h) const {
    h.u32(static_cast<std::uint32_t>(list.size()));
    for (const delivered &d : list) {
      hash_envelope(d.envelope, h);
      hash_into(payload_type(), std::addressof(d.payload), field_flags::hashed, h);
    }
  }

  mutable std::mutex mutex_;
  std::map<slot, buffer> writers_; // nodes are stable; cleared, never erased
  std::vector<buffer *> merge_list_;
  std::vector<delivered> current_;
  std::vector<delivered> previous_;
  std::uint64_t current_frame_ = 0;
  std::uint64_t previous_frame_ = 0;
  std::vector<delivered> staged_current_;
  std::vector<delivered> staged_previous_;
  std::uint64_t staged_current_frame_ = 0;
  std::uint64_t staged_previous_frame_ = 0;
};

} // namespace detail

/// The channel of event type E (ARCH3-R137 to R141): a world resource under channel_key.
/// Move-only; a moved-from channel must not be used.
template <class E> class event_channel {
public:
  /// A channel for E: invalid_argument when `frozen` is not frozen, event_type_unknown when E is
  /// not registered. The registry must outlive the channel.
  [[nodiscard]] static result<event_channel> make(const event_registry &frozen) noexcept {
    if (!frozen.frozen()) {
      return make_error(errc::invalid_argument, "event_channel: the registry is not frozen");
    }
    const auto id = frozen.id_of<E>();
    if (!id) {
      return std::unexpected(id.error());
    }
    return event_channel(std::make_unique<detail::channel_core<E>>(*id, frozen.name(*id),
                                                                   *frozen.payload_type(*id)));
  }

  event_channel(const event_channel &) = delete;
  event_channel &operator=(const event_channel &) = delete;
  event_channel(event_channel &&) noexcept = default;
  event_channel &operator=(event_channel &&) noexcept = default;
  ~event_channel() = default;

  /// Appends `ev` to the writer's buffer; it becomes readable at the next phase boundary. At
  /// capacity (or under RFP7) the event is dropped and counted and write returns ok; a payload
  /// with a non-finite float is event_payload_invalid in Debug (nothing is written).
  [[nodiscard]] result<void> write(const writer_key &w, E &&ev,
                                   std::optional<event_ref> cause = {}) noexcept {
    return put(w, std::move(ev), cause, false);
  }
  /// As write, but a drop returns event_overflow.
  [[nodiscard]] result<void> try_write(const writer_key &w, E &&ev,
                                       std::optional<event_ref> cause = {}) noexcept {
    return put(w, std::move(ev), cause, true);
  }
  /// fn(const event_envelope &, const E &) for every readable event: the previous frame's list,
  /// then the current frame's, each in delivery order.
  template <class F> void read(F &&fn) const { core_->read(fn); }
  /// As read, with the event_ref of each event first: fn(const event_ref &, envelope, payload).
  template <class F> void read_refs(F &&fn) const { core_->read_refs(fn); }
  [[nodiscard]] std::uint64_t dropped() const noexcept { return core_->dropped(); }
  [[nodiscard]] event_type_id type() const noexcept { return core_->type(); }
  /// The type-erased side (stable while the channel lives, also across moves).
  [[nodiscard]] event_stream &stream() noexcept { return *core_; }
  [[nodiscard]] const event_stream &stream() const noexcept { return *core_; }

private:
  explicit event_channel(std::unique_ptr<detail::channel_core<E>> core) noexcept
      : core_(std::move(core)) {}

  [[nodiscard]] result<void> put(const writer_key &w, E &&ev, std::optional<event_ref> cause,
                                 bool report) noexcept {
    if constexpr (event_validation) {
      if (!detail::payload_valid(core_->payload_type(), std::addressof(ev))) {
        return make_error(errc::event_payload_invalid,
                          "event_channel: a float field is NaN or infinite");
      }
    }
    return core_->push(w, std::move(ev), cause, report);
  }

  std::unique_ptr<detail::channel_core<E>> core_;
};

/// Makes the channel of E and adds it to `w` under channel_key: invalid_argument and
/// event_type_unknown as event_channel::make, the world's add_resource errors otherwise.
template <class E>
[[nodiscard]] result<event_channel<E> *> add_event_channel(ecs::world &w,
                                                           const event_registry &frozen) noexcept {
  auto made = event_channel<E>::make(frozen);
  if (!made) {
    return std::unexpected(made.error());
  }
  const ecs::resource_key key = event_registry::channel_key(made->type());
  if (auto added = w.add_resource(key, std::move(*made)); !added) {
    return std::unexpected(added.error());
  }
  return w.resource<event_channel<E>>(key);
}

/// The channel of E in `w`: event_type_unknown when E is not registered, the world's not_found
/// when no channel was added.
template <class E>
[[nodiscard]] result<event_channel<E> *> find_event_channel(ecs::world &w,
                                                            const event_registry &frozen) noexcept {
  const auto id = frozen.id_of<E>();
  if (!id) {
    return std::unexpected(id.error());
  }
  return w.resource<event_channel<E>>(event_registry::channel_key(*id));
}

/// Registers the channel's boundary steps with `fs` (owner thread, before run_frame): deliver at
/// every phase end hook with the frame's index, end_frame in the arena_flip boundary slot. The
/// stream must outlive the scheduler's frames.
void attach(event_stream &stream, frame_scheduler &fs);

/// runtime.events, version 1 (ARCH3-R091, `determinism` pending events): snapshots both frame
/// lists of every channel without correlation and hashes them without correlation. The bytes are
/// a u32le channel count, then per channel (ascending type id) its type id and both lists
/// (previous, current), each a u64le frame, a u32le count and per event the envelope fields and
/// a u32le-length payload of its snapshot and hashed fields. Restore is all or nothing
/// (snapshot_corrupt). The channels must outlive it. Owner-thread, end_frame.
class events_contributor final : public det::snapshot_contributor {
public:
  explicit events_contributor(std::span<event_stream *const> channels);

  [[nodiscard]] std::string_view name() const noexcept override { return "runtime.events"; }
  [[nodiscard]] std::uint32_t version() const noexcept override { return 1; }
  [[nodiscard]] result<void> capture(std::vector<std::byte> &out) const override;
  [[nodiscard]] result<void> restore(std::span<const std::byte> in) override;
  void hash(det::state_hasher &h) const override;

private:
  std::vector<event_stream *> channels_; // ascending type id
};

/// Export to fabric.event (ARCH3-R142): the runtime's own bus fed by its one source. Owner-thread
/// (the constructing thread), at end_frame. The bus and the source must outlive the exporter.
class event_exporter {
public:
  event_exporter(sl::fabric::event_bus &bus, sl::fabric::event_source &source) noexcept;

  /// Publishes every event delivered in frame `frame` by `channels` (ascending type id, then
  /// delivery order) as one batch of fabric.event envelopes: type "runtime.event.<name>", the
  /// reflected payload bytes as an SLB/1 bytes value, the event's correlation {id, scope, tick}.
  /// wrong_thread off the owner thread; internal when the source or the bus refuses an envelope
  /// (nothing is published then).
  [[nodiscard]] result<void> export_frame(std::uint64_t frame,
                                          std::span<const event_stream *const> channels) noexcept;
  /// Envelopes published so far.
  [[nodiscard]] std::uint64_t exported() const noexcept { return exported_; }

private:
  sl::fabric::event_bus *bus_;
  sl::fabric::event_source *source_;
  std::thread::id owner_;
  std::uint64_t exported_ = 0;
};

} // namespace sl::runtime
