#include "sim_fixture.hpp"

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/ecs/command_buffer.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/query.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/platform.hpp"
#include "sl/runtime/replay.hpp"
#include "sl/runtime/time.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace sl::runtime::testing {

namespace {

using ecs::optional;
using ecs::read;
using ecs::write;

constexpr std::size_t energy_slots = 64;

[[nodiscard]] std::uint64_t mix(std::uint64_t x) noexcept {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31U);
}

// A small signed integer in [-span, span] from `x`.
[[nodiscard]] std::int64_t centered(std::uint64_t x, std::uint64_t span) noexcept {
  return static_cast<std::int64_t>(x % ((2 * span) + 1)) - static_cast<std::int64_t>(span);
}

[[nodiscard]] std::uint32_t tick32(const ecs::system_context &ctx) noexcept {
  return static_cast<std::uint32_t>(ctx.tick());
}

// --- apply_inputs ---

struct input_impulse {
  using query_t = ecs::query<write<gen_velocity>, read<gen_aligned>>;
  query_t q;

  void operator()(ecs::system_context &ctx) noexcept {
    const std::uint32_t t = tick32(ctx);
    const result<void> ran =
        ctx.par_for_each(q, [t](std::uint32_t /*batch*/, ecs::entity /*e*/, gen_velocity &v,
                                const gen_aligned &a) noexcept {
          const auto k = static_cast<double>(static_cast<std::int32_t>((a.value + t) % 7U) - 3);
          v.dx += k * 0.01;
          v.dy -= k * 0.005;
        });
    if (!ran && !ctx.stopped()) {
      ctx.fail(ran.error().code);
    }
  }
};

struct padded_mix {
  using query_t = ecs::query<write<gen_padded>>;
  query_t q;

  void operator()(ecs::system_context &ctx) noexcept {
    const std::uint64_t t = ctx.tick();
    ctx.for_each(q, [t](ecs::entity /*e*/, gen_padded &p) noexcept {
      p.b = (p.b * 6364136223846793005ULL) + 1442695040888963407ULL + t;
      p.a = static_cast<std::uint8_t>(p.b >> 56U);
      p.c = static_cast<std::uint16_t>((p.b >> 20U) & 0xFFFFU);
    });
  }
};

// --- pre_tick ---

struct damp {
  using query_t = ecs::query<write<gen_velocity>>;
  query_t q;

  void operator()(ecs::system_context &ctx) noexcept {
    ctx.for_each(q, [](ecs::entity /*e*/, gen_velocity &v) noexcept {
      v.dx = v.dx * 0.99;
      v.dy = (v.dy * 0.99) - 0.001;
    });
  }
};

struct integrate {
  using query_t = ecs::query<write<gen_position>, read<gen_velocity>>;
  query_t q;

  void operator()(ecs::system_context &ctx) noexcept {
    const result<void> ran =
        ctx.par_for_each(q, [](std::uint32_t /*batch*/, ecs::entity /*e*/, gen_position &p,
                               const gen_velocity &v) noexcept {
          p.x += static_cast<float>(v.dx / 50.0);
          p.y += static_cast<float>(v.dy / 50.0);
          if (p.x > 500.0F || p.x < -500.0F) {
            p.x = p.x / 2.0F;
          }
          if (p.y > 500.0F || p.y < -500.0F) {
            p.y = p.y / 2.0F;
          }
          p.z = std::sqrt((p.x * p.x) + (p.y * p.y));
        });
    if (!ran && !ctx.stopped()) {
      ctx.fail(ran.error().code);
    }
  }
};

// --- tick ---

struct health_regen {
  using query_t = ecs::query<write<gen_health>, read<gen_aligned>>;
  query_t q;

  void operator()(ecs::system_context &ctx) noexcept {
    const std::uint32_t t = tick32(ctx);
    const result<void> ran = ctx.par_for_each(q, [t](std::uint32_t /*batch*/, ecs::entity /*e*/,
                                                     gen_health &h, const gen_aligned &a) noexcept {
      h.hp += static_cast<std::int32_t>((a.value + t) % 5U) - 3;
      h.armor = static_cast<std::int16_t>((h.armor + (h.hp % 2 != 0 ? 1 : 0)) % 4);
    });
    if (!ran && !ctx.stopped()) {
      ctx.fail(ran.error().code);
    }
  }
};

struct spawn_row {
  ecs::command_buffer *commands = nullptr;
  std::uint64_t tick = 0;

  void operator()(ecs::entity /*e*/, const gen_padded &p, const gen_position &at) const noexcept {
    if (((p.b >> 8U) + tick) % 61U != 0) {
      return;
    }
    const ecs::pending_entity child =
        commands->spawn_with(gen_position{.x = at.x, .y = at.y, .z = 0.0F},
                             gen_velocity{.dx = static_cast<double>(centered(p.c, 50)) / 100.0,
                                          .dy = static_cast<double>(centered(p.b, 50)) / 100.0});
    commands->add(child, gen_aligned{.value = static_cast<std::uint32_t>(p.b >> 32U)});
    commands->add(child, gen_health{.hp = 40 + static_cast<std::int32_t>(p.c % 40U),
                                    .armor = static_cast<std::int16_t>(p.a % 4U)});
  }
};

struct spawner {
  using query_t = ecs::query<read<gen_padded>, read<gen_position>>;
  query_t q;

  void operator()(ecs::system_context &ctx) noexcept {
    ctx.for_each(q, spawn_row{.commands = &ctx.commands(), .tick = ctx.tick()});
  }
};

// --- post_tick ---

struct reap_row {
  ecs::command_buffer *commands = nullptr;

  void operator()(ecs::entity e, const gen_health &h, const gen_position & /*at*/,
                  const gen_tag *tag) const noexcept {
    if (h.hp <= 0) {
      commands->destroy(e);
    } else if (h.armor % 2 != 0 && tag == nullptr) {
      commands->add(e, gen_tag{});
    } else if (h.armor % 2 == 0 && tag != nullptr) {
      commands->remove<gen_tag>(e);
    }
  }
};

struct reaper {
  using query_t = ecs::query<read<gen_health>, read<gen_position>, optional<const gen_tag>>;
  query_t q;

  void operator()(ecs::system_context &ctx) noexcept {
    ctx.for_each(q, reap_row{.commands = &ctx.commands()});
  }
};

struct energy_sum {
  using query_t = ecs::query<read<gen_position>, read<gen_velocity>>;
  query_t q;
  std::array<det::ordered_buffer<double>, energy_slots> slots{};
  std::array<det::ordered_buffer<double> *, energy_slots> heads{};

  // A batch body: one writer per slot (one job per batch).
  struct row {
    energy_sum *self = nullptr;
    det::ordered_key base;
    std::atomic<bool> *overflow = nullptr;

    void operator()(std::uint32_t batch, ecs::entity /*e*/, const gen_position &p,
                    const gen_velocity &v) const noexcept {
      if (batch >= energy_slots) {
        overflow->store(true);
        return;
      }
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): batch < energy_slots
      det::ordered_buffer<double> &out = self->slots[batch];
      det::ordered_key key = base;
      key.chunk_index = batch;
      key.local_seq = static_cast<std::uint32_t>(out.size());
      const double kinetic = 0.5 * ((v.dx * v.dx) + (v.dy * v.dy));
      const double height = std::sqrt(static_cast<double>((p.x * p.x) + (p.y * p.y) + (p.z * p.z)));
      out.push(key, kinetic + (height / 1000.0));
    }
  };

  struct add_up {
    double *energy = nullptr;
    std::uint64_t *count = nullptr;

    void operator()(const det::ordered_key & /*key*/, const double &value) const noexcept {
      *energy += value;
      ++*count;
    }
  };

  void operator()(ecs::system_context &ctx) noexcept {
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index): i < energy_slots
    for (std::size_t i = 0; i < energy_slots; ++i) {
      slots[i].clear();
      heads[i] = &slots[i];
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)
    std::atomic<bool> overflow{false};
    const det::ordered_key base{.tick = ctx.tick(),
                                .phase = static_cast<std::uint8_t>(ctx.current_phase()),
                                .system_index = ctx.system_index(),
                                .chunk_index = 0,
                                .local_seq = 0};
    const result<void> ran =
        ctx.par_for_each(q, row{.self = this, .base = base, .overflow = &overflow});
    if (!ran) {
      if (!ctx.stopped()) {
        ctx.fail(ran.error().code);
      }
      return;
    }
    if (overflow.load()) {
      ctx.fail(errc::out_of_range);
      return;
    }
    double energy = 0.0;
    std::uint64_t count = 0;
    det::merge_ordered<double>(std::span<det::ordered_buffer<double> *const>(heads),
                               add_up{.energy = &energy, .count = &count});
    auto stats = ctx.resource<sim_stats>(sim_stats_key);
    if (!stats) {
      if (!ctx.stopped()) {
        ctx.fail(stats.error().code);
      }
      return;
    }
    (*stats)->energy = energy;
    (*stats)->population = count;
  }
};

[[nodiscard]] ecs::access_set structural_access() {
  ecs::access_set a;
  a.structural = true;
  return a;
}

[[nodiscard]] ecs::access_set stats_access() {
  ecs::access_set a;
  a.resource_writes.push_back(sim_stats_key);
  return a;
}

// Registers one system whose single query is `body.q`.
template <class Body>
[[nodiscard]] result<void> add_system(ecs::system_scheduler &systems,
                                      const ecs::component_registry &registry,
                                      std::string_view name, phase ph, ecs::access_set access,
                                      const std::vector<std::string_view> &after, bool chunked) {
  auto q = Body::query_t::make(registry);
  if (!q) {
    return std::unexpected(q.error());
  }
  const ecs::component_access needed = q->access();
  ecs::add_query_access(access, needed);
  Body body{.q = std::move(*q)};
  auto id = systems.add(ecs::system_desc{.name = name,
                                         .ph = ph,
                                         .access = std::move(access),
                                         .before = {},
                                         .after = after,
                                         .fn = std::move(body),
                                         .chunked = chunked,
                                         .queries = {needed}});
  if (!id) {
    return std::unexpected(id.error());
  }
  return {};
}

} // namespace

result<std::unique_ptr<jobs::job_system>> sched_rig::start_jobs(platform &plat,
                                                                std::uint32_t workers) {
  jobs::job_config jc;
  jc.workers = workers;
  jc.capacity = 1024;
  return jobs::job_system::start(plat, std::move(jc));
}

result<std::unique_ptr<sched_rig>> sched_rig::make(std::uint32_t workers) {
  auto r = std::make_unique<sched_rig>();
  auto started = start_jobs(r->plat.get(), workers);
  if (!started) {
    return std::unexpected(started.error());
  }
  r->own_jobs = std::move(*started);
  r->js = r->own_jobs.get();
  if (auto rest = r->finish(); !rest) {
    return std::unexpected(rest.error());
  }
  return r;
}

result<std::unique_ptr<sched_rig>> sched_rig::make_on(jobs::job_system &shared) {
  auto r = std::make_unique<sched_rig>();
  r->js = &shared;
  if (auto rest = r->finish(); !rest) {
    return std::unexpected(rest.error());
  }
  return r;
}

result<void> sched_rig::finish() {
  const std::uint32_t workers = js->worker_count();
  arena = std::make_unique<mem::frame_arena>(upstream, workers, mem::frame_arena_block_bytes);
  // Every arena of both sets takes its first block now, on the owner thread, as the gate stack
  // does: a lazy first block on whichever worker first opens a command buffer would land in a
  // steady frame and log the upstream warning (WO-109).
  for (int set = 0; set < 2; ++set) {
    for (std::uint32_t k = 0; k <= workers; ++k) {
      auto p = arena->for_worker(k).try_allocate(1, 1, mem::memory_tag{});
      if (!p) {
        return std::unexpected(p.error());
      }
    }
    arena->flip();
  }
  frame_config fc;
  fc.rate = time::tick_rate{.num = 50, .den = 1};
  auto made = frame_scheduler::make(*js, plat.get(), *arena, fc);
  if (!made) {
    return std::unexpected(made.error());
  }
  frames = std::move(*made);
  auto registered = register_components(registry);
  if (!registered) {
    return std::unexpected(registered.error());
  }
  ids = *registered;
  if (auto frozen = registry.freeze(); !frozen) {
    return std::unexpected(frozen.error());
  }
  auto w = ecs::world::make(registry, world_memory);
  if (!w) {
    return std::unexpected(w.error());
  }
  world = std::move(*w);
  queue = std::make_unique<ecs::command_queue>(registry, *arena);
  systems = std::make_unique<ecs::system_scheduler>(*js);
  return {};
}

result<void> sched_rig::build() const {
  if (auto built = systems->build(*frames, *world, *queue); !built) {
    return built;
  }
  return frames->build();
}

result<frame_stats> sched_rig::step() {
  plat.clock().advance(sched_tick_length);
  return frames->run_frame();
}

result<std::unique_ptr<sim_fixture>> sim_fixture::make(const sim_config &config) {
  auto rig = sched_rig::make(config.workers);
  if (!rig) {
    return std::unexpected(rig.error());
  }
  auto f = std::make_unique<sim_fixture>(passkey{}, config, std::move(*rig));
  if (auto populated = f->populate(); !populated) {
    return std::unexpected(populated.error());
  }
  if (auto added = f->add_systems(); !added) {
    return std::unexpected(added.error());
  }
  f->rig_->systems->set_policy(config.policy);
  if (auto built = f->rig_->build(); !built) {
    return std::unexpected(built.error());
  }
  // The first frame takes the first clock sample and runs no tick (apply_inputs runs); after it
  // every frame runs exactly one tick.
  if (auto primed = f->rig_->step(); !primed) {
    return std::unexpected(primed.error());
  }
  return f;
}

result<void> sim_fixture::populate() {
  ecs::world *const world_ = rig_->world.get();
  if (auto added = world_->add_resource(sim_stats_key, sim_stats{}); !added) {
    return std::unexpected(added.error());
  }
  for (std::uint32_t i = 0; i < config_.entities; ++i) {
    const std::uint64_t r = mix((config_.seed * 0x100000001B3ULL) + i);
    const gen_position at{.x = static_cast<float>(centered(r, 200)),
                          .y = static_cast<float>(centered(r >> 16U, 200)),
                          .z = 0.0F};
    const gen_velocity v{.dx = static_cast<double>(centered(r >> 24U, 100)) / 50.0,
                         .dy = static_cast<double>(centered(r >> 32U, 100)) / 50.0};
    const gen_aligned a{.value = static_cast<std::uint32_t>(r >> 40U)};
    const result<ecs::entity> e =
        i % 2 == 0
            ? world_->create_with(at, v, a,
                                  gen_padded{.a = static_cast<std::uint8_t>(r),
                                             .b = mix(r),
                                             .c = static_cast<std::uint16_t>(r >> 48U)},
                                  gen_tag{})
            : world_->create_with(at, v, a,
                                  gen_health{.hp = 1 + static_cast<std::int32_t>(r % 40U),
                                             .armor = static_cast<std::int16_t>((r >> 8U) % 4U)});
    if (!e) {
      return std::unexpected(e.error());
    }
  }
  return {};
}

result<void> sim_fixture::add_systems() {
  ecs::system_scheduler &s = *rig_->systems;
  const ecs::component_registry &r = rig_->registry;
  // Brace initialisation evaluates left to right: this is the registration order.
  const std::array<result<void>, 8> steps{
      add_system<input_impulse>(s, r, "sim.input_impulse", phase::apply_inputs, {}, {}, true),
      add_system<padded_mix>(s, r, "sim.padded_mix", phase::apply_inputs, {}, {}, false),
      add_system<damp>(s, r, "sim.damp", phase::pre_tick, {}, {"sim.integrate"}, false),
      add_system<integrate>(s, r, "sim.integrate", phase::pre_tick, {}, {}, true),
      add_system<health_regen>(s, r, "sim.health_regen", phase::tick, {}, {}, true),
      add_system<spawner>(s, r, "sim.spawner", phase::tick, structural_access(), {}, false),
      add_system<reaper>(s, r, "sim.reaper", phase::post_tick, structural_access(), {}, false),
      add_system<energy_sum>(s, r, "sim.energy_sum", phase::post_tick, stats_access(), {}, true),
  };
  for (const result<void> &step : steps) {
    // cppcheck-suppress useStlAlgorithm ; the first error is returned as it is
    if (!step) {
      return step;
    }
  }
  return {};
}

result<std::vector<fabric::digest>> sim_fixture::run(std::uint32_t ticks) {
  std::vector<fabric::digest> out;
  out.reserve(ticks);
  for (std::uint32_t i = 0; i < ticks; ++i) {
    auto frame = rig_->step();
    if (!frame) {
      return std::unexpected(frame.error());
    }
    ticks_run_ += frame->ticks_run;
    if (rig_->systems->failures() != 0) {
      return make_error(errc::job_failed, "sim_fixture: a system failed");
    }
    if (!rig_->systems->last_flush()) {
      return std::unexpected(rig_->systems->last_flush().error());
    }
    out.push_back(fixture_hash());
  }
  return out;
}

const sim_stats &sim_fixture::stats() const noexcept {
  static constexpr sim_stats none{};
  auto found = rig_->world->resource<sim_stats>(sim_stats_key);
  return found ? **found : none;
}

fabric::digest sim_fixture::fixture_hash() const {
  det::state_hasher h;
  const fabric::digest world_hash = hash_world(*rig_->world, rig_->ids);
  h.bytes(std::as_bytes(std::span<const std::uint8_t>(world_hash.bytes)));
  const sim_stats &s = stats();
  h.f64(s.energy);
  h.u64(s.population);
  return h.finish();
}

} // namespace sl::runtime::testing
