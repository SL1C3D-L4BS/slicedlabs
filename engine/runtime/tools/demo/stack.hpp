#pragma once

// The lantern-field stack (WO-099, SL-ARCH-003 ARCH3-R169, R094 to R100, R148 to R161): the
// public runtime pieces composed the way runtime::start composes them (runtime_lifecycle.cpp
// start_jobs and start_frames), plus the Fabric link, which runtime does not host (deviation
// D-099-1): the in-tool host (host.hpp), the headless platform with the demo's own manual clock
// (step_clock, 16'666'667 ns per frame) and a record_sink keeping every wall sample (the replay
// input), the runtime memory tags, tracking resources and diagnostics, a work-stealing job system,
// the frame arena and frame scheduler at 60 Hz, the world with its resources and the
// lantern_burnout channel, the command queue, the system scheduler with the eight systems of
// systems.cpp and the world_link over the two document-bound components. Owner thread (the
// thread that called make).
//
// Components (every field snapshot and hashed):
//   lantern_pos {x, y, z f32}, lantern_light {phase f32, color u32}, lantern_life {energy f32,
//   age u32}: transient, 50000 lanterns by default;
//   beacon {glow f32 = field 1, lit u32 = field 2} bound to content.scene.Beacon,
//   ward {radius f32 = 1, strength f32 = 2} bound to content.scene.Ward: 128 each, ingested;
//   resources scene_clock {step, seed}, glow_stats {total, lit}, watch_stats {changes, glow}.

#include "host.hpp"

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/bridge.hpp"
#include "sl/runtime/ecs/command_buffer.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/events.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/headless_platform.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/memdiag.hpp"
#include "sl/runtime/memory.hpp"
#include "sl/runtime/platform.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/snapshot.hpp"
#include "sl/runtime/work_queue.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sl::runtime::demo {

struct lantern_pos {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
};
struct lantern_light {
  float phase = 0.0F;
  std::uint32_t color = 0;
};
struct lantern_life {
  float energy = 0.0F;
  std::uint32_t age = 0;
};
/// content.scene.Beacon {1 glow, 2 lit}.
struct beacon {
  float glow = 0.0F;
  std::uint32_t lit = 0;
};
/// content.scene.Ward {1 radius, 2 strength}.
struct ward {
  float radius = 0.0F;
  float strength = 0.0F;
};
/// The applied steps (the hash timeline: the logical tick is not rewound by a restore) and the
/// scene seed.
struct scene_clock {
  std::uint64_t step = 0;
  std::uint64_t seed = 0;
};
/// lantern.glow's reduction.
struct glow_stats {
  double total = 0.0;
  std::uint64_t lit = 0;
};
/// beacon.watch's counts.
struct watch_stats {
  std::uint64_t changes = 0;
  double glow = 0.0;
};
/// The event lantern.decay writes when a lantern's energy reaches 0.
struct lantern_burnout {
  std::uint32_t index = 0;
  std::uint32_t generation = 0;
  float energy = 0.0F;
};

inline constexpr ecs::resource_key clock_key{1};
inline constexpr ecs::resource_key glow_key{2};
inline constexpr ecs::resource_key watch_key{3};
/// lantern.glow rewrites every beacon once per this many steps.
inline constexpr std::uint64_t beacon_every = 20;
/// The beacons one rewrite touches (a rotating window: one Fabric commit of 16 field changes).
inline constexpr std::uint32_t tune_window = 8;
/// The energy of a spawned lantern: it burns out after this many steps.
inline constexpr float spawn_energy = 100.0F;
/// The event name of lantern_burnout.
inline constexpr std::string_view burnout_event = "lantern_burnout";

/// The demo's clock_source: a manual monotonic clock the runner moves. Any thread reads.
class step_clock final : public clock_source {
public:
  [[nodiscard]] nanos now() const noexcept override { return nanos{now_.load()}; }
  void set(nanos t) noexcept { now_.store(t.count()); }
  void advance(nanos d) noexcept { now_.store(now_.load() + d.count()); }

private:
  std::atomic<std::int64_t> now_{0};
};

/// Keeps every wall sample the frame scheduler passes (owner thread), up to the capacity given
/// to reserve; samples beyond it are counted, not kept.
class sample_recorder final : public record_sink {
public:
  void reserve(std::size_t n) { samples_.reserve(n); }
  void on_wall_sample(std::uint64_t frame, nanos sample) noexcept override;
  [[nodiscard]] const std::vector<nanos> &samples() const noexcept { return samples_; }
  [[nodiscard]] std::uint64_t overflow() const noexcept { return overflow_; }

private:
  std::vector<nanos> samples_;
  std::uint64_t overflow_ = 0;
};

/// The component ids of the scene.
struct scene_ids {
  ecs::component_id pos;
  ecs::component_id light;
  ecs::component_id life;
  ecs::component_id beacon;
  ecs::component_id ward;
};

/// What a stack is built from.
struct stack_options {
  std::uint64_t seed = 1;
  std::uint32_t workers = 1;
  ecs::execution_policy mode = ecs::execution_policy::parallel;
  std::uint32_t entities = 50000;
  /// The headless platform's root directory (the evidence directory).
  std::string root_dir;
  /// Wall samples to keep (frames the stack will run).
  std::size_t frames_hint = 1024;
  /// The world's change tick before the priming frame (resume: above the snapshot's).
  std::uint32_t change_tick = 0;
};

/// The whole stack. Members are destroyed in reverse order: the link first, the host last; use
/// finish() to stop and read the leaks before the tracking resources go.
struct scene_stack {
  std::unique_ptr<lantern_host> host;
  std::unique_ptr<headless_platform> headless;
  step_clock clock;
  sample_recorder recorder;
  std::unique_ptr<platform> plat;
  mem::tag_registry tags;
  std::unique_ptr<mem::tracking_resource> world_memory;
  std::unique_ptr<mem::tracking_resource> frame_memory;
  std::unique_ptr<mem::diagnostics> diag;
  type_registry types;
  ecs::component_registry components;
  event_registry events;
  scene_ids ids;
  jobs::scheduler_policy *policy = nullptr;
  std::unique_ptr<jobs::job_system> js;
  std::unique_ptr<mem::frame_arena> arena;
  std::unique_ptr<frame_scheduler> frames;
  std::unique_ptr<ecs::world> world;
  std::unique_ptr<ecs::command_queue> queue;
  std::unique_ptr<ecs::system_scheduler> systems;
  link::binding_registry bindings;
  std::unique_ptr<link::world_link> link;
  event_channel<lantern_burnout> *channel = nullptr;
  std::unique_ptr<events_contributor> contributor;
  /// The scripted system.fail: lantern.decay fails its next run when set (and clears it).
  std::atomic<bool> fail_next{false};
  std::uint32_t worker_count = 0;
  std::uint64_t frames_run = 0;
  /// The system names by system id (the scheduler keeps no name accessor).
  std::vector<std::string> system_names;
  std::string schedule_json;
  sl::fabric::digest schedule_digest;
  sl::fabric::digest abi_digest;

  /// Builds and wires everything, ingests the host's projection, creates options.entities
  /// lanterns from options.seed and builds the schedule (no frame run yet).
  [[nodiscard]] static result<std::unique_ptr<scene_stack>> make(const stack_options &options);

  /// Runs the priming frame (the first clock sample: no tick).
  [[nodiscard]] result<frame_stats> prime();
  /// Moves the clock by one frame length and runs one frame.
  [[nodiscard]] result<frame_stats> step();
  /// Sets the clock to `sample` (a recorded wall sample) and runs one frame.
  [[nodiscard]] result<frame_stats> step_at(nanos sample);
  /// The state hash of the world (no contributor: the event lists carry logical ticks).
  [[nodiscard]] result<sl::fabric::digest> hash() const;
  /// A snapshot of the world and the event channel.
  [[nodiscard]] result<snapshot> take(std::uint64_t frame, std::uint64_t tick) const;
  /// Restores `s`, then moves the change tick back above every system's last run.
  [[nodiscard]] result<void> restore(const snapshot &s) const;
  /// The scene clock resource (zeros when missing).
  [[nodiscard]] scene_clock scene() const noexcept;
  [[nodiscard]] glow_stats glow() const noexcept;
  [[nodiscard]] watch_stats watch() const noexcept;
  /// The allocations of every tracking resource so far.
  [[nodiscard]] std::uint64_t tracked_allocs() const noexcept;
  /// Live entities.
  [[nodiscard]] std::uint64_t population() const noexcept;
  /// Stops everything in reverse start order (the link first, the job system last) and returns
  /// the shutdown memory report with its leaks; the host and the tracking resources stay.
  [[nodiscard]] mem::memory_report finish();
};

/// Registers the scene's systems on s.systems (systems.cpp): lantern.drift and lantern.flicker
/// (pre_tick), lantern.decay and lantern.glow (tick), lantern.reap and lantern.spawn (post_tick)
/// and beacon.watch (observe); link.observe is the eighth (world_link::attach).
[[nodiscard]] result<void> add_scene_systems(scene_stack &s);

/// splitmix64.
[[nodiscard]] constexpr std::uint64_t mix(std::uint64_t x) noexcept {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31U);
}

/// The values of lantern `key` (a creation index or a spawn key): position in [-32, 32] squared,
/// phase in [0, 1), color from the high bits, `energy`, age 0.
struct lantern_values {
  lantern_pos pos;
  lantern_light light;
  lantern_life life;
};
[[nodiscard]] lantern_values make_lantern(std::uint64_t key, float energy) noexcept;

} // namespace sl::runtime::demo
