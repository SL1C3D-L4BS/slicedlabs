// Canonical text of SLB/1 values (WO-041, SL-ARCH-001 ARCH1-R29, ADR-0012). See codec.hpp; the
// spelling of every form follows tools/fabric/slb_text.py.

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

namespace {

constexpr std::uint64_t max_safe = (std::uint64_t{1} << 53U) - 1;
constexpr std::uint64_t max_u32 = std::numeric_limits<std::uint32_t>::max();
constexpr std::string_view crockford = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr std::size_t id_text_chars = 26;

std::unexpected<error> type_error(std::string_view what) {
  return make_error(errc::type_mismatch, std::format("R-TXT-TYPE: {}", what));
}

std::unexpected<error> syntax_error(std::string_view what) {
  return make_error(errc::text_syntax, std::format("R-TXT-SYNTAX: {}", what));
}

// ---- scalar spellings
// -----------------------------------------------------------------------------

void append_json_string(std::string &out, std::string_view s) {
  out += '"';
  for (const char c : s) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20U) {
        out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
      } else {
        out += c;
      }
      break;
    }
  }
  out += '"';
}

/// "<prefix>_<26 Crockford characters>": the 128 id bits, big-endian, after two zero bits.
result<std::string> id_to_text(const id_bytes &raw) {
  const auto cls = static_cast<id_class>(raw[0]);
  if (!is_valid(cls)) {
    return type_error("id needs 16 bytes and a class in 1..15");
  }
  const byte_span bytes(raw);
  const auto bit = [bytes](std::size_t k) noexcept -> unsigned {
    if (k < 2) {
      return 0; // the 130-bit string starts with two zero bits
    }
    const std::size_t b = k - 2;
    return (static_cast<unsigned>(bytes[b / 8]) >> (7U - static_cast<unsigned>(b % 8))) & 1U;
  };
  std::string out(prefix(cls));
  out += '_';
  for (std::size_t i = 0; i < id_text_chars; ++i) {
    unsigned v = 0;
    for (std::size_t j = 0; j < 5; ++j) {
      v = (v << 1U) | bit((5 * i) + j);
    }
    out += crockford[v];
  }
  return out;
}

result<id_bytes> id_from_text(std::string_view text) {
  const std::size_t sep = text.find('_');
  if (sep == std::string_view::npos) {
    return syntax_error("id text has no '_'");
  }
  const auto cls = class_from_prefix(text.substr(0, sep));
  const std::string_view body = text.substr(sep + 1);
  if (!cls || body.size() != id_text_chars) {
    return syntax_error(std::format("bad id text '{}'", text));
  }
  std::array<unsigned, id_text_chars> digit_store{};
  const std::span<unsigned> digits(digit_store);
  for (std::size_t i = 0; i < body.size(); ++i) {
    const std::size_t v = crockford.find(body[i]);
    if (v == std::string_view::npos) {
      return syntax_error(std::format("bad id alphabet in '{}'", text));
    }
    digits[i] = static_cast<unsigned>(v);
  }
  if (digits[0] > 3) {
    return syntax_error(std::format("id text '{}' exceeds 128 bits", text));
  }
  id_bytes raw{};
  const std::span<std::uint8_t> out(raw);
  for (std::size_t k = 2; k < 5 * id_text_chars; ++k) {
    const unsigned b = (digits[k / 5] >> (4U - static_cast<unsigned>(k % 5))) & 1U;
    const std::size_t at = k - 2;
    out[at / 8] =
        static_cast<std::uint8_t>(out[at / 8] | (b << (7U - static_cast<unsigned>(at % 8))));
  }
  if (raw[0] != static_cast<std::uint8_t>(*cls)) {
    return syntax_error(std::format("prefix of '{}' does not match class byte {}", text, raw[0]));
  }
  return raw;
}

/// The shortest digits (no trailing zero) whose correctly rounded value is x, x > 0 finite, and
/// the decimal exponent of the first digit: the first precision of "%.*e" that round-trips.
template <class F> std::pair<std::string, int> shortest_digits(F x) {
  std::array<char, 64> buf{};
  for (int places = 0; places < 18; ++places) {
    const auto [end, ec] =
        std::to_chars(buf.data(), buf.data() + buf.size(), static_cast<double>(x),
                      std::chars_format::scientific, places);
    if (ec != std::errc{}) {
      break;
    }
    const std::string_view s(buf.data(), static_cast<std::size_t>(end - buf.data()));
    F back{};
    const auto parsed = std::from_chars(s.data(), s.data() + s.size(), back);
    if (parsed.ec != std::errc{} || std::bit_cast<std::uint64_t>(static_cast<double>(back)) !=
                                        std::bit_cast<std::uint64_t>(static_cast<double>(x))) {
      continue;
    }
    const std::size_t e = s.find('e');
    std::string digits;
    for (const char c : s.substr(0, e)) {
      if (c != '.') {
        digits += c;
      }
    }
    while (digits.size() > 1 && digits.back() == '0') {
      digits.pop_back();
    }
    int exp = 0;
    const std::string_view es = s.substr(e + 1);
    const char *first = es.data() + (es.front() == '+' ? 1 : 0);
    std::from_chars(first, es.data() + es.size(), exp);
    return {digits, exp};
  }
  return {"0", 0}; // unreachable: 17 places always round-trip
}

template <class F> std::string float_text(F x) {
  if (x == F{0}) {
    return "0.0";
  }
  std::string out = x < F{0} ? "-" : "";
  const auto [digits, n] = shortest_digits(x < F{0} ? -x : x);
  const auto k = static_cast<int>(digits.size());
  if (n >= -4 && n < 16) {
    if (n >= 0) {
      std::string whole = digits.substr(0, static_cast<std::size_t>(std::min(n + 1, k)));
      whole.resize(static_cast<std::size_t>(n) + 1, '0');
      const std::string frac = k > n + 1 ? digits.substr(static_cast<std::size_t>(n) + 1) : "0";
      out += whole + "." + frac;
    } else {
      out += "0." + std::string(static_cast<std::size_t>(-n - 1), '0') + digits;
    }
  } else {
    out += digits.substr(0, 1);
    if (k > 1) {
      out += "." + digits.substr(1);
    }
    out += std::format("e{}{:02}", n >= 0 ? '+' : '-', n >= 0 ? n : -n);
  }
  return out;
}

// ---- types as the text sees them --------------------------------------------------------------

enum class text_kind : std::uint8_t {
  boolean,
  uint,
  sint,
  enumeration,
  f32,
  f64,
  string,
  bytes,
  id,
  digest,
  list,
  map,
  structure,
  variant,
  oset,
};

struct text_type {
  text_kind kind = text_kind::boolean;
  unsigned bits = 64;
  const type_desc *desc = nullptr; // the type after tagged bases are resolved
};

std::string_view kind_name(text_kind k) noexcept {
  constexpr std::array<std::string_view, 15> names = {
      "bool", "uint",   "int",  "enum", "f32",    "f64",     "string", "bytes",
      "id",   "digest", "list", "map",  "struct", "variant", "oset"};
  return std::span(names)[static_cast<std::size_t>(k)];
}

class text_schema {
public:
  explicit text_schema(const schema_registry &schemas) noexcept : schemas_(&schemas) {}

  [[nodiscard]] result<text_type> lookup(const type_id &id) const {
    const type_desc *t = schemas_->find(id);
    for (std::size_t hops = 0; t != nullptr && t->kind == type_kind::tagged; ++hops) {
      if (hops > max_value_depth || !t->element) {
        t = nullptr;
        break;
      }
      t = schemas_->find(*t->element);
    }
    if (t == nullptr) {
      return make_error(errc::schema_unknown,
                        std::format("type {} is not registered", id.to_string()));
    }
    text_type out{.desc = t};
    switch (t->kind) {
    case type_kind::bool_:
      out.kind = text_kind::boolean;
      break;
    case type_kind::i8:
    case type_kind::i16:
    case type_kind::i32:
    case type_kind::i64:
      out.kind = text_kind::sint;
      out.bits = 8U << (static_cast<unsigned>(t->kind) - static_cast<unsigned>(type_kind::i8));
      break;
    case type_kind::u8:
    case type_kind::u16:
    case type_kind::u32:
    case type_kind::u64:
      out.kind = text_kind::uint;
      out.bits = 8U << (static_cast<unsigned>(t->kind) - static_cast<unsigned>(type_kind::u8));
      break;
    case type_kind::f32:
      out.kind = text_kind::f32;
      break;
    case type_kind::f64:
      out.kind = text_kind::f64;
      break;
    case type_kind::string:
      out.kind = text_kind::string;
      break;
    case type_kind::bytes:
      out.kind = text_kind::bytes;
      break;
    case type_kind::id:
    case type_kind::ref:
      out.kind = text_kind::id;
      break;
    case type_kind::digest:
    case type_kind::blob_ref:
      out.kind = text_kind::digest;
      break;
    case type_kind::enum_:
      out.kind = text_kind::enumeration;
      break;
    case type_kind::struct_:
    case type_kind::external_ref:
      out.kind = text_kind::structure;
      break;
    case type_kind::list:
      out.kind = text_kind::list;
      break;
    case type_kind::map:
      out.kind = text_kind::map;
      break;
    case type_kind::oset:
      out.kind = text_kind::oset;
      break;
    case type_kind::variant:
      out.kind = text_kind::variant;
      break;
    case type_kind::tagged:
      return make_error(errc::schema_unknown, "tagged type without a base");
    }
    return out;
  }

  [[nodiscard]] result<text_type> part(const std::optional<type_id> &id) const {
    if (!id) {
      return make_error(errc::schema_invalid, "a container type without its element or key");
    }
    return lookup(*id);
  }

private:
  const schema_registry *schemas_;
};

std::unexpected<error> mismatch(const text_type &t, const value &v) {
  return type_error(
      std::format("value {} does not fit schema kind {}", to_string(v.kind()), kind_name(t.kind)));
}

/// Whether an integer value is within the width of a uint or int type.
bool fits(const text_type &t, const value &v) noexcept {
  const unsigned bits = t.bits;
  if (t.kind == text_kind::uint) {
    return v.kind() == value_kind::uint && (bits >= 64 || v.as_uint() < (std::uint64_t{1} << bits));
  }
  const std::uint64_t half = std::uint64_t{1} << (bits - 1);
  if (v.kind() == value_kind::uint) {
    return v.as_uint() < half;
  }
  return v.kind() == value_kind::nint && v.nint_magnitude() < half;
}

std::string int_text(const value &v) {
  if (v.kind() == value_kind::uint) {
    const std::uint64_t u = v.as_uint();
    return u <= max_safe ? std::format("{}", u) : std::format("\"{}\"", u);
  }
  // In range for a 64-bit int type, so magnitude + 1 does not overflow.
  const std::uint64_t abs = v.nint_magnitude() + 1;
  return abs <= max_safe ? std::format("-{}", abs) : std::format("\"-{}\"", abs);
}

// ---- to_text -----------------------------------------------------------------------------------

class text_writer {
public:
  explicit text_writer(const text_schema &schema) noexcept : schema_(&schema) {}

  [[nodiscard]] result<void> write(const value &v, const type_id &type) {
    const auto t = schema_->lookup(type);
    if (!t) {
      return std::unexpected(t.error());
    }
    return write(v, *t);
  }

  [[nodiscard]] std::string take() noexcept { return std::move(out_); }

private:
  [[nodiscard]] result<void> write(const value &v, const text_type &t) {
    switch (t.kind) {
    case text_kind::boolean:
      if (v.kind() != value_kind::bool_) {
        return mismatch(t, v);
      }
      out_ += v.as_bool() ? "true" : "false";
      return {};
    case text_kind::uint:
    case text_kind::sint:
      if (!fits(t, v)) {
        return mismatch(t, v);
      }
      out_ += int_text(v);
      return {};
    case text_kind::enumeration: {
      if (v.kind() == value_kind::uint && v.as_uint() <= max_u32) {
        const enumerator_desc *e = t.desc->find_enumerator(static_cast<std::uint32_t>(v.as_uint()));
        if (e != nullptr) {
          append_json_string(out_, e->name);
          return {};
        }
      }
      return mismatch(t, v);
    }
    case text_kind::f32:
      if (v.kind() != value_kind::f32) {
        return mismatch(t, v);
      }
      out_ += float_text(v.as_f32());
      return {};
    case text_kind::f64:
      if (v.kind() != value_kind::f64) {
        return mismatch(t, v);
      }
      out_ += float_text(v.as_f64());
      return {};
    default:
      return write_text_scalar(v, t);
    }
  }

  [[nodiscard]] result<void> write_text_scalar(const value &v, const text_type &t) {
    switch (t.kind) {
    case text_kind::string:
      if (v.kind() != value_kind::string) {
        return mismatch(t, v);
      }
      append_json_string(out_, v.as_string());
      return {};
    case text_kind::bytes:
      if (v.kind() != value_kind::bytes) {
        return mismatch(t, v);
      }
      out_ += '"' + to_hex(v.as_bytes()) + '"';
      return {};
    case text_kind::id: {
      if (v.kind() != value_kind::id) {
        return mismatch(t, v);
      }
      const auto s = id_to_text(v.as_id_bytes());
      if (!s) {
        return std::unexpected(s.error());
      }
      out_ += '"' + *s + '"';
      return {};
    }
    case text_kind::digest: {
      if (v.kind() != value_kind::digest) {
        return mismatch(t, v);
      }
      const auto s = digest_text(v.as_digest());
      if (!s) {
        return mismatch(t, v);
      }
      out_ += '"' + *s + '"';
      return {};
    }
    default:
      return write_container(v, t);
    }
  }

  [[nodiscard]] result<void> write_unknown(const value &v) {
    const auto raw = encode(v);
    if (!raw) {
      return std::unexpected(raw.error());
    }
    out_ += "\"slb:" + to_hex(*raw) + '"';
    return {};
  }

  [[nodiscard]] result<void> write_container(const value &v, const text_type &t) {
    switch (t.kind) {
    case text_kind::list:
      return write_list(v, t);
    case text_kind::map:
      return write_map(v, t);
    case text_kind::structure:
      return write_struct(v, t);
    case text_kind::variant:
      return write_variant(v, t);
    case text_kind::oset:
      return write_oset(v, t);
    default:
      return mismatch(t, v);
    }
  }

  [[nodiscard]] result<void> write_list(const value &v, const text_type &t) {
    if (v.kind() != value_kind::list) {
      return mismatch(t, v);
    }
    const auto element = schema_->part(t.desc->element);
    if (!element) {
      return std::unexpected(element.error());
    }
    out_ += '[';
    for (std::size_t i = 0; i < v.items().size(); ++i) {
      if (i > 0) {
        out_ += ',';
      }
      if (auto r = write(v.items()[i], *element); !r) {
        return r;
      }
    }
    out_ += ']';
    return {};
  }

  [[nodiscard]] result<void> write_map(const value &v, const text_type &t) {
    if (v.kind() != value_kind::map) {
      return mismatch(t, v);
    }
    const auto key = schema_->part(t.desc->key);
    const auto element = schema_->part(t.desc->element);
    if (!key || !element) {
      return std::unexpected(!key ? key.error() : element.error());
    }
    out_ += '[';
    bool first = true;
    for (const map_entry &e : v.entries()) {
      out_ += first ? "[" : ",[";
      first = false;
      if (auto r = write(e.key, *key); !r) {
        return r;
      }
      out_ += ',';
      if (auto r = write(e.v, *element); !r) {
        return r;
      }
      out_ += ']';
    }
    out_ += ']';
    return {};
  }

  [[nodiscard]] result<void> write_struct(const value &v, const text_type &t) {
    if (v.kind() != value_kind::struct_) {
      return mismatch(t, v);
    }
    out_ += '{';
    bool first = true;
    for (const field &f : v.fields()) {
      if (!first) {
        out_ += ',';
      }
      first = false;
      const field_desc *fd = t.desc->find_field(f.no);
      if (fd == nullptr) {
        append_json_string(out_, std::format("?{}", f.no));
        out_ += ':';
        if (auto r = write_unknown(f.v); !r) {
          return r;
        }
        continue;
      }
      append_json_string(out_, fd->name);
      out_ += ':';
      if (auto r = write(f.v, fd->type); !r) {
        return r;
      }
    }
    for (const field_desc &fd : t.desc->fields) {
      if (fd.presence == presence::required && !v.has_field(fd.no)) {
        return type_error(std::format("required field {} is absent", fd.name));
      }
    }
    out_ += '}';
    return {};
  }

  [[nodiscard]] result<void> write_variant(const value &v, const text_type &t) {
    if (v.kind() != value_kind::variant) {
      return mismatch(t, v);
    }
    out_ += '{';
    const case_desc *c = t.desc->find_case(v.case_no());
    if (c == nullptr) {
      append_json_string(out_, std::format("?{}", v.case_no()));
      out_ += ':';
      if (auto r = write_unknown(v.payload()); !r) {
        return r;
      }
    } else {
      append_json_string(out_, c->name);
      out_ += ':';
      if (auto r = write(v.payload(), c->type); !r) {
        return r;
      }
    }
    out_ += '}';
    return {};
  }

  [[nodiscard]] result<void> write_oset(const value &v, const text_type &t) {
    if (v.kind() != value_kind::oset) {
      return mismatch(t, v);
    }
    const auto element = schema_->part(t.desc->element);
    if (!element) {
      return std::unexpected(element.error());
    }
    out_ += '[';
    bool first = true;
    for (const oset_entry &e : v.elements()) {
      const auto id = id_to_text(e.element);
      if (!id) {
        return std::unexpected(id.error());
      }
      out_ += first ? "" : ",";
      first = false;
      out_ += R"({"id":")" + *id + R"(","order":")" + to_hex(e.order_key) + R"(","value":)";
      if (auto r = write(e.v, *element); !r) {
        return r;
      }
      out_ += '}';
    }
    out_ += ']';
    return {};
  }

  const text_schema *schema_;
  std::string out_;
};

// ---- from_text ---------------------------------------------------------------------------------

bool is_plain_integer(std::string_view s) noexcept {
  if (!s.empty() && s.front() == '-') {
    s.remove_prefix(1);
  }
  if (s.empty() || (s.front() == '0' && s.size() > 1)) {
    return false;
  }
  return std::ranges::all_of(s, [](char c) noexcept { return c >= '0' && c <= '9'; });
}

bool is_lower_hex(std::string_view s) noexcept {
  return s.size() % 2 == 0 && std::ranges::all_of(s, [](char c) noexcept {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

/// "?<digits>" as a u32 number; nullopt for any other key.
std::optional<std::uint64_t> unknown_number(std::string_view key) noexcept {
  if (key.size() < 2 || key.front() != '?') {
    return std::nullopt;
  }
  const std::string_view digits = key.substr(1);
  if (!std::ranges::all_of(digits, [](char c) noexcept { return c >= '0' && c <= '9'; })) {
    return std::nullopt;
  }
  std::uint64_t n = 0;
  const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), n);
  if (ec != std::errc{} || end != digits.data() + digits.size()) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return n;
}

bool is_text(const json_value &j) noexcept { return j.kind == json_value::kind_type::string; }

class text_reader {
public:
  explicit text_reader(const text_schema &schema) noexcept : schema_(&schema) {}

  [[nodiscard]] result<value> build(const json_value &j, const type_id &type) {
    const auto t = schema_->lookup(type);
    if (!t) {
      return std::unexpected(t.error());
    }
    return build(j, *t);
  }

private:
  [[nodiscard]] result<value> build(const json_value &j, const text_type &t) {
    switch (t.kind) {
    case text_kind::boolean:
      if (j.kind != json_value::kind_type::boolean) {
        return syntax_error("bool expected");
      }
      return value::boolean(j.boolean);
    case text_kind::uint:
    case text_kind::sint:
      return build_int(j, t);
    case text_kind::f32:
    case text_kind::f64:
      return build_float(j, t);
    case text_kind::string:
    case text_kind::enumeration:
    case text_kind::bytes:
    case text_kind::id:
    case text_kind::digest:
      if (!is_text(j)) {
        return syntax_error("string expected");
      }
      return build_text_scalar(j.text, t);
    default:
      return build_container(j, t);
    }
  }

  [[nodiscard]] static result<value> build_int(const json_value &j, const text_type &t) {
    if ((j.kind != json_value::kind_type::number && !is_text(j)) || !is_plain_integer(j.text)) {
      return syntax_error("integer expected");
    }
    const bool negative = j.text.front() == '-';
    const std::string_view digits = std::string_view(j.text).substr(negative ? 1 : 0);
    std::uint64_t m = 0;
    const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), m);
    if (ec != std::errc{} || end != digits.data() + digits.size()) {
      return type_error(std::format("{} out of range", kind_name(t.kind)));
    }
    if (t.kind == text_kind::uint) {
      if ((negative && m != 0) || (t.bits < 64 && m >= (std::uint64_t{1} << t.bits))) {
        return type_error("uint out of range");
      }
      return value::uint(m);
    }
    const std::uint64_t half = std::uint64_t{1} << (t.bits - 1);
    if (!negative || m == 0) {
      if (m >= half) {
        return type_error("int out of range");
      }
      return value::uint(m);
    }
    if (m > half) {
      return type_error("int out of range");
    }
    return value::nint(m - 1);
  }

  [[nodiscard]] static result<value> build_float(const json_value &j, const text_type &t) {
    if (j.kind != json_value::kind_type::number) {
      return syntax_error("number expected");
    }
    const char *first = j.text.data();
    const char *last = j.text.data() + j.text.size();
    if (t.kind == text_kind::f32) {
      float x = 0;
      const auto [end, ec] = std::from_chars(first, last, x);
      if (ec != std::errc{} || end != last) {
        return type_error(std::format("out of f32 range: {}", j.text));
      }
      return value::f32(x);
    }
    double x = 0;
    const auto [end, ec] = std::from_chars(first, last, x);
    if (ec != std::errc{} || end != last) {
      return type_error(std::format("out of f64 range: {}", j.text));
    }
    return value::f64(x);
  }

  [[nodiscard]] static result<value> build_text_scalar(const std::string &s, const text_type &t) {
    switch (t.kind) {
    case text_kind::string:
      return value::string(s);
    case text_kind::bytes: {
      if (!is_lower_hex(s)) {
        return syntax_error("lowercase hex string expected");
      }
      const auto raw = from_hex(s);
      if (!raw) {
        return syntax_error("lowercase hex string expected");
      }
      return value::bytes(*raw);
    }
    case text_kind::id:
      return id_from_text(s).transform([](const id_bytes &b) { return value::id(b); });
    case text_kind::digest: {
      const std::size_t colon = s.find(':');
      const std::string_view name = std::string_view(s).substr(0, colon);
      const hash_algo algo = name == "sha256" ? hash_algo::sha256 : hash_algo::blake3;
      if (colon == std::string::npos || (name != "sha256" && name != "blake3")) {
        return syntax_error("digest text is sha256:<hex> or blake3:<hex>");
      }
      const std::string_view hex = std::string_view(s).substr(colon + 1);
      if (!is_lower_hex(hex) || hex.size() != 2 * sha256::digest_size) {
        return syntax_error("expected 32 bytes of lowercase hex");
      }
      const auto raw = from_hex(hex);
      digest d;
      d.algo = algo;
      std::ranges::copy(*raw, d.bytes.begin());
      return value::digest(d);
    }
    default:
      for (const enumerator_desc &e : t.desc->enumerators) {
        if (e.name == s) {
          return value::uint(e.no);
        }
      }
      return type_error(std::format("unknown enumerator '{}'", s));
    }
  }

  [[nodiscard]] static result<value> build_unknown(const json_value &j) {
    constexpr std::string_view slb = "slb:";
    if (!is_text(j) || !j.text.starts_with(slb) || !is_lower_hex(j.text.substr(slb.size()))) {
      return syntax_error(R"(unknown field value must be a string "slb:<hex>")");
    }
    const auto raw = from_hex(std::string_view(j.text).substr(slb.size()));
    if (!raw) {
      return std::unexpected(raw.error());
    }
    return decode(*raw);
  }

  [[nodiscard]] result<value> build_container(const json_value &j, const text_type &t) {
    switch (t.kind) {
    case text_kind::list:
      return build_list(j, t);
    case text_kind::map:
      return build_map(j, t);
    case text_kind::oset:
      return build_oset(j, t);
    case text_kind::variant:
      return build_variant(j, t);
    default:
      return build_struct(j, t);
    }
  }

  [[nodiscard]] result<value> build_list(const json_value &j, const text_type &t) {
    if (j.kind != json_value::kind_type::array) {
      return syntax_error("array expected");
    }
    const auto element = schema_->part(t.desc->element);
    if (!element) {
      return std::unexpected(element.error());
    }
    std::vector<value> items;
    items.reserve(j.items.size());
    for (const json_value &x : j.items) {
      auto v = build(x, *element);
      if (!v) {
        return v;
      }
      items.push_back(std::move(*v));
    }
    return value::list(std::move(items));
  }

  [[nodiscard]] result<value> build_map(const json_value &j, const text_type &t) {
    const auto pair = [](const json_value &p) noexcept {
      return p.kind == json_value::kind_type::array && p.items.size() == 2;
    };
    if (j.kind != json_value::kind_type::array || !std::ranges::all_of(j.items, pair)) {
      return syntax_error("array of [key, value] pairs expected");
    }
    const auto key = schema_->part(t.desc->key);
    const auto element = schema_->part(t.desc->element);
    if (!key || !element) {
      return std::unexpected(!key ? key.error() : element.error());
    }
    std::vector<map_entry> entries;
    entries.reserve(j.items.size());
    for (const json_value &p : j.items) {
      auto k = build(p.items[0], *key);
      if (!k) {
        return k;
      }
      auto v = build(p.items[1], *element);
      if (!v) {
        return v;
      }
      entries.push_back(map_entry{.key = std::move(*k), .v = std::move(*v)});
    }
    auto m = value::map(std::move(entries));
    if (!m) {
      return syntax_error(m.error().message);
    }
    return m;
  }

  [[nodiscard]] result<value> build_oset(const json_value &j, const text_type &t) {
    if (j.kind != json_value::kind_type::array) {
      return syntax_error("array expected");
    }
    const auto element = schema_->part(t.desc->element);
    if (!element) {
      return std::unexpected(element.error());
    }
    std::vector<oset_entry> entries;
    entries.reserve(j.items.size());
    for (const json_value &row : j.items) {
      if (row.kind != json_value::kind_type::object || row.members.size() != 3 ||
          row.members[0].key != "id" || row.members[1].key != "order" ||
          row.members[2].key != "value") {
        return syntax_error(R"(oset entries are {"id", "order", "value"})");
      }
      if (!is_text(row.members[0].v)) {
        return syntax_error("element id text expected");
      }
      const auto eid = id_from_text(row.members[0].v.text);
      if (!eid) {
        return std::unexpected(eid.error());
      }
      const json_value &order = row.members[1].v;
      if (!is_text(order) || !is_lower_hex(order.text)) {
        return syntax_error("lowercase hex string expected");
      }
      auto okey = from_hex(order.text);
      auto v = build(row.members[2].v, *element);
      if (!v) {
        return v;
      }
      entries.push_back(
          oset_entry{.element = *eid, .order_key = std::move(*okey), .v = std::move(*v)});
    }
    auto o = value::oset(std::move(entries));
    if (!o) {
      return syntax_error(o.error().message);
    }
    return o;
  }

  [[nodiscard]] result<value> build_variant(const json_value &j, const text_type &t) {
    if (j.kind != json_value::kind_type::object) {
      return syntax_error("object expected");
    }
    if (j.members.size() != 1) {
      return syntax_error("a variant is an object with exactly one key");
    }
    const json_member &m = j.members[0];
    for (const case_desc &c : t.desc->cases) {
      if (c.name == m.key) {
        auto v = build(m.v, c.type);
        if (!v) {
          return v;
        }
        return value::variant(c.no, std::move(*v));
      }
    }
    const auto no = unknown_number(m.key);
    if (!no) {
      return type_error(std::format("unknown variant case '{}'", m.key));
    }
    if (*no > max_u32) {
      return syntax_error("variant case outside u32");
    }
    auto v = build_unknown(m.v);
    if (!v) {
      return v;
    }
    return value::variant(static_cast<std::uint32_t>(*no), std::move(*v));
  }

  [[nodiscard]] result<value> build_struct(const json_value &j, const text_type &t) {
    if (j.kind != json_value::kind_type::object) {
      return syntax_error("object expected");
    }
    std::vector<field> fields;
    fields.reserve(j.members.size());
    for (const json_member &m : j.members) {
      const auto named = std::ranges::find(t.desc->fields, m.key, &field_desc::name);
      if (named != t.desc->fields.end()) {
        auto v = build(m.v, named->type);
        if (!v) {
          return v;
        }
        fields.push_back(field{.no = named->no, .v = std::move(*v)});
        continue;
      }
      const auto no = unknown_number(m.key);
      if (!no) {
        return type_error(std::format("unknown field name '{}'", m.key));
      }
      if (*no > max_u32) {
        return syntax_error("field number outside u32");
      }
      auto v = build_unknown(m.v);
      if (!v) {
        return v;
      }
      fields.push_back(field{.no = static_cast<std::uint32_t>(*no), .v = std::move(*v)});
    }
    auto s = value::struct_(std::move(fields));
    if (!s) {
      return syntax_error(s.error().message);
    }
    return s;
  }

  const text_schema *schema_;
};

} // namespace

result<std::string> to_text(const value &v, type_id type, const schema_registry &schemas) {
  const text_schema schema(schemas);
  text_writer w(schema);
  if (auto r = w.write(v, type); !r) {
    return std::unexpected(r.error());
  }
  return w.take();
}

result<value> from_text(std::string_view text, type_id type, const schema_registry &schemas) {
  const auto tree = parse_json(text);
  if (!tree) {
    return std::unexpected(tree.error());
  }
  const text_schema schema(schemas);
  text_reader r(schema);
  auto v = r.build(*tree, type);
  if (!v) {
    return v;
  }
  const auto again = to_text(*v, type, schemas);
  if (!again) {
    return std::unexpected(again.error());
  }
  if (*again != text) {
    return syntax_error("valid JSON but not the canonical spelling");
  }
  return v;
}

} // namespace sl::fabric
