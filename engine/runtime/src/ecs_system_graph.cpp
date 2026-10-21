// System graphs (WO-094, SL-ARCH-003 ARCH3-R122 to R124, ADR-0024, ADR-0026): build orders every
// phase by Kahn's algorithm over the before/after constraints, ties by registration order
// (system_index = the position), reports system_unknown and schedule_cycle (every system on a
// cycle), derives the conflict edges (transitively reduced; at most job_max_dependencies per task,
// an excess predecessor moving onto a later one) and registers one frame task per system and the
// phase end hook.

#include "sl/core/error.hpp"
#include "sl/runtime/ecs/command_buffer.hpp"
#include "sl/runtime/ecs/query.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/frame.hpp"
#include "sl/runtime/jobs.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <queue>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sl::runtime::ecs {

namespace {

// True when two ascending lists share an element.
template <class T> [[nodiscard]] bool meets(std::span<const T> a, std::span<const T> b) noexcept {
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < a.size() && j < b.size()) {
    if (a[i] < b[j]) {
      ++i;
    } else if (b[j] < a[i]) {
      ++j;
    } else {
      return true;
    }
  }
  return false;
}

// True when `x` writes what `y` reads or writes.
template <class T>
[[nodiscard]] bool writes_into(const std::vector<T> &x_writes, const std::vector<T> &y_reads,
                               const std::vector<T> &y_writes) noexcept {
  return meets<T>(x_writes, y_reads) || meets<T>(x_writes, y_writes);
}

using phase_bits = std::bitset<max_systems_per_phase>;

// The members of every cycle of the subgraph `alive` of `next`: a node is on a cycle when it can
// reach itself.
[[nodiscard]] std::vector<std::size_t>
cycle_members(const std::vector<std::vector<std::size_t>> &next, const std::vector<bool> &alive) {
  std::vector<std::size_t> out;
  for (std::size_t v = 0; v < next.size(); ++v) {
    if (!alive[v]) {
      continue;
    }
    std::vector<bool> seen(next.size(), false);
    std::vector<std::size_t> stack(next[v].begin(), next[v].end());
    bool cyclic = false;
    while (!stack.empty() && !cyclic) {
      const std::size_t u = stack.back();
      stack.pop_back();
      if (!alive[u] || seen[u]) {
        continue;
      }
      if (u == v) {
        cyclic = true;
        break;
      }
      seen[u] = true;
      stack.insert(stack.end(), next[u].begin(), next[u].end());
    }
    if (cyclic) {
      out.push_back(v);
    }
  }
  return out;
}

} // namespace

namespace detail {

std::string_view phase_name(phase ph) noexcept {
  switch (ph) {
  case phase::begin_frame:
    return "begin_frame";
  case phase::apply_inputs:
    return "apply_inputs";
  case phase::pre_tick:
    return "pre_tick";
  case phase::tick:
    return "tick";
  case phase::post_tick:
    return "post_tick";
  case phase::observe:
    return "observe";
  case phase::present:
    return "present";
  case phase::end_frame:
    return "end_frame";
  }
  return "unknown";
}

} // namespace detail

result<std::vector<std::uint32_t>> system_scheduler::order_phase(phase ph) const {
  std::vector<std::uint32_t> members; // ids in registration order
  for (const detail::system_record &s : systems_) {
    if (s.ph == ph) {
      members.push_back(s.id);
    }
  }
  const std::size_t n = members.size();
  const auto local_of = [&](std::string_view name) noexcept -> std::optional<std::size_t> {
    for (std::size_t i = 0; i < n; ++i) {
      // cppcheck-suppress useStlAlgorithm ; the local index, not the element, is returned
      if (systems_[members[i]].name == name) {
        return i;
      }
    }
    return std::nullopt;
  };
  const auto unknown = [&](const detail::system_record &s, const std::string &name) {
    const bool elsewhere = std::ranges::any_of(
        systems_, [&name](const detail::system_record &o) noexcept { return o.name == name; });
    return make_error(errc::system_unknown,
                      "system_scheduler::build: " + s.name + " names " + name + ", " +
                          (elsewhere ? "a system of another phase" : "no system"));
  };
  std::vector<std::vector<std::size_t>> next(n); // a -> b: a runs before b
  std::vector<std::uint32_t> indegree(n, 0);
  for (std::size_t i = 0; i < n; ++i) {
    const detail::system_record &s = systems_[members[i]];
    for (const std::string &name : s.after) {
      const std::optional<std::size_t> j = local_of(name);
      if (!j) {
        return unknown(s, name);
      }
      next[*j].push_back(i);
      ++indegree[i];
    }
    for (const std::string &name : s.before) {
      const std::optional<std::size_t> j = local_of(name);
      if (!j) {
        return unknown(s, name);
      }
      next[i].push_back(*j);
      ++indegree[*j];
    }
  }
  // Kahn's algorithm; the ready system registered first goes next (ARCH3-R123).
  std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<>> ready;
  for (std::size_t i = 0; i < n; ++i) {
    if (indegree[i] == 0) {
      ready.push(i);
    }
  }
  std::vector<std::uint32_t> ordered;
  ordered.reserve(n);
  std::vector<bool> alive(n, true);
  while (!ready.empty()) {
    const std::size_t i = ready.top();
    ready.pop();
    ordered.push_back(members[i]);
    alive[i] = false;
    for (const std::size_t j : next[i]) {
      if (--indegree[j] == 0) {
        ready.push(j);
      }
    }
  }
  if (ordered.size() < n) {
    std::string names;
    for (const std::size_t v : cycle_members(next, alive)) {
      names += names.empty() ? "" : ", ";
      names += systems_[members[v]].name;
    }
    return make_error(errc::schedule_cycle, "system_scheduler::build: a constraint cycle in " +
                                                std::string(detail::phase_name(ph)) + ": " + names);
  }
  return ordered;
}

bool system_scheduler::conflict(std::uint32_t a, std::uint32_t b) const noexcept {
  const access_set &x = systems_[a].access;
  const access_set &y = systems_[b].access;
  if (x.exclusive || y.exclusive) {
    return true;
  }
  return writes_into(x.components.writes, y.components.reads, y.components.writes) ||
         writes_into(y.components.writes, x.components.reads, x.components.writes) ||
         writes_into(x.resource_writes, y.resource_reads, y.resource_writes) ||
         writes_into(y.resource_writes, x.resource_reads, x.resource_writes);
}

void system_scheduler::plan_edges(std::span<const std::uint32_t> ordered) {
  const std::size_t n = ordered.size();
  std::vector<phase_bits> ancestors(n);
  std::vector<std::vector<std::uint16_t>> kept(n);
  for (std::size_t j = 0; j < n; ++j) {
    // Nearest conflicting predecessors first: one already reached through a kept edge is
    // implied (transitive reduction).
    phase_bits covered;
    for (std::size_t i = j; i-- > 0;) {
      if (covered.test(i) || !conflict(ordered[i], ordered[j])) {
        continue;
      }
      kept[j].push_back(static_cast<std::uint16_t>(i));
      covered |= ancestors[i];
      covered.set(i);
    }
    ancestors[j] = covered;
  }
  // A frame task takes at most job_max_dependencies after ids: an excess earliest predecessor p
  // becomes a predecessor of the next one q (p < q < j), which keeps j after p.
  bool moved = true;
  while (moved) {
    moved = false;
    for (std::size_t j = 0; j < n; ++j) {
      while (kept[j].size() > jobs::job_max_dependencies) {
        std::ranges::sort(kept[j]);
        const std::uint16_t first = kept[j][0];
        const std::uint16_t second = kept[j][1];
        kept[j].erase(kept[j].begin());
        std::vector<std::uint16_t> &into = kept[second];
        if (std::ranges::find(into, first) == into.end()) {
          into.push_back(first);
          moved = true;
        }
      }
    }
  }
  for (std::size_t j = 0; j < n; ++j) {
    std::ranges::sort(kept[j]);
    systems_[ordered[j]].edges = std::move(kept[j]);
  }
}

result<void> system_scheduler::build(frame_scheduler &frames, world &w,
                                     command_queue &commands) noexcept {
  if (!is_owner()) {
    return make_error(errc::wrong_thread, "system_scheduler::build off the owner thread");
  }
  if (built_) {
    return {};
  }
  std::array<std::vector<std::uint32_t>, phase_count> orders;
  for (std::size_t p = 0; p < phase_count; ++p) {
    auto ordered = order_phase(static_cast<phase>(p));
    if (!ordered) {
      return std::unexpected(ordered.error());
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): p < phase_count
    orders[p] = std::move(*ordered);
  }
  for (const std::vector<std::uint32_t> &ordered : orders) {
    for (std::size_t i = 0; i < ordered.size(); ++i) {
      systems_[ordered[i]].index = static_cast<std::uint16_t>(i);
    }
    plan_edges(ordered);
  }
  world_ = &w;
  queue_ = &commands;
  order_ = orders;
  std::vector<std::uint32_t> task_of(systems_.size(), 0);
  for (const std::vector<std::uint32_t> &ordered : orders) {
    for (const std::uint32_t id : ordered) {
      const detail::system_record &s = systems_[id];
      std::vector<std::uint32_t> after;
      after.reserve(s.edges.size());
      for (const std::uint16_t e : s.edges) {
        // cppcheck-suppress useStlAlgorithm ; two lookups per edge read plainer as a loop
        after.push_back(task_of[ordered[e]]);
      }
      auto task = frames.add_task(task_desc{.name = std::string_view{s.name},
                                            .ph = s.ph,
                                            .fn = system_task{.self = this, .slot = id},
                                            .after = std::move(after)});
      if (!task) {
        return std::unexpected(task.error());
      }
      task_of[id] = *task;
    }
  }
  frames.add_phase_end_hook(phase_hook{.self = this});
  built_ = true;
  return {};
}

} // namespace sl::runtime::ecs
