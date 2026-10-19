// Edge extraction (WO-044, ARCH1-R55): strong and owned refs found type-directed, provenance inputs
// and fabric.edge records.

#include <algorithm>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "sl/fabric/deps.hpp"

namespace sl::fabric {

std::string_view to_string(edge_kind k) noexcept {
  switch (k) {
  case edge_kind::reference:
    return "reference";
  case edge_kind::containment:
    return "containment";
  case edge_kind::derives_from:
    return "derives_from";
  case edge_kind::builds:
    return "builds";
  case edge_kind::validates:
    return "validates";
  }
  return "unknown";
}

namespace {

void sort_unique(std::vector<edge> &edges) {
  std::ranges::sort(edges);
  auto [dup_first, dup_last] = std::ranges::unique(edges);
  edges.erase(dup_first, dup_last);
}

class walker {
public:
  walker(const schema_registry &reg, const record_id &from) : reg_(reg), from_(from) {}

  [[nodiscard]] result<void> visit(const value &v, const type_desc &t, const ref_spec *ref,
                                   const std::string &where) {
    switch (t.kind) {
    case type_kind::struct_:
      return visit_struct(v, t, where);
    case type_kind::variant:
      return visit_variant(v, t, where);
    case type_kind::tagged:
      return visit_element(v, t, ref, where);
    case type_kind::list:
    case type_kind::oset:
    case type_kind::map:
      return visit_container(v, t, ref, where);
    case type_kind::ref:
      return visit_ref(v, ref, where);
    default:
      return {};
    }
  }

  std::vector<edge> take() {
    sort_unique(edges_);
    return std::move(edges_);
  }

private:
  [[nodiscard]] result<const type_desc *> type_of(type_id id, const std::string &where) const {
    const type_desc *t = reg_.find(id);
    if (t == nullptr) {
      return make_error(errc::schema_unknown,
                        std::format("{}: type {} is not registered", where, id.to_string()));
    }
    return t;
  }

  [[nodiscard]] static result<void> mismatch(const std::string &where, std::string_view what) {
    return make_error(errc::type_mismatch, std::format("{}: {}", where, what));
  }

  [[nodiscard]] result<void> visit_struct(const value &v, const type_desc &t,
                                          const std::string &where) {
    if (v.kind() != value_kind::struct_) {
      return mismatch(where, "a struct type needs a struct value");
    }
    for (const field &f : v.fields()) {
      const std::string at = std::format("{}/{}", where, f.no);
      const field_desc *fd = t.find_field(f.no);
      if (fd == nullptr) {
        return mismatch(at, std::format("{} declares no field {}", t.name, f.no));
      }
      const auto ft = type_of(fd->type, at);
      if (!ft) {
        return std::unexpected(ft.error());
      }
      if (auto r = visit(f.v, **ft, fd->ref ? &*fd->ref : nullptr, at); !r) {
        return r;
      }
    }
    return {};
  }

  [[nodiscard]] result<void> visit_variant(const value &v, const type_desc &t,
                                           const std::string &where) {
    if (v.kind() != value_kind::variant) {
      return mismatch(where, "a variant type needs a variant value");
    }
    const std::string at = std::format("{}/{}", where, v.case_no());
    const case_desc *c = t.find_case(v.case_no());
    if (c == nullptr) {
      return mismatch(at, std::format("{} declares no case {}", t.name, v.case_no()));
    }
    const auto ct = type_of(c->type, at);
    if (!ct) {
      return std::unexpected(ct.error());
    }
    return visit(v.payload(), **ct, nullptr, at);
  }

  [[nodiscard]] result<void> visit_element(const value &v, const type_desc &t, const ref_spec *ref,
                                           const std::string &where) {
    if (!t.element) {
      return mismatch(where, std::format("{} has no element type", t.name));
    }
    const auto et = type_of(*t.element, where);
    if (!et) {
      return std::unexpected(et.error());
    }
    return visit(v, **et, ref, where);
  }

  [[nodiscard]] result<void> visit_container(const value &v, const type_desc &t,
                                             const ref_spec *ref, const std::string &where) {
    if (!t.element) {
      return mismatch(where, std::format("{} has no element type", t.name));
    }
    const auto et = type_of(*t.element, where);
    if (!et) {
      return std::unexpected(et.error());
    }
    const type_desc &elem = **et;
    switch (t.kind) {
    case type_kind::list: {
      if (v.kind() != value_kind::list) {
        return mismatch(where, "a list type needs a list value");
      }
      std::size_t i = 0;
      for (const value &item : v.items()) {
        if (auto r = visit(item, elem, ref, std::format("{}[{}]", where, i++)); !r) {
          return r;
        }
      }
      return {};
    }
    case type_kind::oset: {
      if (v.kind() != value_kind::oset) {
        return mismatch(where, "an oset type needs an oset value");
      }
      for (const oset_entry &e : v.elements()) {
        const record_id id = record_id::from_bytes(e.element).value_or(record_id{});
        if (auto r = visit(e.v, elem, ref, std::format("{}/<{}>", where, id.to_string())); !r) {
          return r;
        }
      }
      return {};
    }
    default: { // map
      if (v.kind() != value_kind::map) {
        return mismatch(where, "a map type needs a map value");
      }
      std::size_t i = 0;
      for (const map_entry &e : v.entries()) {
        if (auto r = visit(e.v, elem, ref, std::format("{}[#{}]", where, i++)); !r) {
          return r;
        }
      }
      return {};
    }
    }
  }

  [[nodiscard]] result<void> visit_ref(const value &v, const ref_spec *ref,
                                       const std::string &where) {
    if (v.kind() != value_kind::id) {
      return mismatch(where, "a ref needs an id value");
    }
    if (ref == nullptr || ref->strength == ref_strength::weak) {
      return {};
    }
    const auto to = v.as_record_id();
    if (!to) {
      return mismatch(where, std::format("not a valid ref target: {}", to.error().message));
    }
    edges_.push_back(edge{.from = from_,
                          .to = *to,
                          .kind = ref->strength == ref_strength::owned ? edge_kind::containment
                                                                       : edge_kind::reference,
                          .strength = ref->strength});
    return {};
  }

  const schema_registry &reg_; // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
  record_id from_;
  std::vector<edge> edges_;
};

} // namespace

result<std::vector<edge>> extract_edges(const record_id &from, const value &body,
                                        const type_desc &type, const schema_registry &reg) {
  walker w(reg, from);
  if (auto r = w.visit(body, type, nullptr, ""); !r) {
    return std::unexpected(std::move(r.error()));
  }
  return w.take();
}

std::vector<edge> provenance_edges(const record_id &from, std::span<const record_id> inputs) {
  std::vector<edge> out;
  out.reserve(inputs.size());
  for (const record_id &in : inputs) {
    out.push_back(edge{
        .from = from, .to = in, .kind = edge_kind::derives_from, .strength = ref_strength::strong});
  }
  sort_unique(out);
  return out;
}

result<edge> edge_from_record(const value &body) {
  if (body.kind() != value_kind::struct_) {
    return make_error(errc::type_mismatch, "an edge record is a struct");
  }
  const auto id_field = [&](std::uint32_t no, std::string_view name) -> result<record_id> {
    const value *f = body.find_field(no);
    if (f == nullptr || f->kind() != value_kind::id) {
      return make_error(errc::type_mismatch,
                        std::format("an edge record needs an id in field {} ({})", no, name));
    }
    return f->as_record_id();
  };
  const auto from = id_field(1, "from");
  if (!from) {
    return std::unexpected(from.error());
  }
  const auto to = id_field(2, "to");
  if (!to) {
    return std::unexpected(to.error());
  }
  const value *k = body.find_field(3);
  if (k == nullptr || k->kind() != value_kind::uint) {
    return make_error(errc::type_mismatch, "an edge record needs a uint in field 3 (kind)");
  }
  if (k->as_uint() < 1 || k->as_uint() > 5) {
    return make_error(errc::invalid_argument,
                      std::format("edge kind {} is not 1..5", k->as_uint()));
  }
  return edge{.from = *from,
              .to = *to,
              .kind = static_cast<edge_kind>(k->as_uint()),
              .strength = ref_strength::strong};
}

} // namespace sl::fabric
