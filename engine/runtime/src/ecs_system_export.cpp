// The schedule export (WO-094, SL-ARCH-003 ARCH3-R126): sl-runtime-schedule/1
// (schemas/runtime/schedule.schema.json) with two-space indentation, keys in schema order, phases
// in `phases` table order (only those with systems), systems in system_index order, component
// names in ascending component id order, resource keys ascending and after-edges as ascending
// system indices. Nothing depends on threads, workers or addresses, so the bytes are identical
// across runs and worker counts.

#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/system.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/frame.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sl::runtime::ecs {

namespace {

void indent(std::string &out, std::size_t depth) { out.append(depth * 2, ' '); }

void quoted(std::string &out, std::string_view text) {
  static constexpr std::string_view hex = "0123456789abcdef";
  out += '"';
  for (const char ch : text) {
    const auto u = static_cast<unsigned char>(ch);
    if (ch == '"' || ch == '\\') {
      out += '\\';
      out += ch;
    } else if (u < 0x20U) {
      out += "\\u00";
      out += hex[u >> 4U];
      out += hex[u & 0xFU];
    } else {
      out += ch;
    }
  }
  out += '"';
}

// A list of already rendered JSON values: [] when empty, one value per line otherwise.
void list(std::string &out, std::span<const std::string> values, std::size_t depth) {
  if (values.empty()) {
    out += "[]";
    return;
  }
  out += "[\n";
  for (std::size_t i = 0; i < values.size(); ++i) {
    indent(out, depth + 1);
    out += values[i];
    out += i + 1 < values.size() ? ",\n" : "\n";
  }
  indent(out, depth);
  out += ']';
}

[[nodiscard]] std::vector<std::string> names(const component_registry &registry,
                                             std::span<const component_id> ids) {
  std::vector<std::string> out;
  out.reserve(ids.size());
  for (const component_id c : ids) {
    std::string q;
    quoted(q, registry.info(c).name);
    out.push_back(std::move(q));
  }
  return out;
}

template <class T, class Key>
[[nodiscard]] std::vector<std::string> numbers(std::span<const T> values, Key key) {
  std::vector<std::string> out;
  out.reserve(values.size());
  for (const T &v : values) {
    // cppcheck-suppress useStlAlgorithm ; a plain loop over a short list reads plainer
    out.push_back(std::to_string(key(v)));
  }
  return out;
}

struct resource_value {
  std::uint32_t operator()(resource_key k) const noexcept { return k.value; }
};

struct edge_value {
  std::uint32_t operator()(std::uint16_t e) const noexcept { return e; }
};

void field(std::string &out, std::size_t depth, std::string_view key) {
  indent(out, depth);
  quoted(out, key);
  out += ": ";
}

} // namespace

std::string system_scheduler::export_json() const {
  std::string out = "{\n";
  field(out, 1, "format");
  out += "\"sl-runtime-schedule/1\",\n";
  field(out, 1, "policy");
  out +=
      policy_ == execution_policy::serial_canonical ? "\"serial_canonical\",\n" : "\"parallel\",\n";
  field(out, 1, "phases");
  std::vector<std::size_t> used;
  if (built_) {
    for (std::size_t p = 0; p < phase_count; ++p) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): p < phase_count
      if (!order_[p].empty()) {
        // cppcheck-suppress useStlAlgorithm ; indices, not elements, are collected
        used.push_back(p);
      }
    }
  }
  if (used.empty()) {
    out += "[]\n}\n";
    return out;
  }
  out += "[\n";
  for (std::size_t u = 0; u < used.size(); ++u) {
    const std::size_t p = used[u];
    indent(out, 2);
    out += "{\n";
    field(out, 3, "phase");
    quoted(out, detail::phase_name(static_cast<phase>(p)));
    out += ",\n";
    field(out, 3, "systems");
    out += "[\n";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): p < phase_count (used)
    const std::vector<std::uint32_t> &ids = order_[p];
    for (std::size_t i = 0; i < ids.size(); ++i) {
      const detail::system_record &s = systems_[ids[i]];
      indent(out, 4);
      out += "{\n";
      field(out, 5, "index");
      out += std::to_string(s.index);
      out += ",\n";
      field(out, 5, "name");
      quoted(out, s.name);
      out += ",\n";
      field(out, 5, "access");
      out += "{\n";
      const component_registry &registry = world_->registry();
      field(out, 6, "component_reads");
      list(out, names(registry, s.access.components.reads), 6);
      out += ",\n";
      field(out, 6, "component_writes");
      list(out, names(registry, s.access.components.writes), 6);
      out += ",\n";
      field(out, 6, "resource_reads");
      list(out, numbers<resource_key>(s.access.resource_reads, resource_value{}), 6);
      out += ",\n";
      field(out, 6, "resource_writes");
      list(out, numbers<resource_key>(s.access.resource_writes, resource_value{}), 6);
      out += ",\n";
      field(out, 6, "structural");
      out += s.access.structural ? "true,\n" : "false,\n";
      field(out, 6, "exclusive");
      out += s.access.exclusive ? "true\n" : "false\n";
      indent(out, 5);
      out += "},\n";
      field(out, 5, "after");
      list(out, numbers<std::uint16_t>(s.edges, edge_value{}), 5);
      out += '\n';
      indent(out, 4);
      out += i + 1 < ids.size() ? "},\n" : "}\n";
    }
    indent(out, 3);
    out += "]\n";
    indent(out, 2);
    out += u + 1 < used.size() ? "},\n" : "}\n";
  }
  indent(out, 1);
  out += "]\n}\n";
  return out;
}

} // namespace sl::runtime::ecs
