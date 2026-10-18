// FAB-INV-GEN-05 (SL-ARCH-001 ARCH1-R29, the WO-045 cut line): the generated text JSON Schema
// (engine/fabric/gen/text/<module>.text.schema.json) accepts exactly what the C++ validator
// accepts on the corpus. The C++ validator is from_text (codec.hpp) followed by
// schema_registry::check; the corpus is every fixture value of tests/fixtures/fabric/types plus
// tests/fixtures/fabric/types/text_corpus.json. The JSON Schema subset used by the generator is
// validated here: $ref, type, properties, required, additionalProperties, items, prefixItems,
// minItems, maxItems, enum, pattern, minimum, maximum, oneOf and anyOf.

#include "sl/fabric/codec.hpp"
#include "sl/fabric/gen/system.gen.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <format>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

std::string read_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.good()) {
    return {};
  }
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

/// The module of a type: the longest prefix with a generated text schema.
std::string module_of(std::string_view type_name) {
  for (std::size_t dot = type_name.rfind('.'); dot != std::string_view::npos && dot > 0;
       dot = type_name.rfind('.', dot - 1)) {
    const std::string module(type_name.substr(0, dot));
    if (!read_file(std::format("{}/../../../engine/fabric/gen/text/{}.text.schema.json",
                               SL_FABRIC_FIXTURES_DIR, module))
             .empty()) {
      return module;
    }
  }
  return {};
}

json_value load(const std::string &path) {
  const std::string text = read_file(path);
  REQUIRE_MESSAGE(!text.empty(), path);
  return must(parse_json(text));
}

void write_string(std::string &out, std::string_view s) {
  out += '"';
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += ch;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c < 0x20) {
      out += std::format("\\u{:04x}", c);
    } else {
      out += ch;
    }
  }
  out += '"';
}

void write_compact(std::string &out, const json_value &j) {
  switch (j.kind) {
  case json_value::kind_type::null:
    out += "null";
    break;
  case json_value::kind_type::boolean:
    out += j.boolean ? "true" : "false";
    break;
  case json_value::kind_type::number:
    out += j.text;
    break;
  case json_value::kind_type::string:
    write_string(out, j.text);
    break;
  case json_value::kind_type::array:
    out += '[';
    for (std::size_t i = 0; i < j.items.size(); ++i) {
      out += i == 0 ? "" : ",";
      write_compact(out, j.items[i]);
    }
    out += ']';
    break;
  case json_value::kind_type::object:
    out += '{';
    for (std::size_t i = 0; i < j.members.size(); ++i) {
      out += i == 0 ? "" : ",";
      write_string(out, j.members[i].key);
      out += ':';
      write_compact(out, j.members[i].v);
    }
    out += '}';
    break;
  }
}

std::string compact(const json_value &j) {
  std::string out;
  write_compact(out, j);
  return out;
}

bool is_integer_token(std::string_view t) {
  return !t.empty() && t.find_first_of(".eE") == std::string_view::npos;
}

/// A validator for the JSON Schema subset the generator writes.
class subset_validator {
public:
  explicit subset_validator(const json_value &root) : root_(&root) {}

  [[nodiscard]] bool valid(const json_value &v, const json_value &s) const {
    if (const json_value *ref = s.find("$ref"); ref != nullptr && !valid(v, resolve(ref->text))) {
      return false;
    }
    return type_ok(v, s) && enum_ok(v, s) && string_ok(v, s) && number_ok(v, s) &&
           object_ok(v, s) && array_ok(v, s) && combinators_ok(v, s);
  }

private:
  [[nodiscard]] const json_value &resolve(std::string_view ref) const {
    constexpr std::string_view prefix = "#/$defs/";
    REQUIRE(ref.starts_with(prefix));
    const json_value *def = root_->find("$defs")->find(ref.substr(prefix.size()));
    REQUIRE_MESSAGE(def != nullptr, ref);
    return *def;
  }

  static bool type_ok(const json_value &v, const json_value &s) {
    const json_value *t = s.find("type");
    if (t == nullptr) {
      return true;
    }
    using k = json_value::kind_type;
    const std::string &name = t->text;
    if (name == "object") {
      return v.kind == k::object;
    }
    if (name == "array") {
      return v.kind == k::array;
    }
    if (name == "string") {
      return v.kind == k::string;
    }
    if (name == "boolean") {
      return v.kind == k::boolean;
    }
    if (name == "number") {
      return v.kind == k::number;
    }
    if (name == "integer") {
      return v.kind == k::number && is_integer_token(v.text);
    }
    FAIL("unsupported type " << name);
    return false;
  }

  static bool enum_ok(const json_value &v, const json_value &s) {
    const json_value *e = s.find("enum");
    if (e == nullptr) {
      return true;
    }
    return std::ranges::any_of(e->items, [&v](const json_value &option) {
      return v.kind == option.kind && v.text == option.text;
    });
  }

  static bool string_ok(const json_value &v, const json_value &s) {
    const json_value *p = s.find("pattern");
    if (p == nullptr || v.kind != json_value::kind_type::string) {
      return true;
    }
    return std::regex_search(v.text, std::regex(p->text, std::regex::ECMAScript));
  }

  static bool bound(const json_value &v, const json_value *limit, bool lower) {
    if (limit == nullptr) {
      return true;
    }
    std::int64_t x = 0;
    std::int64_t l = 0;
    const auto rx = std::from_chars(v.text.data(), v.text.data() + v.text.size(), x);
    const auto rl = std::from_chars(limit->text.data(), limit->text.data() + limit->text.size(), l);
    REQUIRE(rl.ec == std::errc{});
    if (rx.ec != std::errc{} || rx.ptr != v.text.data() + v.text.size()) {
      return false;
    }
    return lower ? x >= l : x <= l;
  }

  static bool number_ok(const json_value &v, const json_value &s) {
    if (v.kind != json_value::kind_type::number) {
      return true;
    }
    return bound(v, s.find("minimum"), true) && bound(v, s.find("maximum"), false);
  }

  [[nodiscard]] bool object_ok(const json_value &v, const json_value &s) const {
    if (v.kind != json_value::kind_type::object) {
      return true;
    }
    if (const json_value *req = s.find("required");
        req != nullptr && !std::ranges::all_of(req->items, [&v](const json_value &name) {
          return v.find(name.text) != nullptr;
        })) {
      return false;
    }
    const json_value *props = s.find("properties");
    const json_value *extra = s.find("additionalProperties");
    const bool closed =
        extra != nullptr && extra->kind == json_value::kind_type::boolean && !extra->boolean;
    return std::ranges::all_of(v.members, [&](const json_member &m) {
      const json_value *ps = props == nullptr ? nullptr : props->find(m.key);
      return ps != nullptr ? valid(m.v, *ps) : !closed;
    });
  }

  static std::size_t count(const json_value *n) {
    std::size_t out = 0;
    if (n != nullptr) {
      std::from_chars(n->text.data(), n->text.data() + n->text.size(), out);
    }
    return out;
  }

  [[nodiscard]] bool array_ok(const json_value &v, const json_value &s) const {
    if (v.kind != json_value::kind_type::array) {
      return true;
    }
    if (const json_value *mn = s.find("minItems"); mn != nullptr && v.items.size() < count(mn)) {
      return false;
    }
    if (const json_value *mx = s.find("maxItems"); mx != nullptr && v.items.size() > count(mx)) {
      return false;
    }
    std::size_t first = 0;
    if (const json_value *prefix = s.find("prefixItems"); prefix != nullptr) {
      for (; first < prefix->items.size() && first < v.items.size(); ++first) {
        if (!valid(v.items[first], prefix->items[first])) {
          return false;
        }
      }
    }
    if (const json_value *items = s.find("items"); items != nullptr) {
      for (std::size_t i = first; i < v.items.size(); ++i) {
        if (!valid(v.items[i], *items)) {
          return false;
        }
      }
    }
    return true;
  }

  [[nodiscard]] bool combinators_ok(const json_value &v, const json_value &s) const {
    if (const json_value *one = s.find("oneOf"); one != nullptr) {
      const auto matches =
          std::ranges::count_if(one->items, [&](const json_value &alt) { return valid(v, alt); });
      if (matches != 1) {
        return false;
      }
    }
    const json_value *any = s.find("anyOf");
    return any == nullptr ||
           std::ranges::any_of(any->items, [&](const json_value &alt) { return valid(v, alt); });
  }

  const json_value *root_;
};

/// from_text, then the registry's conformance check.
bool cpp_accepts(const schema_registry &reg, std::string_view type, const json_value &v) {
  const auto parsed = from_text(compact(v), type_id_of(type), reg);
  if (!parsed) {
    return false;
  }
  std::vector<diagnostic_lite> diags;
  return reg.check(*parsed, type_id_of(type), diags).has_value();
}

class schema_set {
public:
  /// The text schema of the module of `type` and the definition of `type` in it.
  std::pair<const json_value *, const json_value *> find(std::string_view type) {
    const std::string module = module_of(type);
    REQUIRE_MESSAGE(!module.empty(), type);
    auto it = docs_.find(module);
    if (it == docs_.end()) {
      it = docs_
               .emplace(module,
                        load(std::format("{}/../../../engine/fabric/gen/text/{}.text.schema.json",
                                         SL_FABRIC_FIXTURES_DIR, module)))
               .first;
    }
    const json_value *def = it->second.find("$defs")->find(type);
    REQUIRE_MESSAGE(def != nullptr, type);
    return {&it->second, def};
  }

private:
  std::map<std::string, json_value> docs_;
};

} // namespace

TEST_CASE("FAB-INV-GEN-05: the text schema and the C++ validator accept every fixture value") {
  const schema_registry reg = must(gen::system_registry());
  schema_set schemas;
  std::size_t checked = 0;
  for (const gen::type_entry &t : gen::system_types()) {
    CAPTURE(t.name);
    const auto [root, def] = schemas.find(t.name);
    const subset_validator validator(*root);
    const std::string module(module_of(t.name));
    const json_value doc = load(std::format("{}/types/{}/{}.json", SL_FABRIC_FIXTURES_DIR, module,
                                            std::string_view(t.name).substr(module.size() + 1)));
    for (const json_value &v : doc.find("values")->items) {
      CAPTURE(compact(v));
      CHECK(cpp_accepts(reg, t.name, v));
      CHECK(validator.valid(v, *def));
      ++checked;
    }
  }
  CHECK(checked > 0);
}

TEST_CASE("FAB-INV-GEN-05: the text schema and the C++ validator judge the corpus alike") {
  const schema_registry reg = must(gen::system_registry());
  schema_set schemas;
  const json_value corpus = load(std::string(SL_FABRIC_FIXTURES_DIR) + "/types/text_corpus.json");
  REQUIRE(corpus.find("format")->text == "sl-text-corpus/1");
  std::size_t rejects = 0;
  for (const json_value &c : corpus.find("cases")->items) {
    const std::string type = c.find("type")->text;
    const json_value &v = *c.find("value");
    const bool accept = c.find("accept")->boolean;
    CAPTURE(type);
    CAPTURE(c.find("why")->text);
    CAPTURE(compact(v));
    const auto [root, def] = schemas.find(type);
    const subset_validator validator(*root);
    CHECK(cpp_accepts(reg, type, v) == accept);
    CHECK(validator.valid(v, *def) == accept);
    if (!accept) {
      ++rejects;
    }
  }
  CHECK(rejects >= 20);
}
