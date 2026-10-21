#pragma once

// The Fabric link (WO-097, SL-ARCH-003 ARCH3-R148 to R161 and R132, ADR-0027; episodes 091, 095
// and 100): projection in, observations out. The runtime reads authoritative data only through a
// host-supplied sl::fabric::document_projection wrapped by document_source, whose load_plan lists
// the records grouped by document ascending (records of no document first, under the null id),
// then by record id ascending (ARCH3-R148, R152). A component type is document-bound (one
// fabric_binding in the binding_registry, one component per Fabric type) or transient (ARCH3-R151).
// world_link::ingest spawns one entity per bound record of the plan through a command buffer keyed
// {tick, end_frame, link system index, 0, 0} in plan order, flushes it, writes the component from
// the record body with write_source::document and records (record_id, entity) in the entity_map;
// records of an unbound type are skipped and counted (ARCH3-R153). attach registers:
//   - link.observe, an observe-phase system reading every bound component: the rows of each
//     observed component whose changed tick is later than the system's last run become
//     observed_change {record, path_of(field), value} for every bound field whose value differs
//     from the current projection's (echo suppression), pushed into one ordered_buffer per observed
//     component (chunk_index = its position); the outbox merges them by ordered_key, coalesces to
//     the last value per (record, path) and orders by (record id bytes, path) (ARCH3-R154). Only
//     entities of the entity_map are observed: spawn and despawn never are (ARCH3-R160).
//   - the observation_flush slot: skipped (the outbox dropped) when a system failed or the last
//     playback failed since the previous flush; otherwise one patch through
//     propose_from_observation with origin {actor, subsystem, epistemic observed} goes to the
//     proposal_sink, and an error from either is counted as rejected, never a frame error
//     (ARCH3-R155, link_observation_rejected).
//   - the projection_rebase slot: the pending projection (post_projection, any thread) is applied
//     as a diff against the current one keyed by (root_digest, generation): retired records
//     despawn, changed ones are rewritten in place with write_source::document (no changed tick,
//     ARCH3-R132), created ones spawn as in ingest, unchanged handles stay (ARCH3-R157); pending
//     becomes current, the old one is released (at most max_projections_held, ARCH3-R158) and the
//     summed body bytes of the held projections are reported as external tag fabric.projection.
// Debug builds assert at every slot that the host's workspace_alive token is lockable
// (ARCH3-R159). The host commits through the sink (ARCH3-R156); nothing here commits.
// Sources: src/bridge_source.cpp (document_source, binding_registry, null_sink, outbox,
// body_bytes), src/bridge_ingest.cpp (attach, ingest, spawning), src/bridge_observe.cpp (the
// observe system), src/bridge_flush.cpp (observation_flush), src/bridge_rebase.cpp
// (post_projection, projection_rebase). Layer L9: includes L0 to L7 runtime headers.
//
// Thread safety (`thread_safety`): post_projection is any-thread (a mutex guards the pending
// slot); attach, ingest, observe_component and stats are owner-thread (the thread that called
// attach; wrong_thread elsewhere), between frames or in the projection_rebase slot; entities() is
// read-only during phases. Ownership (`ownership`): the proposal_sink is the host's and outlives
// the world_link; the world_link and every document_source are destroyed before the runtime; the
// slots and the system hold a weak reference, so a destroyed link turns them into no-ops.

#include "sl/core/error.hpp"
#include "sl/fabric/boundary.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/value.hpp"
#include "sl/runtime/ecs/command_buffer.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/memdiag.hpp"
#include "sl/runtime/memory.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/replay.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace sl::runtime::link {

/// `limits` max_projections_held: the current and the pending projection.
inline constexpr std::uint32_t max_projections_held = 2;
/// The external memory tag of the held projections (memory.hpp runtime_external_tag_names).
inline constexpr std::string_view projection_tag_name = "fabric.projection";
/// The name of the observe system attach registers.
inline constexpr std::string_view observe_system_name = "link.observe";

/// Where observations go: the host commits them (ARCH3-R156). Owner thread, at end_frame.
class proposal_sink {
public:
  proposal_sink(const proposal_sink &) = delete;
  proposal_sink(proposal_sink &&) = delete;
  proposal_sink &operator=(const proposal_sink &) = delete;
  proposal_sink &operator=(proposal_sink &&) = delete;
  virtual ~proposal_sink();

  /// Takes one proposed patch made with `origin`; an error is counted as rejected by the link.
  [[nodiscard]] virtual result<void> submit(const sl::fabric::patch &p,
                                            const sl::fabric::tx_origin &origin) noexcept = 0;

protected:
  proposal_sink() noexcept = default;
};

/// The preview sink: counts what it receives, keeps the latest patch and never commits.
class null_sink final : public proposal_sink {
public:
  null_sink() noexcept = default;
  null_sink(const null_sink &) = delete;
  null_sink(null_sink &&) = delete;
  null_sink &operator=(const null_sink &) = delete;
  null_sink &operator=(null_sink &&) = delete;
  ~null_sink() override = default;

  [[nodiscard]] result<void> submit(const sl::fabric::patch &p,
                                    const sl::fabric::tx_origin &origin) noexcept override;
  [[nodiscard]] std::uint64_t submissions() const noexcept { return submissions_; }
  /// The ops of every patch received.
  [[nodiscard]] std::uint64_t changes() const noexcept { return changes_; }
  [[nodiscard]] const std::optional<sl::fabric::patch> &last() const noexcept { return last_; }
  [[nodiscard]] const std::optional<sl::fabric::tx_origin> &last_origin() const noexcept {
    return last_origin_;
  }

private:
  std::uint64_t submissions_ = 0;
  std::uint64_t changes_ = 0;
  std::optional<sl::fabric::patch> last_;
  std::optional<sl::fabric::tx_origin> last_origin_;
};

/// What the host supplies: the runtime actor (a service actor with write:<domain> grants), the
/// subsystem of its origin, the projection domains, the Debug lifetime token of the workspace and
/// the fabric.projection tag (reserved through tag_registry::reserve_external).
struct link_config {
  sl::fabric::actor_id actor;
  std::string subsystem = "runtime";
  std::vector<std::string> domains;
  std::weak_ptr<const void> workspace_alive;
  mem::memory_tag projection_tag;
};

/// One record of a load plan.
struct load_entry {
  sl::fabric::record_id document;
  sl::fabric::record_id record;
  sl::fabric::type_id type;

  friend bool operator==(const load_entry &, const load_entry &) noexcept = default;
};

/// The runtime's only window on authoritative data (ARCH3-R148): a host-supplied projection and its
/// load plan, computed once in make.
class document_source {
public:
  /// The projection's read errors.
  [[nodiscard]] static result<document_source>
  make(sl::fabric::document_projection projection) noexcept;

  /// Records grouped by document ascending (no document first), then record id ascending.
  // cppcheck-suppress returnByReference ; the brief's signature: the caller owns its copy
  [[nodiscard]] std::vector<load_entry> load_plan() const { return plan_; }
  [[nodiscard]] const sl::fabric::document_projection &projection() const noexcept {
    return projection_;
  }

private:
  document_source(sl::fabric::document_projection projection,
                  std::vector<load_entry> plan) noexcept;

  sl::fabric::document_projection projection_;
  std::vector<load_entry> plan_;
};

/// The document-bound components: one fabric_binding per component and per Fabric type.
class binding_registry {
public:
  struct entry {
    ecs::component_id component;
    fabric_binding binding;
  };

  /// already_exists for a component or a Fabric type bound twice; invalid_argument for a binding
  /// with no reflected type.
  [[nodiscard]] result<void> add(ecs::component_id component, fabric_binding binding) noexcept;
  /// The binding of Fabric type `type`, nullptr when it is transient here.
  [[nodiscard]] const fabric_binding *find(const sl::fabric::type_id &type) const noexcept;
  /// The entry of Fabric type `type`, nullptr when none.
  [[nodiscard]] const entry *entry_of(const sl::fabric::type_id &type) const noexcept;
  /// The entry of `component`, nullptr when it is transient.
  [[nodiscard]] const entry *find_component(ecs::component_id component) const noexcept;
  /// Ascending component id.
  [[nodiscard]] std::span<const entry> entries() const noexcept { return entries_; }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
  std::vector<entry> entries_;
};

/// Counters since attach (transient-diagnostic).
struct link_stats {
  std::uint64_t ingested = 0;
  std::uint64_t skipped_unbound = 0;
  std::uint64_t proposed_patches = 0;
  std::uint64_t proposed_changes = 0;
  std::uint64_t rejected = 0;
  std::uint64_t echo_suppressed = 0;
  std::uint64_t rebase_created = 0;
  std::uint64_t rebase_updated = 0;
  std::uint64_t rebase_retired = 0;
  std::uint64_t projection_bytes = 0;

  friend bool operator==(const link_stats &, const link_stats &) noexcept = default;
};

/// The observations of one frame (`determinism` outbox, a merge point): per-job buffers merged by
/// ordered_key, coalesced to the last value per (record, path), ordered by (record id bytes,
/// path). Buffers are added on the owner thread between frames; each has one writer job.
class outbox {
public:
  using buffer = det::ordered_buffer<sl::fabric::observed_change>;

  /// A new empty buffer; its address is stable until the outbox is destroyed.
  [[nodiscard]] buffer &add_buffer();
  /// Buffer `i` (i < buffers()).
  [[nodiscard]] buffer &buffer_at(std::size_t i) noexcept { return *buffers_[i]; }
  [[nodiscard]] std::size_t buffers() const noexcept { return buffers_.size(); }
  /// Changes pushed since the last drain or clear.
  [[nodiscard]] std::size_t pending() const noexcept;
  /// The merged, coalesced and ordered changes; empties every buffer.
  [[nodiscard]] std::vector<sl::fabric::observed_change> drain();
  /// Empties every buffer.
  void clear() noexcept;

private:
  std::vector<std::unique_ptr<buffer>> buffers_;
};

/// The body bytes of a value as the link reports them: the payload bytes of every scalar (bool
/// 1, integers 8, f32 4, f64 8, string and bytes their length, id 16, digest 33) plus 4 per struct
/// field number and variant case and 16 plus the order key per oset entry.
[[nodiscard]] std::uint64_t body_bytes(const sl::fabric::value &v) noexcept;
/// The summed body bytes of every record of `projection`.
[[nodiscard]] std::uint64_t projection_bytes(const sl::fabric::document_projection &projection);

namespace detail {

/// Scratch room for one component value (max_component_size, max_component_alignment).
struct alignas(ecs::max_component_alignment) value_scratch {
  std::array<std::byte, ecs::max_component_size> bytes{};
};

/// The state of one link, shared with its system and slots through weak references.
struct link_state {
  ecs::world *world = nullptr;
  frame_scheduler *frames = nullptr;
  ecs::system_scheduler *systems = nullptr;
  ecs::command_queue *commands = nullptr;
  mem::diagnostics *diagnostics = nullptr;
  const binding_registry *bindings = nullptr;
  proposal_sink *sink = nullptr;
  link_config config;
  std::uint32_t system_id = 0;
  std::thread::id owner;

  mem::tracking_resource memory{"link.entities"};
  ecs::entity_map<sl::fabric::record_id> entities{memory};
  std::vector<ecs::component_id> observed; // ascending; observed[i] writes outbox buffer i
  outbox out;
  std::optional<sl::fabric::document_projection> current;
  std::uint64_t current_bytes = 0;
  // The bound fields of every mapped record as the current projection gives them, read back
  // through the binding (defaults applied): the echo-suppression reference.
  std::map<sl::fabric::record_id, sl::fabric::value> projected;
  std::uint64_t failures_seen = 0;
  link_stats stats;
  value_scratch scratch;

  std::mutex mutex; // guards the four fields below
  std::optional<sl::fabric::document_projection> pending;
  std::uint64_t pending_bytes = 0;
  std::uint64_t current_generation = 0;
  bool has_current = false;
};

[[nodiscard]] bool is_owner(const link_state &s) noexcept;
/// SL_DEBUG_ASSERT that the workspace_alive token is lockable (ARCH3-R159).
void check_alive(const link_state &s) noexcept;
/// True when `p` covers exactly the configured domains.
[[nodiscard]] bool same_domains(const link_state &s,
                                const sl::fabric::document_projection &p) noexcept;
/// The ordered_key system_index of the link (the observe system's, or its id before build).
[[nodiscard]] std::uint16_t link_index(const link_state &s) noexcept;
/// Records the bound fields of `record` (body `body`) as the echo reference.
void cache_record(link_state &s, const sl::fabric::record_id &record, const fabric_binding &b,
                  const sl::fabric::value &body) noexcept;
/// Spawns the records `entries` (bound, unmapped) of `projection` in order through one command
/// buffer and maps them; the number spawned. A body the binding cannot read is invalid_argument
/// before anything spawns; the queue's flush errors.
[[nodiscard]] result<std::uint64_t> spawn_entries(link_state &s,
                                                  std::span<const load_entry> entries,
                                                  const sl::fabric::document_projection &projection,
                                                  std::uint64_t tick) noexcept;
/// The observe system body.
void observe(link_state &s, ecs::system_context &ctx) noexcept;
/// The observation_flush slot.
void flush_observations(link_state &s) noexcept;
/// The projection_rebase slot.
void rebase_projection(link_state &s, std::uint64_t tick) noexcept;
/// Reports the held projections' bytes as fabric.projection.
void report_projection_bytes(link_state &s) noexcept;

} // namespace detail

/// The link between one world and the host's document (ARCH3-R148 to R161).
class world_link {
  struct passkey {};

public:
  /// Registers link.observe (observe phase, reads on every bound component) on `systems` (not yet
  /// built) and the observation_flush and projection_rebase slots on `frames`. invalid_argument
  /// for no domains, an expired workspace_alive token, a binding of a component `w` does not know,
  /// that is not trivially copyable or whose size differs from its reflected type; the
  /// diagnostics' report_external error for a projection_tag not reserved as external;
  /// system_scheduler::add's errors. Every reference must outlive the link.
  [[nodiscard]] static result<std::unique_ptr<world_link>>
  attach(ecs::world &w, frame_scheduler &frames, ecs::system_scheduler &systems,
         ecs::command_queue &commands, mem::diagnostics &diagnostics,
         const binding_registry &bindings, proposal_sink &sink, link_config config) noexcept;

  world_link(passkey /*key*/, std::shared_ptr<detail::link_state> state) noexcept
      : state_(std::move(state)) {}
  world_link(const world_link &) = delete;
  world_link(world_link &&) = delete;
  world_link &operator=(const world_link &) = delete;
  world_link &operator=(world_link &&) = delete;
  ~world_link() = default;

  /// Spawns and maps every bound record of `source` not yet mapped (owner thread, before start or
  /// in the projection_rebase slot) and makes its projection current unless the current one is
  /// newer; this call's ingested and skipped_unbound counts. wrong_thread,
  /// structural_change_forbidden inside a phase, invalid_argument for other domains,
  /// projection_stale for a generation below the current one, spawn_entries' errors.
  [[nodiscard]] result<link_stats> ingest(const document_source &source) noexcept;
  /// Any thread: makes `projection` the pending one (replacing an earlier pending one), applied at
  /// the next projection_rebase slot. projection_stale when its generation is not above the
  /// current one, invalid_argument for other domains.
  [[nodiscard]] result<void> post_projection(sl::fabric::document_projection projection) noexcept;
  /// Observes the changes of `component` from the next frame on (owner thread, between frames):
  /// link_transient_component without a binding; observing twice is ok.
  [[nodiscard]] result<void> observe_component(ecs::component_id component) noexcept;
  /// record_id <-> entity of every ingested record.
  [[nodiscard]] const ecs::entity_map<sl::fabric::record_id> &entities() const noexcept {
    return state_->entities;
  }
  [[nodiscard]] link_stats stats() const noexcept { return state_->stats; }
  /// Projections held now (current and pending), at most max_projections_held.
  [[nodiscard]] std::uint32_t projections_held() const noexcept;
  /// The generation of the current projection (0 before the first ingest).
  [[nodiscard]] std::uint64_t current_generation() const noexcept;
  /// The id of the observe system (system_scheduler::add).
  [[nodiscard]] std::uint32_t system_id() const noexcept { return state_->system_id; }

private:
  std::shared_ptr<detail::link_state> state_;
};

} // namespace sl::runtime::link
