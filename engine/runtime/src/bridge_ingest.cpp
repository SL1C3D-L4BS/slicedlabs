// world_link attach, ingest and spawning (WO-097, SL-ARCH-003 ARCH3-R151, R153, R159; ADR-0027).

#include "sl/runtime/bridge.hpp"

#include "sl/core/assert.hpp"
#include "sl/core/error.hpp"
#include "sl/fabric/boundary.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/value.hpp"
#include "sl/runtime/ecs/command_buffer.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/memdiag.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/replay.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sl::runtime::link {

namespace fab = sl::fabric;

namespace {

// The observe system body: a weak reference, so a destroyed link observes nothing.
struct observe_body {
  std::weak_ptr<detail::link_state> state;
  void operator()(ecs::system_context &ctx) const noexcept {
    if (const std::shared_ptr<detail::link_state> s = state.lock()) {
      detail::observe(*s, ctx);
    }
  }
};

struct flush_hook {
  std::weak_ptr<detail::link_state> state;
  void operator()(const boundary_context & /*ctx*/) const noexcept {
    if (const std::shared_ptr<detail::link_state> s = state.lock()) {
      detail::flush_observations(*s);
    }
  }
};

struct rebase_hook {
  std::weak_ptr<detail::link_state> state;
  void operator()(const boundary_context &ctx) const noexcept {
    if (const std::shared_ptr<detail::link_state> s = state.lock()) {
      detail::rebase_projection(*s, ctx.tick);
    }
  }
};

std::vector<std::string> normalised(std::vector<std::string> domains) {
  std::ranges::sort(domains);
  const auto dup = std::ranges::unique(domains);
  domains.erase(dup.begin(), dup.end());
  return domains;
}

// The bound component may be rebuilt from bytes: trivially copyable, of its reflected size.
bool usable(const ecs::world &w, const binding_registry::entry &e) noexcept {
  if (!w.registry().contains(e.component) || e.binding.type() == nullptr) {
    return false;
  }
  const ecs::component_info &info = w.registry().info(e.component);
  return info.trivially_relocatable && info.size == e.binding.type()->size &&
         info.size <= ecs::max_component_size;
}

} // namespace

namespace detail {

bool is_owner(const link_state &s) noexcept { return std::this_thread::get_id() == s.owner; }

void check_alive(const link_state &s) noexcept {
  SL_DEBUG_ASSERT(s.config.workspace_alive.lock() != nullptr,
                  "world_link: the workspace was destroyed before the link (ARCH3-R159)");
}

bool same_domains(const link_state &s, const fab::document_projection &p) noexcept {
  return std::ranges::equal(p.domains(), s.config.domains);
}

std::uint16_t link_index(const link_state &s) noexcept {
  const auto index = s.systems->system_index(s.system_id);
  return index ? *index : static_cast<std::uint16_t>(s.system_id);
}

void cache_record(link_state &s, const fab::record_id &record, const fabric_binding &b,
                  const fab::value &body) noexcept {
  std::ranges::fill(s.scratch.bytes, std::byte{0});
  if (b.from_value(body, s.scratch.bytes.data())) {
    if (auto read_back = b.to_value(s.scratch.bytes.data())) {
      s.projected.insert_or_assign(record, std::move(*read_back));
      return;
    }
  }
  s.projected.insert_or_assign(record, body);
}

result<std::uint64_t> spawn_entries(link_state &s, std::span<const load_entry> entries,
                                    const fab::document_projection &projection,
                                    std::uint64_t tick) noexcept {
  if (entries.empty()) {
    return std::uint64_t{0};
  }
  // Every body must convert before anything spawns.
  std::vector<const fab::value *> bodies;
  bodies.reserve(entries.size());
  for (const load_entry &e : entries) {
    const binding_registry::entry *b = s.bindings->entry_of(e.type);
    auto view = projection.get(e.record);
    if (b == nullptr || !view || view->body == nullptr) {
      return make_error(errc::invalid_argument, "world_link: a record to spawn is not readable");
    }
    std::ranges::fill(s.scratch.bytes, std::byte{0});
    if (auto ok = b->binding.from_value(*view->body, s.scratch.bytes.data()); !ok) {
      return std::unexpected(ok.error());
    }
    bodies.push_back(view->body);
  }
  const det::ordered_key base{.tick = tick,
                              .phase = static_cast<std::uint8_t>(phase::end_frame),
                              .system_index = link_index(s),
                              .chunk_index = 0,
                              .local_seq = 0};
  ecs::command_buffer &buffer = s.commands->open(base);
  const std::uint32_t buffer_id = buffer.id();
  for (std::size_t i = 0; i < entries.size(); ++i) {
    static_cast<void>(buffer.spawn());
  }
  auto report = s.commands->flush(*s.world);
  if (!report) {
    return std::unexpected(report.error());
  }
  std::uint64_t spawned = 0;
  for (const auto &[pending, e] : report->resolved) {
    if (pending.buffer != buffer_id || pending.seq >= entries.size()) {
      continue;
    }
    const load_entry &entry = entries[pending.seq];
    const binding_registry::entry *b = s.bindings->entry_of(entry.type);
    std::ranges::fill(s.scratch.bytes, std::byte{0});
    if (!b->binding.from_value(*bodies[pending.seq], s.scratch.bytes.data())) {
      continue;
    }
    if (auto set =
            s.world->set_raw(e, b->component, s.scratch.bytes.data(), ecs::write_source::document);
        !set) {
      return std::unexpected(set.error());
    }
    if (auto mapped = s.entities.insert(entry.record, e); !mapped) {
      return std::unexpected(mapped.error());
    }
    cache_record(s, entry.record, b->binding, *bodies[pending.seq]);
    ++spawned;
  }
  return spawned;
}

} // namespace detail

result<std::unique_ptr<world_link>>
world_link::attach(ecs::world &w, frame_scheduler &frames, ecs::system_scheduler &systems,
                   ecs::command_queue &commands, mem::diagnostics &diagnostics,
                   const binding_registry &bindings, proposal_sink &sink,
                   link_config config) noexcept {
  config.domains = normalised(std::move(config.domains));
  if (config.domains.empty()) {
    return make_error(errc::invalid_argument, "world_link::attach: no projection domain");
  }
  if (config.workspace_alive.expired()) {
    return make_error(errc::invalid_argument,
                      "world_link::attach: the workspace_alive token is expired");
  }
  if (auto reserved = diagnostics.report_external(config.projection_tag, 0); !reserved) {
    return std::unexpected(reserved.error());
  }
  const bool all_usable = std::ranges::all_of(
      bindings.entries(), [&w](const binding_registry::entry &e) noexcept { return usable(w, e); });
  if (!all_usable) {
    return make_error(errc::invalid_argument,
                      "world_link::attach: a bound component is unknown, not trivially copyable "
                      "or not of its reflected size");
  }
  auto s = std::make_shared<detail::link_state>();
  s->world = &w;
  s->frames = &frames;
  s->systems = &systems;
  s->commands = &commands;
  s->diagnostics = &diagnostics;
  s->bindings = &bindings;
  s->sink = &sink;
  s->config = std::move(config);
  s->owner = std::this_thread::get_id();

  ecs::system_desc desc;
  desc.name = observe_system_name;
  desc.ph = phase::observe;
  for (const binding_registry::entry &e : bindings.entries()) {
    desc.access.components.reads.push_back(e.component);
  }
  desc.fn = observe_body{.state = s};
  auto id = systems.add(std::move(desc));
  if (!id) {
    return std::unexpected(id.error());
  }
  s->system_id = *id;
  frames.add_boundary_hook(boundary_slot::observation_flush, flush_hook{.state = s});
  frames.add_boundary_hook(boundary_slot::projection_rebase, rebase_hook{.state = s});
  return std::make_unique<world_link>(passkey{}, std::move(s));
}

result<link_stats> world_link::ingest(const document_source &source) noexcept {
  detail::link_state &s = *state_;
  if (!detail::is_owner(s)) {
    return make_error(errc::wrong_thread, "world_link::ingest off the owner thread");
  }
  if (s.world->phase_active()) {
    return make_error(errc::structural_change_forbidden, "world_link::ingest inside a phase");
  }
  const fab::document_projection &projection = source.projection();
  if (!detail::same_domains(s, projection)) {
    return make_error(errc::invalid_argument, "world_link::ingest: other projection domains");
  }
  if (s.current && projection.generation() < s.current->generation()) {
    return make_error(errc::projection_stale, "world_link::ingest: an older projection");
  }
  link_stats delta;
  std::vector<load_entry> spawn;
  for (const load_entry &e : source.load_plan()) {
    if (s.bindings->entry_of(e.type) == nullptr) {
      ++delta.skipped_unbound;
    } else if (!s.entities.find(e.record)) {
      spawn.push_back(e);
    }
  }
  auto spawned = detail::spawn_entries(s, spawn, projection, s.frames->current().now);
  if (!spawned) {
    return std::unexpected(spawned.error());
  }
  delta.ingested = *spawned;
  s.stats.ingested += delta.ingested;
  s.stats.skipped_unbound += delta.skipped_unbound;
  if (!s.current || projection.generation() >= s.current->generation()) {
    s.current = projection;
    s.current_bytes = projection_bytes(projection);
    for (const load_entry &e : source.load_plan()) {
      const binding_registry::entry *b = s.bindings->entry_of(e.type);
      auto view = projection.get(e.record);
      if (b != nullptr && view && view->body != nullptr && s.entities.find(e.record)) {
        detail::cache_record(s, e.record, b->binding, *view->body);
      }
    }
    const std::scoped_lock lock(s.mutex);
    s.current_generation = projection.generation();
    s.has_current = true;
  }
  detail::report_projection_bytes(s);
  delta.projection_bytes = s.stats.projection_bytes;
  return delta;
}

result<void> world_link::observe_component(ecs::component_id component) noexcept {
  detail::link_state &s = *state_;
  if (!detail::is_owner(s)) {
    return make_error(errc::wrong_thread, "world_link::observe_component off the owner thread");
  }
  if (s.bindings->find_component(component) == nullptr) {
    return make_error(errc::link_transient_component,
                      "world_link::observe_component: the component has no binding");
  }
  const auto at = std::ranges::lower_bound(s.observed, component);
  if (at != s.observed.end() && *at == component) {
    return {};
  }
  s.observed.insert(at, component);
  while (s.out.buffers() < s.observed.size()) {
    static_cast<void>(s.out.add_buffer());
  }
  return {};
}

std::uint32_t world_link::projections_held() const noexcept {
  detail::link_state &s = *state_;
  const std::scoped_lock lock(s.mutex);
  return (s.has_current ? 1U : 0U) + (s.pending.has_value() ? 1U : 0U);
}

std::uint64_t world_link::current_generation() const noexcept {
  detail::link_state &s = *state_;
  const std::scoped_lock lock(s.mutex);
  return s.current_generation;
}

} // namespace sl::runtime::link
