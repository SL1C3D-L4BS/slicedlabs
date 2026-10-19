// Reverse closure and the impact summary (WO-044, ARCH1-R58).

#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "sl/fabric/deps.hpp"

namespace sl::fabric {

result<impact_set> dependency_graph::reverse_closure(std::span<const record_id> changed,
                                                     const closure_options &options) const {
  std::vector<record_id> seeds(changed.begin(), changed.end());
  for (const record_id &id : seeds) {
    if (!is_valid(id.cls()) || !is_valid(id.scheme())) {
      return make_error(errc::invalid_argument,
                        std::format("changed id {} is not a valid record id", id.to_string()));
    }
  }
  std::ranges::sort(seeds);
  auto [dup_first, dup_last] = std::ranges::unique(seeds);
  seeds.erase(dup_first, dup_last);

  impact_set set;
  set.at_revision = options.at_revision;
  const auto full = [&] { return options.max_nodes != 0 && set.nodes.size() >= options.max_nodes; };

  std::set<record_id> seen;
  std::vector<record_id> level;
  for (const record_id &id : seeds) {
    if (full()) {
      set.truncated = true;
      return set;
    }
    set.nodes.push_back(impact_node{
        .id = id, .cls = id.cls(), .distance = 0, .via = {}, .via_kind = edge_kind::reference});
    seen.insert(id);
    level.push_back(id);
  }

  struct found {
    record_id via;
    edge_kind kind = edge_kind::reference;
  };
  std::uint32_t distance = 0;
  while (!level.empty()) {
    if (options.max_distance && distance >= *options.max_distance) {
      break;
    }
    // Nodes of the level and their in-edges both ascend, so the first finder of a dependent is
    // its smallest via, then its smallest kind.
    std::map<record_id, found> next;
    for (const record_id &n : level) {
      ++set.visited;
      for (const edge &e : in(n)) {
        if (!contains(options.kinds, e.kind)) {
          continue;
        }
        ++set.edges_examined;
        if (!seen.contains(e.from)) {
          next.emplace(e.from, found{.via = n, .kind = e.kind});
        }
      }
    }
    ++distance;
    level.clear();
    for (const auto &[id, f] : next) {
      if (full()) {
        set.truncated = true;
        return set;
      }
      set.nodes.push_back(impact_node{
          .id = id, .cls = id.cls(), .distance = distance, .via = f.via, .via_kind = f.kind});
      seen.insert(id);
      level.push_back(id);
    }
  }
  return set;
}

impact_summary summarize(const impact_set &set, std::size_t first_n) {
  impact_summary out;
  out.truncated = set.truncated;
  std::map<id_class, class_impact> by_class;
  for (const impact_node &n : set.nodes) {
    class_impact &c = by_class[n.cls];
    c.cls = n.cls;
    ++c.count;
    if (c.first.size() < first_n) {
      c.first.push_back(n.id);
    }
    ++out.total;
  }
  for (auto &[cls, c] : by_class) {
    out.classes.push_back(std::move(c));
  }
  return out;
}

} // namespace sl::fabric
