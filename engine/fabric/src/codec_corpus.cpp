// Corpus notation and corpus schemas (WO-041; tests/fixtures/fabric/README.md, slb/schemas.json).
// The notation mirrors tools/fabric/slb_ref.py value_from_json, the schemas slb_text.py
// load_schemas. Used by sl_fabric_corpus and the codec tests.

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

namespace {

using kind_type = json_value::kind_type;

std::unexpected<error> bad(std::string_view what) {
  return make_error(errc::invalid_argument, std::format("corpus notation: {}", what));
}

/// A decimal string without sign as a u64.
std::optional<std::uint64_t> decimal(std::string_view s) noexcept {
  if (s.empty() || !std::ranges::all_of(s, [](char c) noexcept { return c >= '0' && c <= '9'; })) {
    return std::nullopt;
  }
  std::uint64_t n = 0;
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
  if (ec != std::errc{} || end != s.data() + s.size()) {
    return std::nullopt;
  }
  return n;
}

/// "N" minus one, as decimal text (N >= 1, no sign): lets "-18446744073709551616" become the nint
/// magnitude 2^64 - 1 without arithmetic beyond 64 bits.
std::string minus_one(std::string digits) {
  for (std::size_t i = digits.size(); i-- > 0;) {
    if (digits[i] != '0') {
      --digits[i];
      break;
    }
    digits[i] = '9';
  }
  const std::size_t first = digits.find_first_not_of('0');
  return first == std::string::npos ? "0" : digits.substr(first);
}

result<std::vector<std::uint8_t>> hex_of(const json_value &j, std::optional<std::size_t> size) {
  if (j.kind != kind_type::string) {
    return bad("hex string expected");
  }
  auto raw = from_hex(j.text);
  if (!raw) {
    return std::unexpected(raw.error());
  }
  if (size && raw->size() != *size) {
    return bad(std::format("expected {} bytes of hex", *size));
  }
  return raw;
}

result<std::uint32_t> number_u32(const json_value &j) {
  const auto n = j.kind == kind_type::number ? decimal(j.text) : std::nullopt;
  if (!n || *n > 0xFFFFFFFFU) {
    return bad("a number 0..2^32-1 expected");
  }
  return static_cast<std::uint32_t>(*n);
}

result<value> scalar_from_notation(std::string_view name, const json_value &body) {
  if (name == "uint" || name == "nint") {
    if (body.kind != kind_type::string) {
      return bad("decimal string expected");
    }
    if (name == "uint") {
      const auto n = decimal(body.text);
      return n ? result<value>(value::uint(*n)) : bad("uint out of range");
    }
    const std::string_view s = body.text;
    const std::string_view digits = s.substr(std::min<std::size_t>(1, s.size()));
    const bool all_digits = !digits.empty() && std::ranges::all_of(digits, [](char c) noexcept {
      return c >= '0' && c <= '9';
    });
    if (!s.starts_with('-') || !all_digits || digits.find_first_not_of('0') == std::string::npos) {
      return bad("nint must be negative");
    }
    const auto m = decimal(minus_one(std::string(digits)));
    return m ? result<value>(value::nint(*m)) : bad("nint out of range");
  }
  if (name == "f32") {
    const auto raw = hex_of(body, 4);
    if (!raw) {
      return std::unexpected(raw.error());
    }
    std::uint32_t bits = 0;
    for (const std::uint8_t b : *raw) {
      bits = (bits << 8U) | b;
    }
    return value::f32(std::bit_cast<float>(bits));
  }
  if (name == "f64") {
    const auto raw = hex_of(body, 8);
    if (!raw) {
      return std::unexpected(raw.error());
    }
    std::uint64_t bits = 0;
    for (const std::uint8_t b : *raw) {
      bits = (bits << 8U) | b;
    }
    return value::f64(std::bit_cast<double>(bits));
  }
  if (name == "str") {
    if (body.kind != kind_type::string) {
      return bad("string expected");
    }
    return value::string(body.text);
  }
  if (name == "bytes") {
    return hex_of(body, std::nullopt).and_then([](const std::vector<std::uint8_t> &b) {
      return value::bytes(b);
    });
  }
  if (name == "id") {
    return hex_of(body, 16).transform([](const std::vector<std::uint8_t> &b) {
      id_bytes raw{};
      std::ranges::copy(b, raw.begin());
      return value::id(raw);
    });
  }
  if (name == "digest") {
    return hex_of(body, 1 + sha256::digest_size).transform([](const std::vector<std::uint8_t> &b) {
      digest d;
      d.algo = static_cast<hash_algo>(b[0]);
      std::ranges::copy(byte_span(b).subspan(1), d.bytes.begin());
      return value::digest(d);
    });
  }
  return bad(std::format("unknown notation key '{}'", name));
}

result<value> container_from_notation(std::string_view name, const json_value &body) {
  if (body.kind != kind_type::array) {
    return bad("array expected");
  }
  if (name == "list") {
    std::vector<value> items;
    for (const json_value &x : body.items) {
      auto v = value_from_notation(x);
      if (!v) {
        return v;
      }
      items.push_back(std::move(*v));
    }
    return value::list(std::move(items));
  }
  if (name == "variant") {
    if (body.items.size() != 2) {
      return bad("variant is [case, value]");
    }
    const auto c = number_u32(body.items[0]);
    if (!c) {
      return std::unexpected(c.error());
    }
    auto v = value_from_notation(body.items[1]);
    if (!v) {
      return v;
    }
    return value::variant(*c, std::move(*v));
  }
  const std::size_t width = name == "oset" ? 3 : 2;
  for (const json_value &row : body.items) {
    if (row.kind != kind_type::array || row.items.size() != width) {
      return bad(std::format("{} rows have {} parts", name, width));
    }
  }
  if (name == "map") {
    std::vector<map_entry> entries;
    for (const json_value &row : body.items) {
      auto k = value_from_notation(row.items[0]);
      auto v = k ? value_from_notation(row.items[1]) : k;
      if (!v) {
        return v;
      }
      entries.push_back(map_entry{.key = std::move(*k), .v = std::move(*v)});
    }
    return value::map(std::move(entries));
  }
  if (name == "struct") {
    std::vector<field> fields;
    for (const json_value &row : body.items) {
      const auto no = number_u32(row.items[0]);
      if (!no) {
        return std::unexpected(no.error());
      }
      auto v = value_from_notation(row.items[1]);
      if (!v) {
        return v;
      }
      fields.push_back(field{.no = *no, .v = std::move(*v)});
    }
    return value::struct_(std::move(fields));
  }
  if (name == "oset") {
    std::vector<oset_entry> entries;
    for (const json_value &row : body.items) {
      const auto eid = hex_of(row.items[0], 16);
      auto okey = eid ? hex_of(row.items[1], std::nullopt) : eid;
      if (!okey) {
        return std::unexpected(okey.error());
      }
      auto v = value_from_notation(row.items[2]);
      if (!v) {
        return v;
      }
      oset_entry e;
      std::ranges::copy(*eid, e.element.begin());
      e.order_key = std::move(*okey);
      e.v = std::move(*v);
      entries.push_back(std::move(e));
    }
    return value::oset(std::move(entries));
  }
  return bad(std::format("unknown notation key '{}'", name));
}

// ---- corpus schemas
// ------------------------------------------------------------------------------

/// The built-in type a corpus scalar descriptor names, if it has one.
std::optional<std::string> builtin_name(const json_value &desc) {
  const json_value *kind = desc.find("kind");
  if (kind == nullptr || kind->kind != kind_type::string) {
    return std::nullopt;
  }
  const std::string &k = kind->text;
  if (k == "uint" || k == "int") {
    std::uint64_t bits = 64;
    if (const json_value *b = desc.find("bits"); b != nullptr) {
      bits = decimal(b->text).value_or(0);
    }
    if (bits != 8 && bits != 16 && bits != 32 && bits != 64) {
      return std::string(); // a width the type system does not have
    }
    return std::format("{}{}", k == "uint" ? 'u' : 'i', bits);
  }
  constexpr std::array<std::string_view, 7> same = {"bool",  "f32", "f64",   "string",
                                                    "bytes", "id",  "digest"};
  if (std::ranges::find(same, k) != same.end()) {
    return k;
  }
  return std::nullopt;
}

class corpus_converter {
public:
  explicit corpus_converter(const json_value &types) noexcept : types_(&types) {}

  [[nodiscard]] result<corpus_schemas> run() {
    // Pass 1: aliases onto built-ins, and the set of composite names.
    for (const json_member &m : types_->members) {
      const auto builtin = builtin_name(m.v);
      if (builtin && builtin->empty()) {
        skipped_.insert(m.key);
      } else if (builtin) {
        ids_[m.key] = type_id_of(*builtin);
      } else {
        ids_[m.key] = type_id_of("corpus." + m.key);
        composite_.insert(m.key);
      }
    }
    // Pass 2: descriptors; a well-formedness failure skips the type.
    std::map<std::string, type_desc> descs;
    std::map<std::string, std::set<std::string>> refs;
    for (const json_member &m : types_->members) {
      if (!composite_.contains(m.key)) {
        continue;
      }
      std::set<std::string> uses;
      auto t = convert(m.key, m.v, uses);
      if (!t || !check_well_formed(*t)) {
        skipped_.insert(m.key);
        continue;
      }
      descs.emplace(m.key, std::move(*t));
      refs.emplace(m.key, std::move(uses));
    }
    // Pass 3: skip every type that reaches a skipped or unknown name, to a fixed point.
    for (bool changed = true; changed;) {
      changed = false;
      for (const auto &[name, uses] : refs) {
        if (skipped_.contains(name)) {
          continue;
        }
        const bool broken = std::ranges::any_of(uses, [this](const std::string &u) {
          return skipped_.contains(u) || !ids_.contains(u);
        });
        if (broken) {
          skipped_.insert(name);
          changed = true;
        }
      }
    }
    corpus_schemas out;
    for (auto &[name, t] : descs) {
      if (skipped_.contains(name)) {
        continue;
      }
      if (auto r = out.registry.add(std::move(t)); !r) {
        return std::unexpected(r.error());
      }
    }
    for (const auto &[name, id] : ids_) {
      if (!skipped_.contains(name)) {
        out.types.emplace(name, id);
      }
    }
    out.skipped.assign(skipped_.begin(), skipped_.end());
    return out;
  }

private:
  [[nodiscard]] result<type_id> ref(const json_value *j, std::set<std::string> &uses) const {
    if (j == nullptr || j->kind != kind_type::string) {
      return bad("a type reference is a corpus type name");
    }
    uses.insert(j->text);
    const auto it = ids_.find(j->text);
    return it == ids_.end() ? type_id_of("corpus." + j->text) : it->second;
  }

  /// The items of the array member `key`; none when it is absent or not an array.
  [[nodiscard]] static std::span<const json_value> items_of(const json_value &j,
                                                            std::string_view key) noexcept {
    const json_value *v = j.find(key);
    return v != nullptr && v->kind == kind_type::array ? std::span<const json_value>(v->items)
                                                       : std::span<const json_value>();
  }

  [[nodiscard]] static result<std::string> text_of(const json_value &j, std::string_view key) {
    const json_value *v = j.find(key);
    if (v == nullptr || v->kind != kind_type::string) {
      return bad(std::format("'{}' must be a string", key));
    }
    return v->text;
  }

  [[nodiscard]] result<type_desc> convert(const std::string &name, const json_value &desc,
                                          std::set<std::string> &uses) const {
    type_desc t;
    t.name = "corpus." + name;
    t.id = type_id_of(t.name);
    const auto kind = text_of(desc, "kind");
    if (!kind) {
      return std::unexpected(kind.error());
    }
    if (*kind == "enum") {
      t.kind = type_kind::enum_;
      for (const json_value &e : items_of(desc, "enumerators")) {
        const json_value *no_json = e.find("no");
        const auto no = no_json != nullptr ? number_u32(*no_json) : bad("enumerator number");
        const auto n = text_of(e, "name");
        if (!no || !n) {
          return bad("enumerator {no, name}");
        }
        t.enumerators.push_back(enumerator_desc{.no = *no, .name = *n, .doc = {}});
      }
    } else if (*kind == "list" || *kind == "oset") {
      t.kind = *kind == "list" ? type_kind::list : type_kind::oset;
      auto e = ref(desc.find("element"), uses);
      if (!e) {
        return std::unexpected(e.error());
      }
      t.element = *e;
    } else if (*kind == "map") {
      t.kind = type_kind::map;
      const auto key = text_of(desc, "key");
      if (!key) {
        return std::unexpected(key.error());
      }
      constexpr std::array<std::pair<std::string_view, std::string_view>, 4> keys = {
          {{"string", "string"}, {"uint", "u64"}, {"int", "i64"}, {"id", "id"}}};
      const auto *const k =
          std::ranges::find(keys, *key, &std::pair<std::string_view, std::string_view>::first);
      if (k == keys.end()) {
        return bad("map key kind");
      }
      t.key = type_id_of(k->second);
      auto e = ref(desc.find("value"), uses);
      if (!e) {
        return std::unexpected(e.error());
      }
      t.element = *e;
    } else if (*kind == "struct") {
      t.kind = type_kind::struct_;
      for (const json_value &f : items_of(desc, "fields")) {
        const json_value *no_json = f.find("no");
        const auto no = no_json != nullptr ? number_u32(*no_json) : bad("field number");
        const auto n = text_of(f, "name");
        auto type = ref(f.find("type"), uses);
        if (!no || !n || !type) {
          return bad("field {no, name, type}");
        }
        const json_value *opt = f.find("optional");
        field_desc fd;
        fd.no = *no;
        fd.name = *n;
        fd.type = *type;
        fd.presence = opt != nullptr && opt->boolean ? presence::optional : presence::required;
        t.fields.push_back(std::move(fd));
      }
    } else if (*kind == "variant") {
      t.kind = type_kind::variant;
      for (const json_value &c : items_of(desc, "cases")) {
        const json_value *no_json = c.find("no");
        const auto no = no_json != nullptr ? number_u32(*no_json) : bad("case number");
        const auto n = text_of(c, "name");
        auto type = ref(c.find("type"), uses);
        if (!no || !n || !type) {
          return bad("case {no, name, type}");
        }
        t.cases.push_back(case_desc{.no = *no, .name = *n, .type = *type, .doc = {}});
      }
    } else {
      return bad(std::format("unknown corpus kind '{}'", *kind));
    }
    return t;
  }

  const json_value *types_;
  std::map<std::string, type_id> ids_;
  std::set<std::string> composite_;
  std::set<std::string> skipped_;
};

} // namespace

result<value> value_from_notation(const json_value &j) {
  if (j.kind == kind_type::boolean) {
    return value::boolean(j.boolean);
  }
  if (j.kind != kind_type::object || j.members.size() != 1) {
    return bad("a value is true, false or a one-key object");
  }
  const json_member &m = j.members[0];
  if (m.key == "list" || m.key == "map" || m.key == "struct" || m.key == "variant" ||
      m.key == "oset") {
    return container_from_notation(m.key, m.v);
  }
  return scalar_from_notation(m.key, m.v);
}

result<corpus_schemas> load_corpus_schemas(const json_value &doc) {
  const json_value *format = doc.find("format");
  const json_value *types = doc.find("types");
  if (format == nullptr || format->text != "slb-corpus-schemas/1" || types == nullptr ||
      types->kind != kind_type::object) {
    return bad("expected format slb-corpus-schemas/1 with a types object");
  }
  corpus_converter c(*types);
  return c.run();
}

} // namespace sl::fabric
