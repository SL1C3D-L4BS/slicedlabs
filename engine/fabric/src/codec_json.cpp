// Strict JSON reader for canonical text and the corpus files (WO-041, RFC 8259). See codec.hpp.

#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/codec.hpp"

namespace sl::fabric {

namespace {

constexpr std::size_t max_json_depth = 512;

class json_parser {
public:
  explicit json_parser(std::string_view text) noexcept : s_(text) {}

  [[nodiscard]] result<json_value> document() {
    skip_ws();
    auto v = parse(0);
    if (!v) {
      return v;
    }
    skip_ws();
    if (pos_ != s_.size()) {
      return fail("extra data");
    }
    return v;
  }

private:
  [[nodiscard]] std::unexpected<error> fail(std::string_view what) const {
    return make_error(errc::text_syntax,
                      std::format("R-TXT-SYNTAX: not JSON: {} at offset {}", what, pos_));
  }

  [[nodiscard]] bool at_end() const noexcept { return pos_ >= s_.size(); }
  [[nodiscard]] char peek() const noexcept { return at_end() ? '\0' : s_[pos_]; }

  void skip_ws() noexcept {
    while (!at_end() &&
           (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) {
      ++pos_;
    }
  }

  [[nodiscard]] bool literal(std::string_view word) noexcept {
    if (s_.substr(pos_).starts_with(word)) {
      pos_ += word.size();
      return true;
    }
    return false;
  }

  [[nodiscard]] result<json_value> parse(std::size_t depth) {
    json_value v;
    switch (peek()) {
    case '{':
      return parse_object(depth);
    case '[':
      return parse_array(depth);
    case '"': {
      auto s = parse_string();
      if (!s) {
        return std::unexpected(s.error());
      }
      v.kind = json_value::kind_type::string;
      v.text = std::move(*s);
      return v;
    }
    case 't':
    case 'f':
      if (literal("true")) {
        v.kind = json_value::kind_type::boolean;
        v.boolean = true;
        return v;
      }
      if (literal("false")) {
        v.kind = json_value::kind_type::boolean;
        return v;
      }
      return fail("bad literal");
    case 'n':
      if (literal("null")) {
        return v;
      }
      return fail("bad literal");
    default:
      return parse_number();
    }
  }

  [[nodiscard]] result<json_value> parse_array(std::size_t depth) {
    if (depth + 1 > max_json_depth) {
      return fail("nesting over 512");
    }
    ++pos_; // '['
    json_value v;
    v.kind = json_value::kind_type::array;
    skip_ws();
    if (peek() == ']') {
      ++pos_;
      return v;
    }
    while (true) {
      skip_ws();
      auto item = parse(depth + 1);
      if (!item) {
        return item;
      }
      v.items.push_back(std::move(*item));
      skip_ws();
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      if (peek() == ']') {
        ++pos_;
        return v;
      }
      return fail("expected ',' or ']'");
    }
  }

  [[nodiscard]] result<json_value> parse_object(std::size_t depth) {
    if (depth + 1 > max_json_depth) {
      return fail("nesting over 512");
    }
    ++pos_; // '{'
    json_value v;
    v.kind = json_value::kind_type::object;
    skip_ws();
    if (peek() == '}') {
      ++pos_;
      return v;
    }
    while (true) {
      skip_ws();
      if (peek() != '"') {
        return fail("expected a member name");
      }
      auto key = parse_string();
      if (!key) {
        return std::unexpected(key.error());
      }
      if (v.find(*key) != nullptr) {
        return fail(std::format("duplicate object key '{}'", *key));
      }
      skip_ws();
      if (peek() != ':') {
        return fail("expected ':'");
      }
      ++pos_;
      skip_ws();
      auto item = parse(depth + 1);
      if (!item) {
        return item;
      }
      v.members.push_back(json_member{.key = std::move(*key), .v = std::move(*item)});
      skip_ws();
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      if (peek() == '}') {
        ++pos_;
        return v;
      }
      return fail("expected ',' or '}'");
    }
  }

  [[nodiscard]] result<unsigned> hex4() {
    if (s_.size() - pos_ < 4) {
      return fail("short \\u escape");
    }
    unsigned v = 0;
    for (std::size_t i = 0; i < 4; ++i) {
      const char c = s_[pos_ + i];
      unsigned d = 0;
      if (c >= '0' && c <= '9') {
        d = static_cast<unsigned>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        d = static_cast<unsigned>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        d = static_cast<unsigned>(c - 'A' + 10);
      } else {
        return fail("bad \\u escape");
      }
      v = (v << 4U) | d;
    }
    pos_ += 4;
    return v;
  }

  static void append_utf8(std::string &out, unsigned cp) {
    const auto put = [&out](unsigned b) {
      out += static_cast<char>(static_cast<unsigned char>(b));
    };
    if (cp < 0x80U) {
      put(cp);
    } else if (cp < 0x800U) {
      put(0xC0U | (cp >> 6U));
      put(0x80U | (cp & 0x3FU));
    } else if (cp < 0x10000U) {
      put(0xE0U | (cp >> 12U));
      put(0x80U | ((cp >> 6U) & 0x3FU));
      put(0x80U | (cp & 0x3FU));
    } else {
      put(0xF0U | (cp >> 18U));
      put(0x80U | ((cp >> 12U) & 0x3FU));
      put(0x80U | ((cp >> 6U) & 0x3FU));
      put(0x80U | (cp & 0x3FU));
    }
  }

  [[nodiscard]] result<void> escape(std::string &out) {
    if (at_end()) {
      return fail("unterminated string");
    }
    const char c = s_[pos_++];
    switch (c) {
    case '"':
    case '\\':
    case '/':
      out += c;
      return {};
    case 'b':
      out += '\b';
      return {};
    case 'f':
      out += '\f';
      return {};
    case 'n':
      out += '\n';
      return {};
    case 'r':
      out += '\r';
      return {};
    case 't':
      out += '\t';
      return {};
    case 'u':
      break;
    default:
      return fail("bad escape");
    }
    const auto hi = hex4();
    if (!hi) {
      return std::unexpected(hi.error());
    }
    unsigned cp = *hi;
    if (cp >= 0xDC00U && cp <= 0xDFFFU) {
      return fail("lone low surrogate");
    }
    if (cp >= 0xD800U && cp <= 0xDBFFU) {
      if (!literal("\\u")) {
        return fail("lone high surrogate");
      }
      const auto lo = hex4();
      if (!lo) {
        return std::unexpected(lo.error());
      }
      if (*lo < 0xDC00U || *lo > 0xDFFFU) {
        return fail("lone high surrogate");
      }
      cp = 0x10000U + ((cp - 0xD800U) << 10U) + (*lo - 0xDC00U);
    }
    append_utf8(out, cp);
    return {};
  }

  [[nodiscard]] result<std::string> parse_string() {
    ++pos_; // '"'
    std::string out;
    while (true) {
      if (at_end()) {
        return fail("unterminated string");
      }
      const char c = s_[pos_];
      if (c == '"') {
        ++pos_;
        break;
      }
      if (static_cast<unsigned char>(c) < 0x20U) {
        return fail("control character in string");
      }
      ++pos_;
      if (c == '\\') {
        if (auto r = escape(out); !r) {
          return std::unexpected(r.error());
        }
        continue;
      }
      out += c;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): chars viewed as their bytes
    const byte_span bytes(reinterpret_cast<const std::uint8_t *>(out.data()), out.size());
    if (!detail::utf8_valid(bytes)) {
      return fail("ill-formed UTF-8 in string");
    }
    return out;
  }

  [[nodiscard]] static bool digit(char c) noexcept { return c >= '0' && c <= '9'; }

  [[nodiscard]] result<json_value> parse_number() {
    const std::size_t start = pos_;
    if (peek() == '-') {
      ++pos_;
    }
    if (peek() == '0') {
      ++pos_;
    } else if (digit(peek())) {
      while (digit(peek())) {
        ++pos_;
      }
    } else {
      return fail("unexpected character");
    }
    if (peek() == '.') {
      ++pos_;
      if (!digit(peek())) {
        return fail("bad number");
      }
      while (digit(peek())) {
        ++pos_;
      }
    }
    if (peek() == 'e' || peek() == 'E') {
      ++pos_;
      if (peek() == '+' || peek() == '-') {
        ++pos_;
      }
      if (!digit(peek())) {
        return fail("bad number");
      }
      while (digit(peek())) {
        ++pos_;
      }
    }
    json_value v;
    v.kind = json_value::kind_type::number;
    v.text = std::string(s_.substr(start, pos_ - start));
    return v;
  }

  std::string_view s_;
  std::size_t pos_ = 0;
};

} // namespace

const json_value *json_value::find(std::string_view key) const noexcept {
  for (const json_member &m : members) {
    if (m.key == key) {
      return &m.v;
    }
  }
  return nullptr;
}

result<json_value> parse_json(std::string_view text) {
  json_parser p(text);
  return p.document();
}

} // namespace sl::fabric
