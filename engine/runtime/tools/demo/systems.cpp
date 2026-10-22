// The lantern-field systems (WO-099, SL-ARCH-003 ARCH3-R169, R092). Math is + - * / and
// std::sqrt only, on f32 and f64 values and integers; every value that enters the state comes
// from the state itself or from the scene clock's step (never from the logical tick, which a
// restore does not rewind). ctx.tick() only keys ordered buffers and event writers.
//   pre_tick   lantern.drift    par_for_each (chunked): x += (phase - 0.5) / 4,
//                               y += ward bias - phase / 8 (the bias sums strength / radius
//                               over the wards), a coordinate beyond 64 is halved,
//                               z = sqrt(x^2 + y^2)
//              lantern.flicker  par_for_each (chunked): phase += 1/32 + energy / 4096 wrapped to
//                               [0, 1), color = color x 1664525 + 1013904223 + age
//   tick       lantern.decay    for_each: energy - 1, age + 1; energy reaching 0 writes
//                               lantern_burnout (the scripted system.fail of the runner)
//              lantern.glow     par_for_each (chunked) into per-batch ordered_buffers merged by
//                               ordered_key into glow_stats; scene_clock.step + 1; every 20 steps
//                               every beacon gets glow = total / (lit + 1) + k / 8 and lit + 1
//   post_tick  lantern.reap     commands: destroys the lanterns of this tick's burnouts
//              lantern.spawn    commands: spawns one lantern per burnout (energy 100)
//   observe    beacon.watch     the change observer: changed<beacon> into watch_stats
//              link.observe     the world_link's (registered by attach)
// reap and spawn are both structural and declare no conflicting access, so they run concurrently
// and their command buffers open in completion order: the canonical playback (merge by
// ordered_key) makes the result independent of that order, which neg-runtime-determinism breaks.

#include "stack.hpp"

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/command_buffer.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/query.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/events.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/replay.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace sl::runtime::demo {

namespace {

using ecs::changed;
using ecs::read;
using ecs::write;

constexpr std::size_t glow_slots = 64;
constexpr float fold_limit = 64.0F;

[[nodiscard]] det::ordered_key base_key(const ecs::system_context &ctx) noexcept {
  return det::ordered_key{.tick = ctx.tick(),
                          .phase = static_cast<std::uint8_t>(ctx.current_phase()),
                          .system_index = ctx.system_index(),
                          .chunk_index = 0,
                          .local_seq = 0};
}

void fail_unless_stopped(ecs::system_context &ctx, errc code) noexcept {
  if (!ctx.stopped()) {
    ctx.fail(code);
  }
}

// --- pre_tick ---

struct drift_row {
  float bias = 0.0F;

  void operator()(std::uint32_t /*batch*/, ecs::entity /*e*/, lantern_pos &p,
                  const lantern_light &l) const noexcept {
    p.x += (l.phase - 0.5F) / 4.0F;
    p.y += bias - (l.phase / 8.0F);
    if (p.x > fold_limit || p.x < -fold_limit) {
      p.x = p.x / 2.0F;
    }
    if (p.y > fold_limit || p.y < -fold_limit) {
      p.y = p.y / 2.0F;
    }
    p.z = std::sqrt((p.x * p.x) + (p.y * p.y));
  }
};

struct ward_sum {
  float *bias = nullptr;

  void operator()(ecs::entity /*e*/, const ward &w) const noexcept {
    if (w.radius > 0.0F) {
      *bias += w.strength / w.radius;
    }
  }
};

struct drift {
  using query_t = ecs::query<write<lantern_pos>, read<lantern_light>>;
  using ward_query_t = ecs::query<read<ward>>;
  query_t q;
  ward_query_t wards;

  void operator()(ecs::system_context &ctx) noexcept {
    float bias = 0.0F;
    ctx.for_each(wards, ward_sum{.bias = &bias});
    // The wards hold 0.5 to 1.5 / 4 to 11 each: the bias stays near the phase term's mean.
    const result<void> ran = ctx.par_for_each(q, drift_row{.bias = bias / 1024.0F});
    if (!ran) {
      fail_unless_stopped(ctx, ran.error().code);
    }
  }
};

struct flicker_row {
  void operator()(std::uint32_t /*batch*/, ecs::entity /*e*/, lantern_light &l,
                  const lantern_life &life) const noexcept {
    l.phase += (1.0F / 32.0F) + (life.energy / 4096.0F);
    if (l.phase >= 1.0F) {
      l.phase -= 1.0F;
    }
    if (l.phase < 0.0F) {
      l.phase += 1.0F;
    }
    l.color = (l.color * 1664525U) + 1013904223U + life.age;
  }
};

struct flicker {
  using query_t = ecs::query<write<lantern_light>, read<lantern_life>>;
  query_t q;

  void operator()(ecs::system_context &ctx) noexcept {
    const result<void> ran = ctx.par_for_each(q, flicker_row{});
    if (!ran) {
      fail_unless_stopped(ctx, ran.error().code);
    }
  }
};

// --- tick ---

struct decay_row {
  event_channel<lantern_burnout> *channel = nullptr;
  writer_key key;
  bool *failed = nullptr;

  void operator()(ecs::entity e, lantern_life &life) const noexcept {
    life.energy -= 1.0F;
    life.age += 1U;
    if (life.energy <= 0.0F && life.energy > -1.0F && !*failed) {
      if (!channel->write(key, lantern_burnout{.index = e.index,
                                               .generation = e.generation,
                                               .energy = life.energy})) {
        *failed = true;
      }
    }
  }
};

struct decay {
  using query_t = ecs::query<write<lantern_life>>;
  query_t q;
  ecs::resource_key channel_key;
  std::atomic<bool> *fail_next = nullptr;

  void operator()(ecs::system_context &ctx) noexcept {
    if (fail_next->exchange(false)) {
      // The runner's scripted system.fail (RFP5's effect, which Release builds compile out).
      ctx.fail(errc::job_failed);
      return;
    }
    auto channel = ctx.resource<event_channel<lantern_burnout>>(channel_key);
    if (!channel) {
      fail_unless_stopped(ctx, channel.error().code);
      return;
    }
    bool failed = false;
    const writer_key key{.tick = ctx.tick(),
                         .ph = ctx.current_phase(),
                         .system_index = ctx.system_index(),
                         .chunk_index = 0};
    ctx.for_each(q, decay_row{.channel = *channel, .key = key, .failed = &failed});
    // cppcheck-suppress knownConditionTrueFalse ; decay_row sets it through its pointer
    if (failed) {
      fail_unless_stopped(ctx, errc::event_payload_invalid);
    }
  }
};

struct glow_slot_row {
  std::array<det::ordered_buffer<double>, glow_slots> *slots = nullptr;
  det::ordered_key base;
  std::atomic<bool> *overflow = nullptr;

  void operator()(std::uint32_t batch, ecs::entity /*e*/, const lantern_life &life,
                  const lantern_light &l) const noexcept {
    if (batch >= glow_slots) {
      overflow->store(true);
      return;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): batch < glow_slots
    det::ordered_buffer<double> &out = (*slots)[batch];
    det::ordered_key key = base;
    key.chunk_index = batch;
    key.local_seq = static_cast<std::uint32_t>(out.size());
    const double energy = life.energy > 0.0F ? static_cast<double>(life.energy) : 0.0;
    out.push(key, (energy / 100.0) * static_cast<double>(l.phase));
  }
};

struct glow_add {
  double *total = nullptr;
  std::uint64_t *lit = nullptr;

  void operator()(const det::ordered_key & /*key*/, const double &v) const noexcept {
    *total += v;
    if (v > 0.25) {
      ++*lit;
    }
  }
};

/// Rewrites the beacons of one window (tune_window rows from `first`, in query order).
struct beacon_tune {
  float base = 0.0F;
  std::uint32_t first = 0;
  std::uint32_t *k = nullptr;

  void operator()(ecs::entity /*e*/, beacon &b) const noexcept {
    const std::uint32_t row = *k;
    ++*k;
    if (row < first || row >= first + tune_window) {
      return;
    }
    b.glow = base + (static_cast<float>(row % 16U) / 8.0F);
    b.lit += 1U;
  }
};

struct glow {
  using query_t = ecs::query<read<lantern_life>, read<lantern_light>>;
  using beacon_query_t = ecs::query<write<beacon>>;
  query_t q;
  beacon_query_t beacons;
  std::array<det::ordered_buffer<double>, glow_slots> slots{};
  std::array<det::ordered_buffer<double> *, glow_slots> heads{};

  void operator()(ecs::system_context &ctx) noexcept {
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index): i < glow_slots
    for (std::size_t i = 0; i < glow_slots; ++i) {
      slots[i].clear();
      heads[i] = &slots[i];
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)
    std::atomic<bool> overflow{false};
    const result<void> ran = ctx.par_for_each(
        q, glow_slot_row{.slots = &slots, .base = base_key(ctx), .overflow = &overflow});
    if (!ran) {
      fail_unless_stopped(ctx, ran.error().code);
      return;
    }
    if (overflow.load()) {
      ctx.fail(errc::out_of_range);
      return;
    }
    double total = 0.0;
    std::uint64_t lit = 0;
    det::merge_ordered<double>(std::span<det::ordered_buffer<double> *const>(heads),
                               glow_add{.total = &total, .lit = &lit});
    auto stats = ctx.resource<glow_stats>(glow_key);
    auto clock = ctx.resource<scene_clock>(clock_key);
    if (!stats || !clock) {
      fail_unless_stopped(ctx, !stats ? stats.error().code : clock.error().code);
      return;
    }
    (*stats)->total = total;
    (*stats)->lit = lit;
    ++(*clock)->step;
    if ((*clock)->step % beacon_every == 0) {
      std::uint32_t k = 0;
      const auto base = static_cast<float>(total / static_cast<double>(lit + 1U));
      const auto window = static_cast<std::uint32_t>((*clock)->step / beacon_every);
      const std::uint32_t first = (window * tune_window) % beacon_records;
      ctx.for_each(beacons, beacon_tune{.base = base, .first = first, .k = &k});
    }
  }
};

// --- post_tick ---

struct reap_event {
  ecs::command_buffer *commands = nullptr;
  std::uint64_t tick = 0;

  void operator()(const event_envelope &env, const lantern_burnout &ev) const noexcept {
    if (env.tick == tick) {
      commands->destroy(ecs::entity{.index = ev.index, .generation = ev.generation});
    }
  }
};

struct reap {
  ecs::resource_key channel_key;

  void operator()(ecs::system_context &ctx) const noexcept {
    auto channel = ctx.resource<const event_channel<lantern_burnout>>(channel_key);
    if (!channel) {
      fail_unless_stopped(ctx, channel.error().code);
      return;
    }
    (*channel)->read(reap_event{.commands = &ctx.commands(), .tick = ctx.tick()});
  }
};

struct spawn_event {
  ecs::command_buffer *commands = nullptr;
  std::uint64_t tick = 0;
  std::uint64_t key = 0;
  std::uint64_t *seq = nullptr;

  void operator()(const event_envelope &env, const lantern_burnout & /*ev*/) const noexcept {
    if (env.tick != tick) {
      return;
    }
    lantern_values v = make_lantern(key + *seq, spawn_energy);
    ++*seq;
    [[maybe_unused]] const ecs::pending_entity child = commands->spawn_with(v.pos, v.light, v.life);
  }
};

struct spawn {
  ecs::resource_key channel_key;

  void operator()(ecs::system_context &ctx) const noexcept {
    auto channel = ctx.resource<const event_channel<lantern_burnout>>(channel_key);
    auto clock = ctx.resource<const scene_clock>(clock_key);
    if (!channel || !clock) {
      fail_unless_stopped(ctx, !channel ? channel.error().code : clock.error().code);
      return;
    }
    // A spawn key per step: far from the creation indices 0..entities.
    const std::uint64_t key = mix(((*clock)->seed * 0x100000001B3ULL) ^ (*clock)->step) << 20U;
    std::uint64_t seq = 0;
    (*channel)->read(
        spawn_event{.commands = &ctx.commands(), .tick = ctx.tick(), .key = key, .seq = &seq});
  }
};

// --- observe ---

struct watch_row {
  watch_stats *out = nullptr;

  void operator()(ecs::entity /*e*/, const beacon &b) const noexcept {
    ++out->changes;
    out->glow += static_cast<double>(b.glow);
  }
};

struct watch {
  using query_t = ecs::query<read<beacon>, changed<beacon>>;
  query_t q;

  void operator()(ecs::system_context &ctx) noexcept {
    auto stats = ctx.resource<watch_stats>(watch_key);
    if (!stats) {
      fail_unless_stopped(ctx, stats.error().code);
      return;
    }
    ctx.for_each(q, watch_row{.out = *stats});
  }
};

// --- registration ---

[[nodiscard]] result<void> add(scene_stack &s, std::string_view name, phase ph,
                               ecs::access_set access,
                               const std::vector<ecs::component_access> &queries, ecs::system_fn fn,
                               bool chunked) {
  for (const ecs::component_access &q : queries) {
    ecs::add_query_access(access, q);
  }
  auto id = s.systems->add(ecs::system_desc{.name = name,
                                            .ph = ph,
                                            .access = std::move(access),
                                            .before = {},
                                            .after = {},
                                            .fn = std::move(fn),
                                            .chunked = chunked,
                                            .queries = queries});
  if (!id) {
    return std::unexpected(id.error());
  }
  if (s.system_names.size() <= *id) {
    s.system_names.resize(std::size_t{*id} + 1U);
  }
  s.system_names[*id] = std::string(name);
  return {};
}

template <class Q> [[nodiscard]] result<Q> make_query(const scene_stack &s) {
  return Q::make(s.components);
}

} // namespace

result<void> add_scene_systems(scene_stack &s) {
  const ecs::resource_key channel_key = event_registry::channel_key(s.channel->type());

  auto drift_q = make_query<drift::query_t>(s);
  auto ward_q = make_query<drift::ward_query_t>(s);
  auto flicker_q = make_query<flicker::query_t>(s);
  auto decay_q = make_query<decay::query_t>(s);
  auto glow_q = make_query<glow::query_t>(s);
  auto beacon_q = make_query<glow::beacon_query_t>(s);
  auto watch_q = make_query<watch::query_t>(s);
  if (!drift_q || !ward_q || !flicker_q || !decay_q || !glow_q || !beacon_q || !watch_q) {
    return make_error(errc::internal, "lantern field: a query does not build");
  }

  ecs::access_set decay_access;
  decay_access.resource_writes.push_back(channel_key);
  ecs::access_set glow_access;
  glow_access.resource_writes = {clock_key, glow_key};
  ecs::access_set reap_access;
  reap_access.structural = true;
  reap_access.resource_reads.push_back(channel_key);
  ecs::access_set spawn_access;
  spawn_access.structural = true;
  spawn_access.resource_reads = {clock_key, channel_key};
  ecs::access_set watch_access;
  watch_access.resource_writes.push_back(watch_key);

  const std::vector<ecs::component_access> drift_needs{drift_q->access(), ward_q->access()};
  const std::vector<ecs::component_access> flicker_needs{flicker_q->access()};
  const std::vector<ecs::component_access> decay_needs{decay_q->access()};
  const std::vector<ecs::component_access> glow_needs{glow_q->access(), beacon_q->access()};
  const std::vector<ecs::component_access> watch_needs{watch_q->access()};

  // Brace initialisation evaluates left to right: this is the registration order.
  const std::array<result<void>, 7> steps{
      add(s, "lantern.drift", phase::pre_tick, {}, drift_needs,
          drift{.q = std::move(*drift_q), .wards = std::move(*ward_q)}, true),
      add(s, "lantern.flicker", phase::pre_tick, {}, flicker_needs,
          flicker{.q = std::move(*flicker_q)}, true),
      add(s, "lantern.decay", phase::tick, decay_access, decay_needs,
          decay{.q = std::move(*decay_q), .channel_key = channel_key, .fail_next = &s.fail_next},
          false),
      add(s, "lantern.glow", phase::tick, glow_access, glow_needs,
          glow{.q = std::move(*glow_q), .beacons = std::move(*beacon_q)}, true),
      add(s, "lantern.reap", phase::post_tick, reap_access, {}, reap{.channel_key = channel_key},
          false),
      add(s, "lantern.spawn", phase::post_tick, spawn_access, {}, spawn{.channel_key = channel_key},
          false),
      add(s, "beacon.watch", phase::observe, watch_access, watch_needs,
          watch{.q = std::move(*watch_q)}, false),
  };
  for (const result<void> &step : steps) {
    // cppcheck-suppress useStlAlgorithm ; the first error is returned as it is
    if (!step) {
      return step;
    }
  }
  return {};
}

} // namespace sl::runtime::demo
