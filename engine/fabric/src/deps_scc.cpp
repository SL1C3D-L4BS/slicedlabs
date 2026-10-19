// Cycles, witness paths and SCC condensation (WO-044, ARCH1-R56). Tarjan runs on a dense,
// index-based copy of the masked graph and is iterative, so a deep chain cannot overflow the stack.

#include <algorithm>
#include <cstdint>
#include <format>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "sl/fabric/deps.hpp"

namespace sl::fabric {

namespace {

constexpr std::uint32_t none = std::numeric_limits<std::uint32_t>::max();

struct arc {
  std::uint32_t to = 0;
  edge e;
};

/// The nodes of a graph by index (ascending id) and the edges of the masked kinds, ascending by
/// (target, kind) per node.
struct dense {
  std::vector<record_id> ids;
  std::vector<std::vector<arc>> adj;

  dense(const dependency_graph &g, edge_kind_mask kinds) : ids(g.nodes()), adj(ids.size()) {
    for (std::size_t v = 0; v < ids.size(); ++v) {
      for (const edge &e : g.out(ids[v])) {
        if (contains(kinds, e.kind)) {
          adj[v].push_back(arc{.to = index_of(e.to), .e = e});
        }
      }
      std::ranges::sort(adj[v], [](const arc &a, const arc &b) {
        return a.to != b.to ? a.to < b.to : a.e.kind < b.e.kind;
      });
    }
  }

  [[nodiscard]] std::uint32_t index_of(const record_id &id) const {
    return static_cast<std::uint32_t>(std::ranges::lower_bound(ids, id) - ids.begin());
  }
};

/// Components in numbering order (ascending smallest member); members ascending.
struct sccs {
  std::vector<std::vector<std::uint32_t>> members;
  std::vector<std::uint32_t> comp_of;
};

sccs tarjan(const dense &d) {
  const std::size_t n = d.ids.size();
  std::vector<std::uint32_t> index(n, none);
  std::vector<std::uint32_t> low(n, 0);
  std::vector<bool> on_stack(n, false);
  std::vector<std::uint32_t> stack;
  struct frame {
    std::uint32_t v;
    std::size_t next;
  };
  std::vector<frame> calls;
  std::vector<std::vector<std::uint32_t>> found;
  std::uint32_t counter = 0;

  for (std::uint32_t root = 0; root < n; ++root) {
    if (index[root] != none) {
      continue;
    }
    calls.push_back(frame{.v = root, .next = 0});
    index[root] = low[root] = counter++;
    stack.push_back(root);
    on_stack[root] = true;
    while (!calls.empty()) {
      frame &f = calls.back();
      const std::uint32_t v = f.v;
      if (f.next < d.adj[v].size()) {
        const std::uint32_t w = d.adj[v][f.next++].to;
        if (index[w] == none) {
          index[w] = low[w] = counter++;
          stack.push_back(w);
          on_stack[w] = true;
          calls.push_back(frame{.v = w, .next = 0});
        } else if (on_stack[w]) {
          low[v] = std::min(low[v], index[w]);
        }
        continue;
      }
      if (low[v] == index[v]) {
        std::vector<std::uint32_t> comp;
        std::uint32_t w = none;
        while (w != v) {
          w = stack.back();
          stack.pop_back();
          on_stack[w] = false;
          comp.push_back(w);
        }
        std::ranges::sort(comp);
        found.push_back(std::move(comp));
      }
      calls.pop_back();
      if (!calls.empty()) {
        const std::uint32_t parent = calls.back().v;
        low[parent] = std::min(low[parent], low[v]);
      }
    }
  }
  std::ranges::sort(found, [](const auto &a, const auto &b) { return a.front() < b.front(); });
  sccs out;
  out.comp_of.assign(n, 0);
  for (std::uint32_t c = 0; c < found.size(); ++c) {
    for (const std::uint32_t v : found[c]) {
      out.comp_of[v] = c;
    }
  }
  out.members = std::move(found);
  return out;
}

bool has_self_loop(const dense &d, std::uint32_t v) {
  return std::ranges::any_of(d.adj[v], [v](const arc &a) { return a.to == v; });
}

bool is_cyclic(const dense &d, const std::vector<std::uint32_t> &members) {
  return members.size() > 1 || has_self_loop(d, members.front());
}

} // namespace

std::optional<std::vector<edge>> dependency_graph::find_cycle(edge_kind_mask kinds) const {
  const dense d(*this, kinds);
  const sccs s = tarjan(d);
  for (std::uint32_t c = 0; c < s.members.size(); ++c) {
    if (!is_cyclic(d, s.members[c])) {
      continue;
    }
    // The shortest cycle through the smallest member, by breadth first search in the component.
    const std::uint32_t start = s.members[c].front();
    std::vector<std::uint32_t> parent(d.ids.size(), none);
    std::vector<edge> via(d.ids.size());
    std::vector<bool> seen(d.ids.size(), false);
    std::vector<std::uint32_t> queue{start};
    seen[start] = true;
    for (std::size_t head = 0; head < queue.size(); ++head) {
      const std::uint32_t u = queue[head];
      for (const arc &a : d.adj[u]) {
        if (a.to == start) {
          std::vector<edge> around{a.e};
          for (std::uint32_t v = u; v != start; v = parent[v]) {
            around.push_back(via[v]);
          }
          std::ranges::reverse(around);
          return around;
        }
        if (s.comp_of[a.to] == c && !seen[a.to]) {
          seen[a.to] = true;
          parent[a.to] = u;
          via[a.to] = a.e;
          queue.push_back(a.to);
        }
      }
    }
  }
  return std::nullopt;
}

result<void> dependency_graph::check_acyclic(edge_kind_mask kinds) const {
  for (std::uint8_t n = 1; n <= 5; ++n) {
    const auto kind = static_cast<edge_kind>(n);
    if (!contains(kinds, kind)) {
      continue;
    }
    const auto cycle = find_cycle(mask_of(kind));
    if (!cycle) {
      continue;
    }
    std::string witness = cycle->front().from.to_string();
    for (const edge &e : *cycle) {
      witness += " -> ";
      witness += e.to.to_string();
    }
    return make_error(errc::dependency_cycle,
                      std::format("{} cycle: {}", to_string(kind), witness));
  }
  return {};
}

condensation dependency_graph::condense(edge_kind_mask kinds) const {
  const dense d(*this, kinds);
  const sccs s = tarjan(d);
  condensation out;
  out.components.reserve(s.members.size());
  for (const auto &members : s.members) {
    scc_component comp;
    comp.cyclic = is_cyclic(d, members);
    for (const std::uint32_t v : members) {
      comp.members.push_back(d.ids[v]);
      out.component_of.emplace(d.ids[v], out.components.size());
    }
    out.components.push_back(std::move(comp));
  }
  for (std::uint32_t v = 0; v < d.ids.size(); ++v) {
    for (const arc &a : d.adj[v]) {
      if (s.comp_of[v] != s.comp_of[a.to]) {
        out.edges.emplace_back(s.comp_of[v], s.comp_of[a.to]);
      }
    }
  }
  std::ranges::sort(out.edges);
  auto [dup_first, dup_last] = std::ranges::unique(out.edges);
  out.edges.erase(dup_first, dup_last);

  // Kahn from the components that depend on nothing, smallest index first.
  std::vector<std::size_t> waiting(out.components.size(), 0);
  std::vector<std::vector<std::size_t>> dependents(out.components.size());
  for (const auto &[from, to] : out.edges) {
    ++waiting[from];
    dependents[to].push_back(from);
  }
  std::set<std::size_t> ready;
  for (std::size_t c = 0; c < waiting.size(); ++c) {
    if (waiting[c] == 0) {
      ready.insert(c);
    }
  }
  while (!ready.empty()) {
    const std::size_t c = *ready.begin();
    ready.erase(ready.begin());
    out.order.push_back(c);
    for (const std::size_t dep : dependents[c]) {
      if (--waiting[dep] == 0) {
        ready.insert(dep);
      }
    }
  }
  return out;
}

} // namespace sl::fabric
