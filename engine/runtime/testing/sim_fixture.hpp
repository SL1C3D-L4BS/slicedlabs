#pragma once

// testing::sim_fixture (WO-094, SL-ARCH-003 ARCH3-R090, R121 to R126, RT-INV-SCHED-06): a world
// with the eight world_gen components, seeded with sources (position, velocity, aligned, padded,
// tag) and mortals (position, velocity, aligned, health), and eight reference systems over
// apply_inputs, pre_tick, tick and post_tick using integer math and float + - * / and sqrt only:
//   apply_inputs  sim.input_impulse  par_for_each: velocity += f(aligned, tick)
//                 sim.padded_mix     for_each: an integer LCG over padded
//   pre_tick      sim.damp           for_each: velocity *= 0.99 (registered first, after integrate)
//                 sim.integrate      par_for_each: position += velocity, z = sqrt(x*x + y*y)
//   tick          sim.health_regen   par_for_each: hp += (aligned + tick) % 5 - 3, armor churn
//                 sim.spawner        commands: a source spawns a mortal now and then (padded.b)
//   post_tick     sim.reaper         commands: destroys hp <= 0, adds or removes gen_tag by armor
//                 sim.energy_sum     par_for_each into per-batch ordered_buffers merged by
//                                    ordered_key into the sim_stats resource
// The runner advances the scripted clock one tick per frame (50 Hz), so run(n) runs n ticks and
// returns the state hash after each. fixture_hash() is det::state_hasher over hash_world (the
// entity allocator and every hashed component field in iteration order) and sim_stats.
// testing::sched_rig is the bare stack under it (jobs, frame scheduler, world, command queue and
// system scheduler) for the sched tests. Owner-thread (`thread_safety`, testing::*). Include it
// by path: "../../../engine/runtime/testing/sim_fixture.hpp".

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/ecs/command_buffer.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/memory.hpp"

#include "scripted_platform.hpp"
#include "world_gen.hpp"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace sl::runtime::testing {

/// One frame of the rigs: 20 ms at 50 Hz.
inline constexpr nanos sched_tick_length{20'000'000};

/// Jobs on `workers` workers (its own job system, or a shared one), a frame scheduler at 50 Hz
/// with its frame arena, the eight world_gen components (frozen), an empty world, a command queue
/// on the frame arena and a system scheduler. Fields are destroyed in reverse order (the
/// scheduler first, an owned job system last).
struct sched_rig {
  scripted_platform plat;
  mem::tracking_resource upstream{"sched.rig.arena"};
  mem::tracking_resource world_memory{"sched.rig.world"};
  std::unique_ptr<jobs::job_system> own_jobs;
  jobs::job_system *js = nullptr;
  std::unique_ptr<mem::frame_arena> arena;
  std::unique_ptr<frame_scheduler> frames;
  ecs::component_registry registry;
  gen_ids ids;
  std::unique_ptr<ecs::world> world;
  std::unique_ptr<ecs::command_queue> queue;
  std::unique_ptr<ecs::system_scheduler> systems;

  [[nodiscard]] static result<std::unique_ptr<sched_rig>> make(std::uint32_t workers);
  /// A rig on `shared` (owner thread; it must outlive the rig), for many short schedules.
  [[nodiscard]] static result<std::unique_ptr<sched_rig>> make_on(jobs::job_system &shared);
  /// Starts a job system of `workers` workers on `plat` (capacity 1024).
  [[nodiscard]] static result<std::unique_ptr<jobs::job_system>> start_jobs(platform &plat,
                                                                            std::uint32_t workers);
  /// The frame scheduler, registry, world, queue and system scheduler on `js`.
  [[nodiscard]] result<void> finish();
  /// systems->build then frames->build.
  [[nodiscard]] result<void> build() const;
  /// Advances the clock one tick and runs one frame.
  [[nodiscard]] result<frame_stats> step();
};

/// The resource sim.energy_sum writes.
struct sim_stats {
  double energy = 0.0;
  std::uint64_t population = 0;
};

inline constexpr ecs::resource_key sim_stats_key{1};

struct sim_config {
  std::uint32_t workers = 2;
  ecs::execution_policy policy = ecs::execution_policy::parallel;
  std::uint32_t ticks = 60;
  std::uint64_t seed = 1;
  /// Initial entities (half sources, half mortals); above par_batch_min_rows, so par_for_each
  /// runs two batches.
  std::uint32_t entities = 4400;
};

class sim_fixture {
  struct passkey {};

public:
  /// A built fixture: jobs on config.workers workers, the world seeded from config.seed, the
  /// eight systems registered and built under config.policy, and one priming frame run (the first
  /// clock sample: no tick).
  [[nodiscard]] static result<std::unique_ptr<sim_fixture>> make(const sim_config &config);

  sim_fixture(passkey /*key*/, const sim_config &config, std::unique_ptr<sched_rig> rig) noexcept
      : config_(config), rig_(std::move(rig)) {}
  sim_fixture(const sim_fixture &) = delete;
  sim_fixture(sim_fixture &&) = delete;
  sim_fixture &operator=(const sim_fixture &) = delete;
  sim_fixture &operator=(sim_fixture &&) = delete;
  ~sim_fixture() = default;

  /// Runs `ticks` frames of one tick each; the state hash after each. A frame error, a failed
  /// system or a failed playback is returned.
  [[nodiscard]] result<std::vector<fabric::digest>> run(std::uint32_t ticks);
  /// Runs config.ticks ticks.
  [[nodiscard]] result<std::vector<fabric::digest>> run() { return run(config_.ticks); }

  [[nodiscard]] fabric::digest fixture_hash() const;

  [[nodiscard]] sched_rig &rig() noexcept { return *rig_; }
  [[nodiscard]] const ecs::system_scheduler &systems() const noexcept { return *rig_->systems; }
  [[nodiscard]] const sim_stats &stats() const noexcept;
  [[nodiscard]] std::uint64_t ticks_run() const noexcept { return ticks_run_; }

private:
  [[nodiscard]] result<void> populate();
  [[nodiscard]] result<void> add_systems();

  sim_config config_;
  std::unique_ptr<sched_rig> rig_;
  std::uint64_t ticks_run_ = 0;
};

} // namespace sl::runtime::testing
