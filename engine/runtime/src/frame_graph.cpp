// Phase graphs (WO-089, SL-ARCH-003 ARCH3-R094, R095, R097, R100, R066): add_task, build (once:
// the per-phase task lists, local positions and critical paths) and the run of one phase: its
// tasks submitted as frame jobs with their after-edges, the wait (helping), the main-affinity
// drain and the phase end hooks.

#include "sl/core/assert.hpp"
#include "sl/core/error.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/jobs.hpp"
#include "sl/runtime/sync.hpp"
#include "sl/runtime/time.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sl::runtime {

namespace {

// The phases of the `phases` table with systems: true.
[[nodiscard]] bool takes_tasks(phase ph) noexcept {
  switch (ph) {
  case phase::apply_inputs:
  case phase::pre_tick:
  case phase::tick:
  case phase::post_tick:
  case phase::observe:
    return true;
  case phase::begin_frame:
  case phase::present:
  case phase::end_frame:
    return false;
  }
  return false;
}

} // namespace

result<std::uint32_t> frame_scheduler::add_task(task_desc desc) noexcept {
  if (!is_owner()) {
    return make_error(errc::wrong_thread, "frame_scheduler::add_task off the owner thread");
  }
  if (built_) {
    return make_error(errc::schedule_frozen, "frame_scheduler::add_task after build");
  }
  if (!takes_tasks(desc.ph)) {
    return make_error(errc::invalid_argument, "frame_scheduler::add_task: a phase without tasks");
  }
  if (!desc.fn) {
    return make_error(errc::invalid_argument, "frame_scheduler::add_task: empty task body");
  }
  if (desc.after.size() > jobs::job_max_dependencies) {
    return make_error(errc::invalid_argument, "frame_scheduler::add_task: more than 8 after ids");
  }
  for (const std::uint32_t a : desc.after) {
    if (a >= tasks_.size()) {
      return make_error(errc::invalid_argument, "frame_scheduler::add_task: unknown after id");
    }
    if (tasks_[a].ph != desc.ph) {
      return make_error(errc::invalid_argument, "frame_scheduler::add_task: cross-phase edge");
    }
  }
  const auto in_phase = std::ranges::count_if(
      tasks_, [ph = desc.ph](const task_record &t) noexcept { return t.ph == ph; });
  if (std::cmp_greater_equal(in_phase, max_tasks_per_phase)) {
    return make_error(errc::out_of_range, "frame_scheduler::add_task: max_tasks_per_phase reached");
  }
  const auto id = static_cast<std::uint32_t>(tasks_.size());
  tasks_.push_back(task_record{.name = std::string(desc.name),
                               .ph = desc.ph,
                               .fn = std::move(desc.fn),
                               .after = std::move(desc.after),
                               .local = 0});
  return id;
}

result<void> frame_scheduler::build() noexcept {
  if (!is_owner()) {
    return make_error(errc::wrong_thread, "frame_scheduler::build off the owner thread");
  }
  if (built_) {
    return {};
  }
  // Task ids are in add order and after ids name earlier tasks, so add order is topological.
  std::vector<std::uint32_t> depth(tasks_.size(), 0);
  std::size_t widest = 0;
  for (std::uint32_t id = 0; id < tasks_.size(); ++id) {
    task_record &t = tasks_[id];
    phase_graph &g = graphs_[static_cast<std::size_t>(t.ph)];
    t.local = static_cast<std::uint32_t>(g.tasks.size());
    g.tasks.push_back(id);
    std::uint32_t d = 1;
    for (const std::uint32_t a : t.after) {
      // cppcheck-suppress useStlAlgorithm ; a max over a short id list reads plainer as a loop
      d = std::max(d, depth[a] + 1);
    }
    depth[id] = d;
    g.critical_path = std::max(g.critical_path, d);
    widest = std::max(widest, g.tasks.size());
  }
  handles_.assign(widest, jobs::job_handle{});
  built_ = true;
  return {};
}

void frame_scheduler::frame_job::operator()(jobs::job_context &ctx) const noexcept {
  self->run_task(task, ctx);
}

void frame_scheduler::run_task(std::uint32_t task, jobs::job_context &ctx) noexcept {
  task_context tc{.job = ctx, .time = current_, .task = task, .tick = current_.now};
  tasks_[task].fn(tc);
}

void frame_scheduler::run_phase(phase ph, frame_stats &stats) noexcept {
  const nanos start = platform_->clock.now();
  const phase_graph &g = graphs_[static_cast<std::size_t>(ph)];
  for (std::size_t i = 0; i < g.tasks.size(); ++i) {
    const std::uint32_t id = g.tasks[i];
    const task_record &t = tasks_[id];
    std::array<jobs::dependency, jobs::job_max_dependencies> storage{};
    const std::span<jobs::dependency> deps(storage);
    std::size_t n = 0;
    for (const std::uint32_t a : t.after) {
      deps[n++] = jobs::dependency::on(handles_[tasks_[a].local]);
    }
    handles_[i] = jobs_->submit(jobs::job_desc{.fn = frame_job{.self = this, .task = id},
                                               .prio = jobs::priority::frame,
                                               .aff = jobs::affinity::any,
                                               .name = std::string_view{t.name}},
                                deps.first(n), &frame_jobs_);
  }
  // ARCH3-R095: every job of the phase completes before the boundary.
  const result<void> waited = jobs_->wait(frame_jobs_);
  SL_DEBUG_ASSERT(waited.has_value(), "frame_scheduler: a counter wait cannot fail");
  finish_phase(ph, start, stats);
}

void frame_scheduler::finish_phase(phase ph, nanos start, frame_stats &stats) noexcept {
  jobs_->run_main_affinity(); // ARCH3-R066: drained at the phase boundary
  const time::tick now = logical_.now();
  for (const phase_end_hook &hook : phase_end_hooks_) {
    hook(ph, now);
  }
  record_phase(stats, ph, start);
}

} // namespace sl::runtime
