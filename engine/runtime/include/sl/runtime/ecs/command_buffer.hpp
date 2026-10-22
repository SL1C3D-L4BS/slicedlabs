#pragma once

// ECS command buffers (WO-091, SL-ARCH-003 ARCH3-R118 to R120, ADR-0025, ADR-0026; episodes 091
// and 092). A job opens one command_buffer from a command_queue under its ordered_key base
// (local_seq 0) and records spawn, spawn_with, destroy, add, remove and set; each command gets
// local_seq = its index in the buffer. Commands and their payloads (each component value
// move-constructed into place) live in 16384-byte blocks (`limits` command_buffer_block_bytes) from
// the job's frame arena, mem::frame_arena::for_worker(worker); every piece (a command header, an id
// or pointer array, one value of at most 2048 bytes) fits one block. A recording failure
// (out_of_memory from the arena, component_unknown for an unregistered type) is sticky: the buffer
// records nothing more and flush stops there with that error, after the commands of smaller keys.
// flush plays every buffer back on the owner thread at a phase boundary through det::merge_ordered
// by (base key, local_seq), never in completion order, with world::set_phase_active(false) for the
// duration, through world's public structural API only (create_raw, destroy, add_raw, remove_raw,
// set_raw). A pending_entity resolves when its spawn plays, in key order; a command naming a dead
// entity, or a pending entity not yet resolved, is skipped and counted (skipped_stale);
// component_exists and component_missing are skipped and counted (skipped_refused); any other world
// error (out_of_memory, entity_capacity, archetype_limit, structural_change_forbidden) is returned
// as is with the commands before it applied. Every flush and clear destroys every payload and
// empties the queue. Sources: src/ecs_command_buffer.cpp (recording), src/ecs_command_queue.cpp
// (open, flush, clear). Layer L6: includes L0 to L2 runtime headers and ecs/world.hpp. Thread
// safety (`thread_safety`): command_buffer recording is worker-only, one writer per buffer;
// command_queue::open is any-thread (a mutex); flush and clear are owner-thread (the thread that
// built the queue; flush returns wrong_thread elsewhere, clear is a Debug assertion and does
// nothing). Lifetime (`ownership`): the blocks are frame memory, so a queue must be flushed or
// cleared before its frame arena flips twice after the recording.

#include "sl/core/error.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/memory.hpp"
#include "sl/runtime/replay.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace sl::runtime::ecs {

/// The bytes of one command block (`limits` command_buffer_block_bytes, from the frame arena).
inline constexpr std::size_t command_buffer_block_bytes = 16384;
/// The alignment of a command block (the strictest component alignment).
inline constexpr std::size_t command_buffer_block_alignment = 64;

/// An entity a command buffer will spawn: the buffer's open index in its queue and the spawn
/// command's local_seq. Valid until the queue's next flush or clear.
struct pending_entity {
  std::uint32_t buffer = 0;
  std::uint32_t seq = 0;

  friend constexpr auto operator<=>(const pending_entity &,
                                    const pending_entity &) noexcept = default;
};

/// The target of a command: a real entity or one a command buffer spawns.
using entity_ref = std::variant<entity, pending_entity>;

/// What one flush did, in playback (ordered_key) order.
struct flush_report {
  enum class op_kind : std::uint8_t { spawn, destroy, add, remove, set };
  struct op {
    op_kind kind = op_kind::spawn;
    entity target;
    /// The component of add, remove and set; component_id{} for spawn and destroy.
    component_id component;
    /// set only: the entity lacked the component, so the set added it (a component_added
    /// signal, ARCH3-R129); the kind stays set.
    bool added = false;

    friend constexpr bool operator==(const op &, const op &) noexcept = default;
  };

  std::vector<op> applied;
  /// Each spawn that played: its pending handle and the entity it became, in key order.
  std::vector<std::pair<pending_entity, entity>> resolved;
  /// Commands naming a dead entity or an unresolved pending entity.
  std::uint32_t skipped_stale = 0;
  /// add of a component the entity has (component_exists), remove of one it lacks
  /// (component_missing).
  std::uint32_t skipped_refused = 0;
};

class command_queue;

namespace detail {

/// One recorded command in frame memory (trivially destructible; never destroyed). values[i] is
/// the payload of component ids[i]; add and set point both arrays at their own members.
struct command {
  command *next = nullptr;
  entity_ref target;
  entity resolved;
  const component_id *ids = nullptr;
  void *const *values = nullptr;
  component_id component;
  void *value = nullptr;
  std::uint32_t buffer = 0;
  std::uint32_t seq = 0;
  std::uint16_t count = 0;
  flush_report::op_kind kind = flush_report::op_kind::spawn;
  /// errc::ok for a command; the sticky recording error for a buffer's failure marker.
  errc fault = errc::ok;
};
static_assert(std::is_trivially_destructible_v<command>);

/// True when no type of Ts repeats.
template <class... Ts> struct distinct_types : std::true_type {};
template <class T, class... Rest>
struct distinct_types<T, Rest...>
    : std::bool_constant<(!std::is_same_v<T, Rest> && ...) && distinct_types<Rest...>::value> {};

} // namespace detail

/// One job's commands. Worker-only while recording (a single writer); opened by
/// command_queue::open, played back and emptied by command_queue::flush. Neither copyable nor
/// movable.
class command_buffer {
public:
  using op_kind = flush_report::op_kind;

  command_buffer(const command_buffer &) = delete;
  command_buffer(command_buffer &&) = delete;
  command_buffer &operator=(const command_buffer &) = delete;
  command_buffer &operator=(command_buffer &&) = delete;
  ~command_buffer() = default;

  /// Spawns an entity with no component at playback.
  pending_entity spawn() noexcept;
  /// Spawns an entity with the given components (distinct types) at playback.
  template <class... Cs> pending_entity spawn_with(Cs &&...values) noexcept {
    constexpr std::size_t n = sizeof...(Cs);
    static_assert(n <= max_components_per_archetype);
    static_assert((component_type<std::remove_cvref_t<Cs>> && ...));
    static_assert((std::is_nothrow_constructible_v<std::remove_cvref_t<Cs>, Cs &&> && ...));
    static_assert(detail::distinct_types<std::remove_cvref_t<Cs>...>::value,
                  "spawn_with: a component type is given twice");
    if constexpr (n == 0) {
      return spawn();
    } else {
      const pending_entity handle{.buffer = id_, .seq = size_};
      const std::array<result<component_id>, n> found{
          registry_->id_of<std::remove_cvref_t<Cs>>()...};
      const std::array<std::size_t, n> sizes{sizeof(std::remove_cvref_t<Cs>)...};
      const std::array<std::size_t, n> aligns{alignof(std::remove_cvref_t<Cs>)...};
      detail::command *c = start(op_kind::spawn, entity_ref{});
      if (c == nullptr || !ids_known(found)) {
        return handle;
      }
      auto *ids =
          static_cast<component_id *>(take(n * sizeof(component_id), alignof(component_id)));
      auto *slots = static_cast<void **>(take(n * sizeof(void *), alignof(void *)));
      if (ids == nullptr || slots == nullptr) {
        return handle;
      }
      for (std::size_t i = 0; i < n; ++i) {
        ids[i] = found[i].value_or(component_id{});
        slots[i] = take(sizes[i], aligns[i]);
        if (slots[i] == nullptr) {
          return handle;
        }
      }
      construct_each(slots, std::index_sequence_for<Cs...>{}, std::forward<Cs>(values)...);
      c->ids = ids;
      c->values = slots;
      c->count = static_cast<std::uint16_t>(n);
      append(c);
      return handle;
    }
  }
  /// Destroys the entity at playback.
  void destroy(const entity_ref &target) noexcept;
  /// Adds component T at playback (component_exists there is skipped and counted).
  template <class T> void add(const entity_ref &target, T &&value) noexcept {
    put(op_kind::add, target, std::forward<T>(value));
  }
  /// Removes component T at playback (component_missing there is skipped and counted).
  template <class T> void remove(const entity_ref &target) noexcept {
    const auto cid = registry_->id_of<std::remove_cvref_t<T>>();
    if (!cid) {
      fail(cid.error().code);
      return;
    }
    remove_id(target, *cid);
  }
  /// Replaces component T (adds it when missing) at playback.
  template <class T> void set(const entity_ref &target, T &&value) noexcept {
    put(op_kind::set, target, std::forward<T>(value));
  }

  /// The commands recorded (a refused command is not counted).
  [[nodiscard]] std::uint32_t size() const noexcept { return size_; }
  /// The sticky recording error, or ok.
  [[nodiscard]] errc status() const noexcept { return fault_.fault; }
  /// The ordered_key base the buffer was opened with.
  [[nodiscard]] const det::ordered_key &key() const noexcept { return base_; }
  /// The open index in the queue (pending_entity::buffer).
  [[nodiscard]] std::uint32_t id() const noexcept { return id_; }

private:
  friend class command_queue;
  struct passkey {};

public:
  /// Built by command_queue only.
  command_buffer(passkey /*unused*/, const component_registry &registry) noexcept
      : registry_(&registry) {}

private:
  template <class T> void put(op_kind kind, const entity_ref &target, T &&value) noexcept {
    using U = std::remove_cvref_t<T>;
    static_assert(component_type<U>);
    static_assert(std::is_nothrow_constructible_v<U, T &&>);
    const auto cid = registry_->id_of<U>();
    if (!cid) {
      fail(cid.error().code);
      return;
    }
    detail::command *c = start(kind, target);
    if (c == nullptr) {
      return;
    }
    void *slot = take(sizeof(U), alignof(U));
    if (slot == nullptr) {
      return;
    }
    ::new (slot) U(std::forward<T>(value));
    c->component = *cid;
    c->value = slot;
    c->ids = &c->component;
    c->values = &c->value;
    c->count = 1;
    append(c);
  }
  template <class... Cs, std::size_t... I>
  static void construct_each(void *const *slots, std::index_sequence<I...> /*unused*/,
                             Cs &&...values) noexcept {
    (::new (slots[I]) std::remove_cvref_t<Cs>(std::forward<Cs>(values)), ...);
  }
  template <std::size_t N>
  [[nodiscard]] bool ids_known(const std::array<result<component_id>, N> &found) noexcept {
    const auto bad = std::ranges::find_if(
        found, [](const result<component_id> &f) noexcept { return !f.has_value(); });
    if (bad != found.end()) {
      fail(bad->error().code);
      return false;
    }
    return true;
  }

  /// Takes `bytes` aligned to `align` from the current block or a new one; nullptr once the
  /// buffer failed (a failed block allocation makes it fail).
  [[nodiscard]] void *take(std::size_t bytes, std::size_t align) noexcept;
  /// A new command header (not yet appended); nullptr once the buffer failed.
  [[nodiscard]] detail::command *start(op_kind kind, const entity_ref &target) noexcept;
  void append(detail::command *c) noexcept;
  void remove_id(const entity_ref &target, component_id component) noexcept;
  /// Makes the buffer fail with `code` (the first failure wins).
  void fail(errc code) noexcept;
  /// Starts a fresh recording (open).
  void reset(const det::ordered_key &base, std::uint32_t index, mem::resource &memory) noexcept;
  /// Fills index_ with (base + local_seq, command) and the failure marker (flush).
  void build_index() noexcept;
  /// Destroys every payload and forgets every command (flush, clear).
  void release() noexcept;

  const component_registry *registry_ = nullptr;
  mem::resource *memory_ = nullptr;
  det::ordered_key base_;
  std::uint32_t id_ = 0;
  std::byte *block_ = nullptr;
  std::size_t used_ = 0;
  detail::command *first_ = nullptr;
  detail::command *last_ = nullptr;
  std::uint32_t size_ = 0;
  detail::command fault_;
  det::ordered_buffer<detail::command *> index_;
};

/// The command buffers of one world between two flushes. Neither copyable nor movable.
class command_queue {
public:
  /// A queue for worlds of `frozen` (registry must be frozen and outlive the queue) whose blocks
  /// come from `arena`. The calling thread becomes the owner thread.
  command_queue(const component_registry &frozen, mem::frame_arena &arena) noexcept;
  command_queue(const command_queue &) = delete;
  command_queue(command_queue &&) = delete;
  command_queue &operator=(const command_queue &) = delete;
  command_queue &operator=(command_queue &&) = delete;
  ~command_queue();

  /// The buffer of the job keyed `base` (one per job; base.local_seq must be 0, otherwise the
  /// buffer fails with invalid_argument) recording into the owner thread's frame arena. Any thread.
  [[nodiscard]] command_buffer &open(const det::ordered_key &base) noexcept;
  /// As open(base), recording into worker `worker`'s frame arena (job_context::worker()).
  [[nodiscard]] command_buffer &open(const det::ordered_key &base, std::uint32_t worker) noexcept;
  /// Owner thread, phase boundary (no job recording): plays back every buffer by (base key,
  /// local_seq) and empties the queue. wrong_thread off the owner thread, invalid_argument for a
  /// world of another registry or two buffers of one base key (nothing applied); otherwise a
  /// recording error or a world error other than stale_entity, component_exists and
  /// component_missing is returned as is, with the commands before it applied.
  [[nodiscard]] result<flush_report> flush(world &w) noexcept;
  /// Owner thread: discards every recorded command unplayed (a faulted frame) and empties the
  /// queue.
  void clear() noexcept;
  /// The buffers opened since the last flush or clear.
  [[nodiscard]] std::uint32_t open_count() const noexcept;

private:
  friend struct command_player;

  /// The entity `target` names now: a real handle, or the resolved spawn of a pending one; false
  /// for a pending entity that is unknown or not yet resolved.
  [[nodiscard]] bool resolve(const entity_ref &target, entity &out) const noexcept;
  /// Releases the first `buffers` buffers and resets the open count.
  void release(std::uint32_t buffers) noexcept;
  /// True when two of the first `buffers` buffers share a base key.
  [[nodiscard]] bool duplicate_keys(std::uint32_t buffers) noexcept;

  const component_registry *registry_;
  mem::frame_arena *arena_;
  mutable std::mutex mutex_;
  std::vector<std::unique_ptr<command_buffer>> pool_; // guarded by mutex_ while jobs record
  std::uint32_t open_ = 0;                            // guarded by mutex_
  std::vector<det::ordered_buffer<detail::command *> *> merge_;
  std::vector<det::ordered_key> keys_;
  std::thread::id owner_;
};

} // namespace sl::runtime::ecs
