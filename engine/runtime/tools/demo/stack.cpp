// The lantern-field stack (WO-099). See stack.hpp.

#include "stack.hpp"

#include "host.hpp"

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/bridge.hpp"
#include "sl/runtime/ecs/command_buffer.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/events.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/headless_platform.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/memdiag.hpp"
#include "sl/runtime/memory.hpp"
#include "sl/runtime/platform.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/scheduler_stats.hpp"
#include "sl/runtime/snapshot.hpp"
#include "sl/runtime/time.hpp"
#include "sl/runtime/work_queue.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sl::runtime::demo {

namespace {

constexpr field_flags kept = field_flags::snapshot | field_flags::hashed;
/// One frame at 60 Hz (the manual clock's step).
constexpr nanos frame_step{16'666'667};

[[nodiscard]] type_info pos_info() {
  type_builder<lantern_pos> b("demo.LanternPos");
  SL_RT_FIELD(b, lantern_pos, x, kept);
  SL_RT_FIELD(b, lantern_pos, y, kept);
  SL_RT_FIELD(b, lantern_pos, z, kept);
  return b.build();
}

[[nodiscard]] type_info light_info() {
  type_builder<lantern_light> b("demo.LanternLight");
  SL_RT_FIELD(b, lantern_light, phase, kept);
  SL_RT_FIELD(b, lantern_light, color, kept);
  return b.build();
}

[[nodiscard]] type_info life_info() {
  type_builder<lantern_life> b("demo.LanternLife");
  SL_RT_FIELD(b, lantern_life, energy, kept);
  SL_RT_FIELD(b, lantern_life, age, kept);
  return b.build();
}

[[nodiscard]] type_info beacon_info() {
  type_builder<beacon> b(beacon_type);
  SL_RT_FIELD_BOUND(b, beacon, glow, kept, 1);
  SL_RT_FIELD_BOUND(b, beacon, lit, kept, 2);
  b.schema(beacon_type);
  return b.build();
}

[[nodiscard]] type_info ward_info() {
  type_builder<ward> b(ward_type);
  SL_RT_FIELD_BOUND(b, ward, radius, kept, 1);
  SL_RT_FIELD_BOUND(b, ward, strength, kept, 2);
  b.schema(ward_type);
  return b.build();
}

[[nodiscard]] type_info clock_info() {
  type_builder<scene_clock> b("demo.SceneClock");
  SL_RT_FIELD(b, scene_clock, step, kept);
  SL_RT_FIELD(b, scene_clock, seed, kept);
  return b.build();
}

[[nodiscard]] type_info glow_info() {
  type_builder<glow_stats> b("demo.GlowStats");
  SL_RT_FIELD(b, glow_stats, total, kept);
  SL_RT_FIELD(b, glow_stats, lit, kept);
  return b.build();
}

[[nodiscard]] type_info watch_info() {
  type_builder<watch_stats> b("demo.WatchStats");
  SL_RT_FIELD(b, watch_stats, changes, kept);
  SL_RT_FIELD(b, watch_stats, glow, kept);
  return b.build();
}

[[nodiscard]] type_info burnout_info() {
  type_builder<lantern_burnout> b("demo.LanternBurnout");
  SL_RT_FIELD(b, lantern_burnout, index, kept);
  SL_RT_FIELD(b, lantern_burnout, generation, kept);
  SL_RT_FIELD(b, lantern_burnout, energy, kept);
  return b.build();
}

template <class T>
[[nodiscard]] result<ecs::component_id> add_component(scene_stack &s, std::string_view name,
                                                      const type_info &info) {
  auto added = s.types.add(info);
  if (!added) {
    return std::unexpected(added.error());
  }
  return s.components.add<T>(name, *added);
}

[[nodiscard]] result<void> register_types(scene_stack &s) {
  auto pos = add_component<lantern_pos>(s, "demo.lantern_pos", pos_info());
  auto light = add_component<lantern_light>(s, "demo.lantern_light", light_info());
  auto life = add_component<lantern_life>(s, "demo.lantern_life", life_info());
  auto b = add_component<beacon>(s, "demo.beacon", beacon_info());
  auto w = add_component<ward>(s, "demo.ward", ward_info());
  auto clock = add_component<scene_clock>(s, "demo.scene_clock", clock_info());
  auto glow = add_component<glow_stats>(s, "demo.glow_stats", glow_info());
  auto watch = add_component<watch_stats>(s, "demo.watch_stats", watch_info());
  for (const result<ecs::component_id> *r : {&pos, &light, &life, &b, &w, &clock, &glow, &watch}) {
    if (!*r) {
      return std::unexpected(r->error());
    }
  }
  s.ids = scene_ids{.pos = *pos, .light = *light, .life = *life, .beacon = *b, .ward = *w};
  if (auto added = s.events.add<lantern_burnout>(burnout_event, burnout_info()); !added) {
    return std::unexpected(added.error());
  }
  if (auto frozen = s.types.freeze(); !frozen) {
    return frozen;
  }
  if (auto frozen = s.components.freeze(); !frozen) {
    return frozen;
  }
  return s.events.freeze();
}

[[nodiscard]] result<void> memory_root(scene_stack &s) {
  for (const std::string_view name : mem::runtime_tag_names) {
    if (auto added = s.tags.add(name); !added) {
      return std::unexpected(added.error());
    }
  }
  for (const std::string_view name : mem::runtime_external_tag_names) {
    if (auto added = s.tags.reserve_external(name); !added) {
      return std::unexpected(added.error());
    }
  }
  if (auto frozen = s.tags.freeze(); !frozen) {
    return frozen;
  }
  auto world_tag = s.tags.find("ecs.entities");
  auto frame_tag = s.tags.find("frame.arena");
  if (!world_tag || !frame_tag) {
    return make_error(errc::internal, "lantern field: a runtime tag is missing");
  }
  s.world_memory = std::make_unique<mem::tracking_resource>("demo.world", *world_tag, &s.tags);
  s.frame_memory = std::make_unique<mem::tracking_resource>("demo.frame", *frame_tag, &s.tags);
  s.diag = std::make_unique<mem::diagnostics>(s.tags, 1, mem::report_interval_frames);
  s.diag->add(*s.world_memory);
  s.diag->add(*s.frame_memory);
  return {};
}

[[nodiscard]] result<void> start_jobs(scene_stack &s, const stack_options &options) {
  jobs::job_config jc;
  jc.workers = options.workers;
  jc.capacity = jobs::job_capacity_default;
  jc.instance_id = 1;
  jc.seed = options.seed;
  jc.make_queue = jobs::make_ws_queue;
  jobs::steal_options steal;
  steal.seed = options.seed;
  auto policy = jobs::make_steal_policy(steal);
  if (!policy) {
    return std::unexpected(policy.error());
  }
  s.policy = policy->get();
  jc.policy = std::move(*policy);
  auto started = jobs::job_system::start(*s.plat, std::move(jc));
  if (!started) {
    return std::unexpected(started.error());
  }
  s.js = std::move(*started);
  s.worker_count = s.js->worker_count();
  return {};
}

[[nodiscard]] result<void> start_frames(scene_stack &s, const stack_options &options) {
  auto frame_tag = s.tags.find("frame.arena");
  s.arena = std::make_unique<mem::frame_arena>(*s.frame_memory, s.js->worker_count(),
                                               mem::frame_arena_block_bytes,
                                               frame_tag ? *frame_tag : mem::memory_tag{});
  s.diag->add("frame.arena", *s.arena);
  const mem::memory_tag arena_tag = frame_tag ? *frame_tag : mem::memory_tag{};
  // Every arena of both sets takes its first block now, on the owner thread: which worker first
  // opens a command buffer depends on stealing, so a lazy first block could land in any frame.
  // Each flip resets the set it makes current (the blocks stay), and the owner of a worker arena
  // is the thread of its first allocation after a flip.
  for (int set = 0; set < 2; ++set) {
    for (std::uint32_t k = 0; k <= s.arena->workers(); ++k) {
      auto p = s.arena->for_worker(k).try_allocate(1, 1, arena_tag);
      if (!p) {
        return std::unexpected(p.error());
      }
    }
    s.arena->flip();
  }
  frame_config fc;
  fc.rate = time::tick_rate{.num = 60, .den = 1};
  auto frames = frame_scheduler::make(*s.js, *s.plat, *s.arena, fc);
  if (!frames) {
    return std::unexpected(frames.error());
  }
  s.frames = std::move(*frames);
  auto w = ecs::world::make(s.components, *s.world_memory, ecs::world_config{});
  if (!w) {
    return std::unexpected(w.error());
  }
  s.world = std::move(*w);
  s.world->set_job_system(s.js.get());
  s.world->set_change_tick(options.change_tick);
  s.queue = std::make_unique<ecs::command_queue>(s.components, *s.arena);
  s.systems = std::make_unique<ecs::system_scheduler>(*s.js);
  s.systems->set_policy(options.mode);
  return {};
}

[[nodiscard]] result<void> add_resources(scene_stack &s, const stack_options &options) {
  if (auto added = s.world->add_resource(clock_key, scene_clock{.step = 0, .seed = options.seed});
      !added) {
    return added;
  }
  if (auto added = s.world->add_resource(glow_key, glow_stats{}); !added) {
    return added;
  }
  if (auto added = s.world->add_resource(watch_key, watch_stats{}); !added) {
    return added;
  }
  auto channel = add_event_channel<lantern_burnout>(*s.world, s.events);
  if (!channel) {
    return std::unexpected(channel.error());
  }
  s.channel = *channel;
  attach(s.channel->stream(), *s.frames);
  const std::array<event_stream *, 1> streams{&s.channel->stream()};
  s.contributor = std::make_unique<events_contributor>(std::span<event_stream *const>(streams));
  return {};
}

[[nodiscard]] result<void> bind(scene_stack &s, ecs::component_id component,
                                std::string_view type) {
  auto t = s.types.find(type);
  if (!t) {
    return std::unexpected(t.error());
  }
  auto b = fabric_binding::bind(**t, s.host->schemas(), type);
  if (!b) {
    return std::unexpected(b.error());
  }
  return s.bindings.add(component, std::move(*b));
}

[[nodiscard]] result<void> attach_link(scene_stack &s) {
  if (auto ok = bind(s, s.ids.beacon, beacon_type); !ok) {
    return ok;
  }
  if (auto ok = bind(s, s.ids.ward, ward_type); !ok) {
    return ok;
  }
  auto tag = s.tags.find(link::projection_tag_name);
  if (!tag) {
    return std::unexpected(tag.error());
  }
  link::link_config config;
  config.actor = lantern_host::runtime_actor();
  config.domains = {std::string(scene_domain)};
  config.workspace_alive = s.host->alive_token();
  config.projection_tag = *tag;
  auto attached = link::world_link::attach(*s.world, *s.frames, *s.systems, *s.queue, *s.diag,
                                           s.bindings, *s.host, std::move(config));
  if (!attached) {
    return std::unexpected(attached.error());
  }
  s.link = std::move(*attached);
  const std::uint32_t id = s.link->system_id();
  if (s.system_names.size() <= id) {
    s.system_names.resize(std::size_t{id} + 1U);
  }
  s.system_names[id] = std::string(link::observe_system_name);
  s.host->attach(*s.link);
  return s.link->observe_component(s.ids.beacon);
}

[[nodiscard]] result<void> populate(scene_stack &s, const stack_options &options) {
  // The records first: their entities take the same indices in every stack of this scene.
  auto source = link::document_source::make(s.host->projection());
  if (!source) {
    return std::unexpected(source.error());
  }
  if (auto ingested = s.link->ingest(*source); !ingested) {
    return std::unexpected(ingested.error());
  }
  for (std::uint32_t i = 0; i < options.entities; ++i) {
    // Energy 1 + i % 100: one lantern in a hundred burns out at every step.
    lantern_values v =
        make_lantern((options.seed * 0x100000001B3ULL) + i, 1.0F + static_cast<float>(i % 100U));
    auto e = s.world->create_with(v.pos, v.light, v.life);
    if (!e) {
      return std::unexpected(e.error());
    }
  }
  return {};
}

[[nodiscard]] result<void> build(scene_stack &s) {
  mem::diagnostics *d = s.diag.get();
  s.frames->add_boundary_hook(boundary_slot::diagnostics, [d](const boundary_context &bc) noexcept {
    d->on_frame_end(bc.frame_index);
  });
  if (auto built = s.systems->build(*s.frames, *s.world, *s.queue); !built) {
    return built;
  }
  if (auto built = s.frames->build(); !built) {
    return built;
  }
  s.schedule_json = s.systems->export_json();
  s.schedule_digest = detail::sha256_of(std::as_bytes(std::span(s.schedule_json)));
  s.abi_digest = s.components.layout_digest();
  return {};
}

} // namespace

void sample_recorder::on_wall_sample(std::uint64_t /*frame*/, nanos sample) noexcept {
  if (samples_.size() < samples_.capacity()) {
    samples_.push_back(sample);
  } else {
    ++overflow_;
  }
}

lantern_values make_lantern(std::uint64_t key, float energy) noexcept {
  const std::uint64_t r = mix(key);
  const auto centered = [](std::uint64_t x) noexcept {
    return static_cast<float>(static_cast<std::int64_t>(x % 6401U) - 3200) / 100.0F;
  };
  lantern_values v;
  v.pos.x = centered(r);
  v.pos.y = centered(r >> 16U);
  v.pos.z = 0.0F;
  v.light.phase = static_cast<float>((r >> 24U) % 1000U) / 1000.0F;
  v.light.color = static_cast<std::uint32_t>(r >> 32U);
  v.life.energy = energy;
  v.life.age = 0;
  return v;
}

result<std::unique_ptr<scene_stack>> scene_stack::make(const stack_options &options) {
  auto s = std::make_unique<scene_stack>();
  auto host = lantern_host::make(options.seed);
  if (!host) {
    return std::unexpected(host.error());
  }
  s->host = std::move(*host);
  auto headless =
      make_headless_platform(platform_options{.root_dir = options.root_dir, .instance_id = 1});
  if (!headless) {
    return std::unexpected(headless.error());
  }
  s->headless = std::move(*headless);
  s->recorder.reserve(options.frames_hint);
  platform &base = s->headless->get();
  s->plat = std::make_unique<platform>(platform{.clock = s->clock,
                                                .wall = base.wall,
                                                .threads = base.threads,
                                                .input = base.input,
                                                .files = base.files,
                                                .win = base.win,
                                                .sink = s->recorder,
                                                .instance_id = base.instance_id});
  if (auto ok = memory_root(*s); !ok) {
    return std::unexpected(ok.error());
  }
  if (auto ok = register_types(*s); !ok) {
    return std::unexpected(ok.error());
  }
  if (auto ok = start_jobs(*s, options); !ok) {
    return std::unexpected(ok.error());
  }
  if (auto ok = start_frames(*s, options); !ok) {
    return std::unexpected(ok.error());
  }
  if (auto ok = add_resources(*s, options); !ok) {
    return std::unexpected(ok.error());
  }
  if (auto ok = attach_link(*s); !ok) {
    return std::unexpected(ok.error());
  }
  if (auto ok = add_scene_systems(*s); !ok) {
    return std::unexpected(ok.error());
  }
  if (auto ok = populate(*s, options); !ok) {
    return std::unexpected(ok.error());
  }
  if (auto ok = build(*s); !ok) {
    return std::unexpected(ok.error());
  }
  return s;
}

result<frame_stats> scene_stack::prime() {
  ++frames_run;
  return frames->run_frame();
}

result<frame_stats> scene_stack::step() {
  clock.advance(frame_step);
  ++frames_run;
  return frames->run_frame();
}

result<frame_stats> scene_stack::step_at(nanos sample) {
  clock.set(sample);
  ++frames_run;
  return frames->run_frame();
}

result<sl::fabric::digest> scene_stack::hash() const {
  return state_hash(*world, components, types, {});
}

result<snapshot> scene_stack::take(std::uint64_t frame, std::uint64_t tick) const {
  const std::array<const det::snapshot_contributor *, 1> with{contributor.get()};
  const snapshot_meta meta{.frame = frame,
                           .tick = tick,
                           .schedule_digest = schedule_digest,
                           .runtime_abi_digest = abi_digest};
  return sl::runtime::take(*world, components, types, with, meta);
}

result<void> scene_stack::restore(const snapshot &s) const {
  // Neither the logical tick nor a system's last run goes back with a restore: keep the change
  // tick ahead of both, so a restored stamp reads as old and a new write as new (D-099-5).
  const std::uint32_t ahead = world->change_tick();
  const std::array<det::snapshot_contributor *, 1> with{contributor.get()};
  if (auto restored = sl::runtime::restore(*world, components, types, with, s, abi_digest);
      !restored) {
    return restored;
  }
  if (world->change_tick() < ahead) {
    world->set_change_tick(ahead);
  }
  // ARCH3-R030: no background result from before the restore reaches the world.
  js->drain_results([](const jobs::background_result & /*r*/) noexcept {});
  return {};
}

scene_clock scene_stack::scene() const noexcept {
  auto found = world->resource<scene_clock>(clock_key);
  return found ? **found : scene_clock{};
}

glow_stats scene_stack::glow() const noexcept {
  auto found = world->resource<glow_stats>(glow_key);
  return found ? **found : glow_stats{};
}

watch_stats scene_stack::watch() const noexcept {
  auto found = world->resource<watch_stats>(watch_key);
  return found ? **found : watch_stats{};
}

std::uint64_t scene_stack::tracked_allocs() const noexcept {
  return world_memory->totals().allocs + frame_memory->totals().allocs;
}

std::uint64_t scene_stack::population() const noexcept { return world->stats().rows; }

mem::memory_report scene_stack::finish() {
  link.reset();
  contributor.reset();
  systems.reset();
  queue.reset();
  channel = nullptr;
  world.reset();
  frames.reset();
  arena.reset();
  if (js != nullptr) {
    // cppcheck-suppress unreadVariable ; nothing is left to report a stop error to
    [[maybe_unused]] const result<void> stopped = js->stop();
    js.reset();
  }
  mem::memory_report report = diag->make_report(frames_run);
  report.leaks = diag->leaks();
  return report;
}

} // namespace sl::runtime::demo
