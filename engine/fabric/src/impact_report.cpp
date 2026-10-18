// Impact reports as values, derivation pins, the F4 currency check over explicit inputs and the
// default edge rule (WO-056, SL-ARCH-001 ARCH1-R55, ARCH1-R58, ARCH1-R59).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/deps.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/impact.hpp"
#include "sl/fabric/provenance.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/tree.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

namespace {

// ---- encoding helpers -------------------------------------------------------------------------

result<value> id_list(std::span<const record_id> ids) {
  std::vector<value> items;
  items.reserve(ids.size());
  for (const record_id &id : ids) {
    items.push_back(value::id(id));
  }
  return value::list(std::move(items));
}

result<value> class_value(const class_impact &c) {
  auto ids = id_list(c.first);
  if (!ids) {
    return std::unexpected(std::move(ids.error()));
  }
  return value::struct_({field{.no = 1, .v = value::uint(static_cast<std::uint64_t>(c.cls))},
                         field{.no = 2, .v = value::uint(c.count)},
                         field{.no = 3, .v = std::move(*ids)}});
}

result<value> role_value(const role_impact &r) {
  auto ids = id_list(r.first);
  if (!ids) {
    return std::unexpected(std::move(ids.error()));
  }
  return value::struct_({field{.no = 1, .v = value::uint(static_cast<std::uint64_t>(r.role))},
                         field{.no = 2, .v = value::id(r.type.id())},
                         field{.no = 3, .v = value::uint(r.count)},
                         field{.no = 4, .v = std::move(*ids)}});
}

// ---- decoding helpers -------------------------------------------------------------------------

error mismatch(std::string_view what) {
  return make_error(errc::type_mismatch, std::format("impact report: {}", what)).error();
}

result<const value *> required(const value &v, std::uint32_t no, value_kind kind,
                               std::string_view name) {
  const value *f = v.find_field(no);
  if (f == nullptr) {
    return std::unexpected(mismatch(std::format("field {} ({}) is missing", no, name)));
  }
  if (f->kind() != kind) {
    return std::unexpected(mismatch(std::format("field {} ({}) is {}, not {}", no, name,
                                                to_string(f->kind()), to_string(kind))));
  }
  return f;
}

result<std::uint64_t> uint_field(const value &v, std::uint32_t no, std::string_view name) {
  auto f = required(v, no, value_kind::uint, name);
  if (!f) {
    return std::unexpected(std::move(f.error()));
  }
  return (*f)->as_uint();
}

result<std::vector<record_id>> ids_field(const value &v, std::uint32_t no, std::string_view name) {
  auto f = required(v, no, value_kind::list, name);
  if (!f) {
    return std::unexpected(std::move(f.error()));
  }
  std::vector<record_id> out;
  out.reserve((*f)->items().size());
  for (const value &item : (*f)->items()) {
    if (item.kind() != value_kind::id) {
      return std::unexpected(mismatch(std::format("{} holds a {}", name, to_string(item.kind()))));
    }
    auto id = item.as_record_id();
    if (!id) {
      return std::unexpected(std::move(id.error()));
    }
    out.push_back(*id);
  }
  return out;
}

// The first error among several results, if any.
template <class... R> std::optional<error> first_error(const R &...rs) {
  std::optional<error> out;
  ((out || rs.has_value() ? void() : void(out = rs.error())), ...);
  return out;
}

result<class_impact> class_from(const value &v) {
  if (v.kind() != value_kind::struct_) {
    return std::unexpected(mismatch("a class entry is not a struct"));
  }
  auto cls = uint_field(v, 1, "class");
  auto count = uint_field(v, 2, "count");
  auto ids = ids_field(v, 3, "class ids");
  if (auto e = first_error(cls, count, ids)) {
    return std::unexpected(std::move(*e));
  }
  if (*cls > 0xFF || !is_valid(static_cast<id_class>(*cls))) {
    return std::unexpected(mismatch(std::format("id class {} is not assigned", *cls)));
  }
  return class_impact{
      .cls = static_cast<id_class>(*cls), .count = *count, .first = std::move(*ids)};
}

result<role_impact> role_from(const value &v) {
  if (v.kind() != value_kind::struct_) {
    return std::unexpected(mismatch("a role entry is not a struct"));
  }
  auto role = uint_field(v, 1, "role");
  auto type = required(v, 2, value_kind::id, "type");
  auto count = uint_field(v, 3, "count");
  auto ids = ids_field(v, 4, "role ids");
  if (auto e = first_error(role, type, count, ids)) {
    return std::unexpected(std::move(*e));
  }
  if (*role != static_cast<std::uint64_t>(impact_role::test) &&
      *role != static_cast<std::uint64_t>(impact_role::build_target)) {
    return std::unexpected(mismatch(std::format("role {} is unknown", *role)));
  }
  auto raw = (*type)->as_record_id();
  if (!raw) {
    return std::unexpected(std::move(raw.error()));
  }
  type_id t;
  if (!raw->is_null()) {
    auto typed = type_id::from(*raw);
    if (!typed) {
      return std::unexpected(std::move(typed.error()));
    }
    t = *typed;
  }
  return role_impact{.role = static_cast<impact_role>(*role),
                     .type = t,
                     .count = *count,
                     .first = std::move(*ids)};
}

} // namespace

std::string_view to_string(impact_role r) noexcept {
  switch (r) {
  case impact_role::test:
    return "test";
  case impact_role::build_target:
    return "build_target";
  }
  return "unknown";
}

result<value> to_value(const impact_report &r) {
  std::vector<value> classes;
  classes.reserve(r.summary.classes.size());
  for (const class_impact &c : r.summary.classes) {
    auto v = class_value(c);
    if (!v) {
      return std::unexpected(std::move(v.error()));
    }
    classes.push_back(std::move(*v));
  }
  std::vector<value> roles;
  roles.reserve(r.roles.size());
  for (const role_impact &ri : r.roles) {
    auto v = role_value(ri);
    if (!v) {
      return std::unexpected(std::move(v.error()));
    }
    roles.push_back(std::move(*v));
  }
  auto class_list = value::list(std::move(classes));
  auto role_list = value::list(std::move(roles));
  if (auto e = first_error(class_list, role_list)) {
    return std::unexpected(std::move(*e));
  }
  std::vector<field> fields;
  fields.push_back(field{.no = 1, .v = value::uint(r.summary.total)});
  fields.push_back(field{.no = 2, .v = value::boolean(r.summary.truncated)});
  fields.push_back(field{.no = 3, .v = std::move(*class_list)});
  fields.push_back(field{.no = 4, .v = std::move(*role_list)});
  fields.push_back(field{.no = 5, .v = value::uint(r.visited)});
  fields.push_back(field{.no = 6, .v = value::uint(r.edges_examined)});
  if (r.at_revision) {
    fields.push_back(field{.no = 7, .v = value::digest(*r.at_revision)});
  }
  return value::struct_(std::move(fields));
}

result<impact_report> impact_report_from_value(const value &v) {
  if (v.kind() != value_kind::struct_) {
    return std::unexpected(mismatch("not a struct"));
  }
  impact_report out;
  auto total = uint_field(v, 1, "total");
  auto truncated = required(v, 2, value_kind::bool_, "truncated");
  auto classes = required(v, 3, value_kind::list, "classes");
  auto roles = required(v, 4, value_kind::list, "roles");
  auto visited = uint_field(v, 5, "visited");
  auto examined = uint_field(v, 6, "edges_examined");
  if (auto e = first_error(total, truncated, classes, roles, visited, examined)) {
    return std::unexpected(std::move(*e));
  }
  out.summary.total = *total;
  out.summary.truncated = (*truncated)->as_bool();
  for (const value &c : (*classes)->items()) {
    auto ci = class_from(c);
    if (!ci) {
      return std::unexpected(std::move(ci.error()));
    }
    out.summary.classes.push_back(std::move(*ci));
  }
  for (const value &r : (*roles)->items()) {
    auto ri = role_from(r);
    if (!ri) {
      return std::unexpected(std::move(ri.error()));
    }
    out.roles.push_back(std::move(*ri));
  }
  out.visited = *visited;
  out.edges_examined = *examined;
  if (const value *rev = v.find_field(7); rev != nullptr) {
    if (rev->kind() != value_kind::digest) {
      return std::unexpected(mismatch("field 7 (at_revision) is not a digest"));
    }
    out.at_revision = rev->as_digest();
  }
  for (const field &f : v.fields()) {
    if (f.no < 1 || f.no > 7) {
      return std::unexpected(mismatch(std::format("field {} is undeclared", f.no)));
    }
  }
  return out;
}

std::optional<derivation_pin> pin_of(const provenance &p, const digest &record_object) {
  if (!check_provenance(p) || !p.input_ids || !p.input_revisions) {
    return std::nullopt;
  }
  const std::vector<record_id> &ids = *p.input_ids;
  const std::vector<digest> &revs = *p.input_revisions;
  derivation_pin pin{.record_object = record_object, .inputs = {}};
  pin.inputs.reserve(ids.size());
  for (std::size_t i = 0; i < ids.size() && i < revs.size(); ++i) {
    pin.inputs.push_back(pinned_input{.id = ids[i], .revision = revs[i]});
  }
  std::ranges::sort(pin.inputs);
  const auto dup = std::ranges::unique(pin.inputs);
  pin.inputs.erase(dup.begin(), dup.end());
  return pin;
}

result<bool> is_current(std::span<const pinned_input> inputs, const snapshot &head) {
  bool current = true;
  for (const pinned_input &in : inputs) {
    auto e = head.entry(in.id);
    if (!e) {
      return std::unexpected(std::move(e.error()));
    }
    if (!e->has_value() || (*e)->kind != entry_kind::record) {
      current = false; // deleted or retired: the input changed
      continue;
    }
    // Read and verify the record object: an unverifiable input is an error, never current.
    auto rec = head.get(in.id);
    if (!rec) {
      return std::unexpected(std::move(rec.error()));
    }
    if (rec->object != in.revision) {
      current = false;
    }
  }
  return current;
}

result<std::vector<edge>> default_edges(const record_view &record, const schema_registry &reg) {
  if (record.body == nullptr) {
    return make_error(errc::invalid_argument,
                      std::format("record {} has no body", record.id.to_string()));
  }
  const type_desc *t = reg.find(record.type);
  if (t == nullptr) {
    return make_error(errc::schema_unknown,
                      std::format("record {} has the unregistered type {}", record.id.to_string(),
                                  record.type.to_string()));
  }
  auto edges = extract_edges(record.id, *record.body, *t, reg);
  if (!edges) {
    return std::unexpected(std::move(edges.error()));
  }
  if (!record.provenance_inputs.empty()) {
    if (t->state_class != state_class::derived) {
      return make_error(errc::invalid_argument,
                        std::format("record {} has provenance inputs but type {} is not derived",
                                    record.id.to_string(), t->name));
    }
    std::vector<edge> prov = provenance_edges(record.id, record.provenance_inputs);
    edges->insert(edges->end(), prov.begin(), prov.end());
  }
  if (t->domain == "fabric.edge") {
    auto named = edge_from_record(*record.body);
    if (!named) {
      return std::unexpected(std::move(named.error()));
    }
    edges->push_back(*named);
  }
  return edges;
}

} // namespace sl::fabric
