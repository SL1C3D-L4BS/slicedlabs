// sl-type-def/1 modules as type descriptors (WO-057): the module form of schemas/fabric/README.md
// read into type_desc for the classifier, the migration fixtures and tools.

#include <charconv>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/migration.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

namespace {

// The scalar kind a field type stands for (through tagged; an enum is its number), or nullopt.
using kind_resolver = std::function<std::optional<type_kind>(const type_id &)>;

std::unexpected<error> bad(std::string_view type, std::string_view what) {
  return make_error(errc::invalid_argument,
                    std::format("type {}: {}", type.empty() ? "?" : type, what));
}

const json_value *member(const json_value &j, std::string_view key, json_value::kind_type kind) {
  const json_value *m = j.find(key);
  return m != nullptr && m->kind == kind ? m : nullptr;
}

std::optional<std::string> string_member(const json_value &j, std::string_view key) {
  const json_value *m = member(j, key, json_value::kind_type::string);
  return m != nullptr ? std::optional<std::string>(m->text) : std::nullopt;
}

template <class T> std::optional<T> parse_number(std::string_view text) {
  T out{};
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
  if (ec != std::errc{} || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return out;
}

std::optional<std::uint32_t> u32_member(const json_value &j, std::string_view key) {
  const json_value *m = member(j, key, json_value::kind_type::number);
  return m != nullptr ? parse_number<std::uint32_t>(m->text) : std::nullopt;
}

std::optional<type_version> parse_version(std::string_view text) {
  const auto dot = text.find('.');
  if (dot == std::string_view::npos) {
    return std::nullopt;
  }
  auto major = parse_number<std::uint32_t>(text.substr(0, dot));
  auto minor = parse_number<std::uint32_t>(text.substr(dot + 1));
  if (!major || !minor) {
    return std::nullopt;
  }
  return type_version{.major = *major, .minor = *minor};
}

std::optional<id_class> class_from_name(std::string_view name) {
  for (std::uint8_t n = 0; n <= 15; ++n) {
    const auto c = static_cast<id_class>(n);
    if (to_string(c) == name) {
      return c;
    }
  }
  return std::nullopt;
}

std::optional<state_class> state_from_name(std::string_view name) {
  for (std::uint8_t n = 0; n <= 3; ++n) {
    const auto s = static_cast<state_class>(n);
    if (to_string(s) == name) {
      return s;
    }
  }
  return std::nullopt;
}

std::optional<type_kind> builtin_kind(const type_id &t) {
  for (const type_desc &b : builtin_types()) {
    if (b.id == t) {
      return b.kind;
    }
  }
  return std::nullopt;
}

bool is_integer_token(std::string_view text) {
  return text.find_first_of(".eE") == std::string_view::npos;
}

result<value> integer_value(std::string_view text) {
  if (!text.empty() && text.front() == '-') {
    if (auto v = parse_number<std::int64_t>(text)) {
      return value::integer(*v);
    }
  } else if (auto v = parse_number<std::uint64_t>(text)) {
    return value::uint(*v);
  }
  return make_error(errc::invalid_argument, std::format("{} is not a 64-bit integer", text));
}

// A field default as the value of `kind` (nullopt: judged by the JSON token alone).
result<value> default_value(const json_value &j, std::optional<type_kind> kind) {
  using jk = json_value::kind_type;
  switch (j.kind) {
  case jk::boolean:
    return value::boolean(j.boolean);
  case jk::string:
    return value::string(j.text);
  case jk::number:
    if (kind == type_kind::f32 || kind == type_kind::f64 || (!kind && !is_integer_token(j.text))) {
      auto d = parse_number<double>(j.text);
      if (!d) {
        return make_error(errc::invalid_argument, std::format("{} is not a number", j.text));
      }
      return kind == type_kind::f32 ? value::f32(static_cast<float>(*d)) : value::f64(*d);
    }
    return integer_value(j.text);
  default:
    return make_error(errc::invalid_argument, "a default must be a boolean, number or string");
  }
}

result<std::optional<type_id>> type_ref(const json_value &j, std::string_view key) {
  const json_value *m = j.find(key);
  if (m == nullptr) {
    return std::optional<type_id>();
  }
  if (m->kind != json_value::kind_type::string || m->text.empty()) {
    return make_error(errc::invalid_argument, std::format("{} must name a type", key));
  }
  return std::optional<type_id>(type_id_of(m->text));
}

result<ref_spec> parse_ref(const json_value &j) {
  ref_spec r;
  const auto strength = string_member(j, "strength");
  if (strength == "weak") {
    r.strength = ref_strength::weak;
  } else if (strength == "strong") {
    r.strength = ref_strength::strong;
  } else if (strength == "owned") {
    r.strength = ref_strength::owned;
  } else {
    return make_error(errc::invalid_argument, "ref strength must be weak, strong or owned");
  }
  const json_value *targets = member(j, "targets", json_value::kind_type::array);
  if (targets == nullptr) {
    return make_error(errc::invalid_argument, "ref targets must be a list");
  }
  for (const json_value &t : targets->items) {
    if (t.kind != json_value::kind_type::string) {
      return make_error(errc::invalid_argument, "a ref target must name a type");
    }
    r.targets.push_back(type_id_of(t.text));
  }
  return r;
}

result<field_desc> parse_field(const json_value &j, const kind_resolver &resolve) {
  field_desc f;
  const auto no = u32_member(j, "no");
  auto name = string_member(j, "name");
  auto type = string_member(j, "type");
  const auto pres = string_member(j, "presence");
  if (!no || !name || !type || (pres != "required" && pres != "optional")) {
    return make_error(errc::invalid_argument,
                      "a field needs no, name, type and presence required|optional");
  }
  f.no = *no;
  f.name = std::move(*name);
  f.type = type_id_of(*type);
  f.presence = pres == "required" ? presence::required : presence::optional;
  if (const json_value *d = j.find("default")) {
    auto v = default_value(*d, resolve(f.type));
    if (!v) {
      return make_error(errc::invalid_argument,
                        std::format("field {} default: {}", f.no, v.error().message));
    }
    f.default_value = std::move(*v);
  }
  if (j.find("since_minor") != nullptr) {
    const auto since = u32_member(j, "since_minor");
    if (!since) {
      return make_error(errc::invalid_argument, std::format("field {} since_minor", f.no));
    }
    f.since_minor = *since;
  }
  if (j.find("deprecated_in") != nullptr) {
    const auto dep = u32_member(j, "deprecated_in");
    if (!dep) {
      return make_error(errc::invalid_argument, std::format("field {} deprecated_in", f.no));
    }
    f.deprecated_in = dep;
  }
  if (const json_value *r = j.find("ref")) {
    auto spec = parse_ref(*r);
    if (!spec) {
      return make_error(errc::invalid_argument,
                        std::format("field {}: {}", f.no, spec.error().message));
    }
    f.ref = std::move(*spec);
  }
  f.doc = string_member(j, "doc").value_or("");
  return f;
}

result<void> parse_members(const json_value &t, type_desc &out) {
  if (const json_value *list = member(t, "enumerators", json_value::kind_type::array)) {
    for (const json_value &e : list->items) {
      const auto no = u32_member(e, "no");
      auto name = string_member(e, "name");
      if (!no || !name) {
        return make_error(errc::invalid_argument, "an enumerator needs no and name");
      }
      out.enumerators.push_back(enumerator_desc{
          .no = *no, .name = std::move(*name), .doc = string_member(e, "doc").value_or("")});
    }
  }
  if (const json_value *list = member(t, "cases", json_value::kind_type::array)) {
    for (const json_value &c : list->items) {
      const auto no = u32_member(c, "no");
      auto name = string_member(c, "name");
      const auto type = string_member(c, "type");
      if (!no || !name || !type) {
        return make_error(errc::invalid_argument, "a case needs no, name and type");
      }
      out.cases.push_back(case_desc{.no = *no,
                                    .name = std::move(*name),
                                    .type = type_id_of(*type),
                                    .doc = string_member(c, "doc").value_or("")});
    }
  }
  return {};
}

result<type_desc> parse_type(const json_value &t, std::string_view default_domain,
                             const kind_resolver &resolve) {
  if (t.kind != json_value::kind_type::object) {
    return make_error(errc::invalid_argument, "a type must be an object");
  }
  type_desc out;
  out.name = string_member(t, "name").value_or("");
  if (out.name.empty()) {
    return bad(out.name, "no name");
  }
  out.id = type_id_of(out.name);
  const auto kind = type_kind_from_string(string_member(t, "kind").value_or(""));
  if (!kind) {
    return bad(out.name, "unknown kind");
  }
  out.kind = *kind;
  const auto version = parse_version(string_member(t, "version").value_or(""));
  if (!version) {
    return bad(out.name, "version must be \"major.minor\"");
  }
  out.version = *version;
  out.domain = string_member(t, "domain").value_or(std::string(default_domain));
  if (out.domain.empty()) {
    out.domain = std::string(default_domain);
  }
  const auto rc = class_from_name(string_member(t, "record_class").value_or("none"));
  const auto sc = state_from_name(string_member(t, "state_class").value_or("none"));
  if (!rc || !sc) {
    return bad(out.name, "unknown record_class or state_class");
  }
  out.record_class = *rc;
  if (const json_value *d = member(t, "document_root", json_value::kind_type::boolean)) {
    out.document_root = d->boolean;
  }
  // WO-045: only a record type keeps its state class.
  out.state_class =
      out.record_class != id_class::none || out.document_root ? *sc : state_class::none;
  if (const json_value *fields = member(t, "fields", json_value::kind_type::array)) {
    for (const json_value &f : fields->items) {
      auto fd = parse_field(f, resolve);
      if (!fd) {
        return bad(out.name, fd.error().message);
      }
      out.fields.push_back(std::move(*fd));
    }
  }
  if (const json_value *reserved = member(t, "reserved", json_value::kind_type::array)) {
    for (const json_value &r : reserved->items) {
      const auto no = r.kind == json_value::kind_type::number ? parse_number<std::uint32_t>(r.text)
                                                              : std::nullopt;
      if (!no) {
        return bad(out.name, "a reserved number must be a u32");
      }
      out.reserved.push_back(*no);
    }
  }
  if (auto ok = parse_members(t, out); !ok) {
    return bad(out.name, ok.error().message);
  }
  auto element = type_ref(t, "element");
  auto key = type_ref(t, "key");
  if (!element || !key) {
    return bad(out.name, !element ? element.error().message : key.error().message);
  }
  out.element = *element;
  out.key = *key;
  out.doc = string_member(t, "doc").value_or("");
  return out;
}

} // namespace

result<type_desc> type_from_json(const json_value &t, std::string_view default_domain) {
  return parse_type(t, default_domain, builtin_kind);
}

result<std::vector<type_desc>> types_from_module(const json_value &module) {
  if (string_member(module, "format") != "sl-type-def/1") {
    return make_error(errc::format_unsupported, "a module must have format sl-type-def/1");
  }
  const json_value *types = member(module, "types", json_value::kind_type::array);
  if (types == nullptr) {
    return make_error(errc::invalid_argument, "a module needs a list of types");
  }
  // The module's own enum and tagged types, so defaults of fields typed by them take the right
  // scalar kind.
  struct shape {
    std::optional<type_kind> kind;
    std::optional<type_id> element;
  };
  std::map<type_id, shape> shapes;
  for (const json_value &t : types->items) {
    const auto name = string_member(t, "name");
    if (!name) {
      continue;
    }
    const auto element = string_member(t, "element");
    shapes.insert_or_assign(
        type_id_of(*name),
        shape{.kind = type_kind_from_string(string_member(t, "kind").value_or("")),
              .element = element ? std::optional<type_id>(type_id_of(*element)) : std::nullopt});
  }
  const kind_resolver resolve = [&shapes](const type_id &start) -> std::optional<type_kind> {
    type_id cur = start;
    for (std::size_t hops = 0; hops <= shapes.size(); ++hops) {
      if (auto b = builtin_kind(cur)) {
        return b;
      }
      const auto it = shapes.find(cur);
      if (it == shapes.end() || !it->second.kind) {
        return std::nullopt;
      }
      if (*it->second.kind == type_kind::enum_) {
        return type_kind::u32;
      }
      if (*it->second.kind != type_kind::tagged || !it->second.element) {
        return std::nullopt;
      }
      cur = *it->second.element;
    }
    return std::nullopt;
  };
  const std::string domain = string_member(module, "domain").value_or("");
  std::vector<type_desc> out;
  for (const json_value &t : types->items) {
    auto d = parse_type(t, domain, resolve);
    if (!d) {
      return std::unexpected(d.error());
    }
    out.push_back(std::move(*d));
  }
  return out;
}

} // namespace sl::fabric
