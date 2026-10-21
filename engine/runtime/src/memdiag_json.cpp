// sl-runtime-memreport/1 (WO-085, SL-ARCH-003 ARCH3-R053, schemas/runtime/memreport.schema.json).
// to_json writes the schema's key order with two-space indentation (the layout of
// schemas/runtime/examples/memreport.json) from sorted copies of the four arrays. from_json parses
// strict JSON (RFC 8259; integers only in this format) into a small value tree, then maps it with
// the schema's required keys, types and ranges; the first mismatch is invalid_argument naming its
// JSON pointer. Nothing throws: allocation failure terminates (ADR-0023).

#include "sl/core/error.hpp"
#include "sl/core/log.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/memdiag.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

namespace sl::runtime::mem {

namespace {

// --- writing ---

void put_key(std::string &out, int indent, std::string_view key) {
  out.append(static_cast<std::size_t>(indent), ' ');
  sl::log::append_json_string(out, key);
  out += ": ";
}

void put_uint(std::string &out, int indent, std::string_view key, std::uint64_t value, bool last) {
  put_key(out, indent, key);
  out += std::to_string(value);
  out += last ? "\n" : ",\n";
}

void put_text(std::string &out, int indent, std::string_view key, std::string_view value,
              bool last) {
  put_key(out, indent, key);
  sl::log::append_json_string(out, value);
  out += last ? "\n" : ",\n";
}

// Writes `"key": [ {...}, ... ]` with one object per element, written by `body`.
template <class T, class Body>
void put_array(std::string &out, std::string_view key, const std::vector<T> &items, Body body) {
  put_key(out, 2, key);
  if (items.empty()) {
    out += "[],\n";
    return;
  }
  out += "[\n";
  for (std::size_t i = 0; i < items.size(); ++i) {
    out += "    {\n";
    body(items[i]);
    out += i + 1 == items.size() ? "    }\n" : "    },\n";
  }
  out += "  ],\n";
}

// --- parsing: a value tree ---

struct json_member;

struct json_value {
  enum class kind : std::uint8_t { null, boolean, integer, number, string, array, object };
  kind type = kind::null;
  bool flag = false;
  std::uint64_t integer = 0; // kind::integer: a non-negative integer that fits 64 bits
  std::string text;          // kind::string
  std::vector<json_value> items;
  std::vector<json_member> members;
};

struct json_member {
  std::string key;
  json_value value;
};

constexpr int max_depth = 16;

class parser {
public:
  explicit parser(std::string_view text) noexcept : text_(text) {}

  [[nodiscard]] bool document(json_value &out) {
    skip_space();
    if (!value(out, 0)) {
      return false;
    }
    skip_space();
    if (pos_ != text_.size()) {
      fail("trailing characters");
      return false;
    }
    return true;
  }

  [[nodiscard]] const std::string &error() const noexcept { return error_; }

private:
  bool fail(std::string_view why) {
    if (error_.empty()) {
      error_ = "malformed JSON at offset " + std::to_string(pos_) + ": " + std::string(why);
    }
    return false;
  }

  void skip_space() noexcept {
    while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' ||
                                   text_[pos_] == '\n' || text_[pos_] == '\r')) {
      ++pos_;
    }
  }

  bool literal(std::string_view word) {
    if (text_.substr(pos_, word.size()) != word) {
      return fail("unknown literal");
    }
    pos_ += word.size();
    return true;
  }

  bool value(json_value &out, int depth) {
    if (depth > max_depth) {
      return fail("nesting deeper than 16");
    }
    if (pos_ >= text_.size()) {
      return fail("unexpected end");
    }
    const char c = text_[pos_];
    switch (c) {
    case '{':
      return object(out, depth);
    case '[':
      return array(out, depth);
    case '"':
      out.type = json_value::kind::string;
      return string(out.text);
    case 't':
      out.type = json_value::kind::boolean;
      out.flag = true;
      return literal("true");
    case 'f':
      out.type = json_value::kind::boolean;
      return literal("false");
    case 'n':
      out.type = json_value::kind::null;
      return literal("null");
    default:
      return number(out);
    }
  }

  bool object(json_value &out, int depth) {
    out.type = json_value::kind::object;
    ++pos_; // {
    skip_space();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      ++pos_;
      return true;
    }
    while (true) {
      skip_space();
      if (pos_ >= text_.size() || text_[pos_] != '"') {
        return fail("expected a key");
      }
      json_member m;
      if (!string(m.key)) {
        return false;
      }
      skip_space();
      if (pos_ >= text_.size() || text_[pos_] != ':') {
        return fail("expected ':'");
      }
      ++pos_;
      skip_space();
      if (!value(m.value, depth + 1)) {
        return false;
      }
      out.members.push_back(std::move(m));
      skip_space();
      if (pos_ < text_.size() && text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (pos_ < text_.size() && text_[pos_] == '}') {
        ++pos_;
        return true;
      }
      return fail("expected ',' or '}'");
    }
  }

  bool array(json_value &out, int depth) {
    out.type = json_value::kind::array;
    ++pos_; // [
    skip_space();
    if (pos_ < text_.size() && text_[pos_] == ']') {
      ++pos_;
      return true;
    }
    while (true) {
      skip_space();
      json_value item;
      if (!value(item, depth + 1)) {
        return false;
      }
      out.items.push_back(std::move(item));
      skip_space();
      if (pos_ < text_.size() && text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (pos_ < text_.size() && text_[pos_] == ']') {
        ++pos_;
        return true;
      }
      return fail("expected ',' or ']'");
    }
  }

  static void put_utf8(std::string &out, std::uint32_t cp) {
    if (cp < 0x80U) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800U) {
      out += static_cast<char>(0xC0U | (cp >> 6U));
      out += static_cast<char>(0x80U | (cp & 0x3FU));
    } else if (cp < 0x10000U) {
      out += static_cast<char>(0xE0U | (cp >> 12U));
      out += static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
      out += static_cast<char>(0x80U | (cp & 0x3FU));
    } else {
      out += static_cast<char>(0xF0U | (cp >> 18U));
      out += static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU));
      out += static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
      out += static_cast<char>(0x80U | (cp & 0x3FU));
    }
  }

  bool hex4(std::uint32_t &out) {
    if (text_.size() - pos_ < 4) {
      return fail("short \\u escape");
    }
    out = 0;
    for (const char c : text_.substr(pos_, 4)) {
      std::uint32_t digit = 0;
      if (c >= '0' && c <= '9') {
        digit = static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = static_cast<std::uint32_t>(c - 'a') + 10U;
      } else if (c >= 'A' && c <= 'F') {
        digit = static_cast<std::uint32_t>(c - 'A') + 10U;
      } else {
        return fail("bad \\u escape");
      }
      out = (out << 4U) | digit;
    }
    pos_ += 4;
    return true;
  }

  bool escape(std::string &out) {
    if (pos_ >= text_.size()) {
      return fail("unterminated escape");
    }
    const char e = text_[pos_++];
    switch (e) {
    case '"':
    case '\\':
    case '/':
      out += e;
      return true;
    case 'b':
      out += '\b';
      return true;
    case 'f':
      out += '\f';
      return true;
    case 'n':
      out += '\n';
      return true;
    case 'r':
      out += '\r';
      return true;
    case 't':
      out += '\t';
      return true;
    case 'u': {
      std::uint32_t cp = 0;
      if (!hex4(cp)) {
        return false;
      }
      if (cp >= 0xD800U && cp <= 0xDBFFU) {
        std::uint32_t low = 0;
        if (text_.substr(pos_, 2) != "\\u") {
          return fail("lone high surrogate");
        }
        pos_ += 2;
        if (!hex4(low)) {
          return false;
        }
        if (low < 0xDC00U || low > 0xDFFFU) {
          return fail("bad low surrogate");
        }
        cp = 0x10000U + ((cp - 0xD800U) << 10U) + (low - 0xDC00U);
      } else if (cp >= 0xDC00U && cp <= 0xDFFFU) {
        return fail("lone low surrogate");
      }
      put_utf8(out, cp);
      return true;
    }
    default:
      return fail("unknown escape");
    }
  }

  bool string(std::string &out) {
    ++pos_; // opening quote
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == '"') {
        ++pos_;
        return true;
      }
      if (static_cast<unsigned char>(c) < 0x20U) {
        return fail("control character in a string");
      }
      ++pos_;
      if (c == '\\') {
        if (!escape(out)) {
          return false;
        }
      } else {
        out += c;
      }
    }
    return fail("unterminated string");
  }

  bool number(json_value &out) {
    const std::size_t start = pos_;
    bool integral = true;
    if (text_[pos_] == '-') {
      integral = false; // negative: never valid in this format
      ++pos_;
    }
    const std::size_t digits = pos_;
    while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
      ++pos_;
    }
    if (pos_ == digits) {
      return fail("unexpected character");
    }
    if (text_[digits] == '0' && pos_ - digits > 1) {
      return fail("leading zero");
    }
    const auto more_digits = [this] {
      const std::size_t from = pos_;
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        ++pos_;
      }
      return pos_ > from;
    };
    if (pos_ < text_.size() && text_[pos_] == '.') {
      integral = false;
      ++pos_;
      if (!more_digits()) {
        return fail("digits expected after '.'");
      }
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      integral = false;
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
        ++pos_;
      }
      if (!more_digits()) {
        return fail("digits expected in the exponent");
      }
    }
    out.type = json_value::kind::number;
    if (integral) {
      const std::string_view span = text_.substr(start, pos_ - start);
      std::uint64_t v = 0;
      const auto [end, ec] = std::from_chars(span.data(), span.data() + span.size(), v);
      if (ec == std::errc{} && end == span.data() + span.size()) {
        out.type = json_value::kind::integer;
        out.integer = v;
      }
    }
    return true;
  }

  std::string_view text_;
  std::size_t pos_ = 0;
  std::string error_;
};

// --- parsing: the mapping onto memory_report ---

// The JSON pointer `path`/`key`.
std::string join(const std::string &path, std::string_view key) {
  std::string out = path;
  out += '/';
  out += key;
  return out;
}

class mapper {
public:
  // The value of `key` in object `obj` at `path`; nullptr (and an error) when missing.
  const json_value *member(const json_value &obj, std::string_view key, const std::string &path) {
    const auto found =
        std::ranges::find_if(obj.members, [&](const json_member &m) { return m.key == key; });
    if (found != obj.members.end()) {
      return &found->value;
    }
    fail(join(path, key), "missing required key");
    return nullptr;
  }

  // `obj` is an object with exactly `keys`, each once.
  bool exact_keys(const json_value &obj, std::initializer_list<std::string_view> keys,
                  const std::string &path) {
    if (obj.type != json_value::kind::object) {
      return fail(path.empty() ? "/" : path, "expected an object");
    }
    for (std::size_t i = 0; i < obj.members.size(); ++i) {
      const std::string &key = obj.members[i].key;
      if (std::ranges::find(keys, std::string_view(key)) == keys.end()) {
        return fail(join(path, key), "unexpected key");
      }
      for (std::size_t j = 0; j < i; ++j) {
        if (obj.members[j].key == key) {
          return fail(join(path, key), "duplicate key");
        }
      }
    }
    return std::ranges::all_of(
        keys, [&](std::string_view key) { return member(obj, key, path) != nullptr; });
  }

  bool uint_at(const json_value &obj, std::string_view key, const std::string &path,
               std::uint64_t min, std::uint64_t max, std::uint64_t &out) {
    const json_value *v = member(obj, key, path);
    if (v == nullptr) {
      return false;
    }
    const std::string at = join(path, key);
    if (v->type != json_value::kind::integer) {
      return fail(at, "expected a non-negative integer of at most 64 bits");
    }
    if (v->integer < min || v->integer > max) {
      return fail(at, std::to_string(v->integer) + " is outside " + std::to_string(min) + ".." +
                          std::to_string(max));
    }
    out = v->integer;
    return true;
  }

  bool text_at(const json_value &obj, std::string_view key, const std::string &path,
               std::string &out) {
    const json_value *v = member(obj, key, path);
    if (v == nullptr) {
      return false;
    }
    const std::string at = join(path, key);
    if (v->type != json_value::kind::string) {
      return fail(at, "expected a string");
    }
    if (v->text.empty()) {
      return fail(at, "expected a non-empty string");
    }
    out = v->text;
    return true;
  }

  const json_value *array_at(const json_value &obj, std::string_view key, const std::string &path) {
    const json_value *v = member(obj, key, path);
    if (v != nullptr && v->type != json_value::kind::array) {
      fail(join(path, key), "expected an array");
      return nullptr;
    }
    return v;
  }

  bool fail(const std::string &path, std::string_view why) {
    if (error_.empty()) {
      error_ = "key " + path + ": " + std::string(why);
    }
    return false;
  }

  [[nodiscard]] const std::string &error() const noexcept { return error_; }

private:
  std::string error_;
};

constexpr std::uint64_t u64_max = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t u32_max = std::numeric_limits<std::uint32_t>::max();

bool map_tag(mapper &m, const json_value &v, const std::string &at, tag_stats &t) {
  return m.exact_keys(
             v, {"tag", "live_bytes", "live_blocks", "peak_bytes", "allocs", "frees", "failures"},
             at) &&
         m.text_at(v, "tag", at, t.tag) &&
         m.uint_at(v, "live_bytes", at, 0, u64_max, t.live_bytes) &&
         m.uint_at(v, "live_blocks", at, 0, u64_max, t.live_blocks) &&
         m.uint_at(v, "peak_bytes", at, 0, u64_max, t.peak_bytes) &&
         m.uint_at(v, "allocs", at, 0, u64_max, t.allocs) &&
         m.uint_at(v, "frees", at, 0, u64_max, t.frees) &&
         m.uint_at(v, "failures", at, 0, u64_max, t.failures);
}

bool map_allocator(mapper &m, const json_value &v, const std::string &at, allocator_stats &a) {
  std::uint64_t permille = 0;
  const bool ok =
      m.exact_keys(v, {"name", "free_bytes", "largest_free_run", "fragmentation_permille"}, at) &&
      m.text_at(v, "name", at, a.name) &&
      m.uint_at(v, "free_bytes", at, 0, u64_max, a.free_bytes) &&
      m.uint_at(v, "largest_free_run", at, 0, u64_max, a.largest_free_run) &&
      m.uint_at(v, "fragmentation_permille", at, 0, 1000, permille);
  a.fragmentation_permille = static_cast<std::uint32_t>(permille);
  return ok;
}

bool map_external(mapper &m, const json_value &v, const std::string &at, external_stats &e) {
  return m.exact_keys(v, {"tag", "bytes"}, at) && m.text_at(v, "tag", at, e.tag) &&
         m.uint_at(v, "bytes", at, 0, u64_max, e.bytes);
}

bool map_leak(mapper &m, const json_value &v, const std::string &at, leak_entry &l) {
  return m.exact_keys(v, {"tag", "site", "size"}, at) && m.text_at(v, "tag", at, l.tag) &&
         m.text_at(v, "site", at, l.site) && m.uint_at(v, "size", at, 1, u64_max, l.size);
}

// Maps every element of array `key` with `one`.
template <class T, class One>
bool map_array(mapper &m, const json_value &root, std::string_view key, std::vector<T> &out,
               One one) {
  const json_value *items = m.array_at(root, key, "");
  if (items == nullptr) {
    return false;
  }
  out.reserve(items->items.size());
  for (std::size_t i = 0; i < items->items.size(); ++i) {
    T value{};
    if (!one(m, items->items[i], join(join("", key), std::to_string(i)), value)) {
      return false;
    }
    out.push_back(std::move(value));
  }
  return true;
}

} // namespace

std::string to_json(const memory_report &report) {
  std::vector<tag_stats> tags = report.tags;
  std::ranges::stable_sort(tags, {}, &tag_stats::tag);
  std::vector<allocator_stats> allocators = report.allocators;
  std::ranges::stable_sort(allocators, {}, &allocator_stats::name);
  std::vector<external_stats> external = report.external;
  std::ranges::stable_sort(external, {}, &external_stats::tag);
  std::vector<leak_entry> leaks = report.leaks;
  std::ranges::sort(leaks, [](const leak_entry &a, const leak_entry &b) {
    return std::tie(a.tag, a.site, a.size) < std::tie(b.tag, b.site, b.size);
  });

  std::string out = "{\n";
  put_text(out, 2, "format", memreport_format, false);
  put_uint(out, 2, "instance", report.instance, false);
  put_uint(out, 2, "frame", report.frame, false);
  put_array(out, "tags", tags, [&out](const tag_stats &t) {
    put_text(out, 6, "tag", t.tag, false);
    put_uint(out, 6, "live_bytes", t.live_bytes, false);
    put_uint(out, 6, "live_blocks", t.live_blocks, false);
    put_uint(out, 6, "peak_bytes", t.peak_bytes, false);
    put_uint(out, 6, "allocs", t.allocs, false);
    put_uint(out, 6, "frees", t.frees, false);
    put_uint(out, 6, "failures", t.failures, true);
  });
  put_array(out, "allocators", allocators, [&out](const allocator_stats &a) {
    put_text(out, 6, "name", a.name, false);
    put_uint(out, 6, "free_bytes", a.free_bytes, false);
    put_uint(out, 6, "largest_free_run", a.largest_free_run, false);
    put_uint(out, 6, "fragmentation_permille", a.fragmentation_permille, true);
  });
  put_array(out, "external", external, [&out](const external_stats &e) {
    put_text(out, 6, "tag", e.tag, false);
    put_uint(out, 6, "bytes", e.bytes, true);
  });
  put_array(out, "leaks", leaks, [&out](const leak_entry &l) {
    put_text(out, 6, "tag", l.tag, false);
    put_text(out, 6, "site", l.site, false);
    put_uint(out, 6, "size", l.size, true);
  });
  put_uint(out, 2, "upstream_allocs_last_frame", report.upstream_allocs_last_frame, true);
  out += "}\n";
  return out;
}

result<memory_report> from_json(std::string_view text) {
  json_value root;
  parser p(text);
  if (!p.document(root)) {
    return make_error(errc::invalid_argument, "from_json: " + p.error());
  }
  mapper m;
  memory_report report;
  std::string format;
  std::uint64_t instance = 0;
  const bool ok =
      m.exact_keys(root,
                   {"format", "instance", "frame", "tags", "allocators", "external", "leaks",
                    "upstream_allocs_last_frame"},
                   "") &&
      m.text_at(root, "format", "", format) &&
      (format == memreport_format || m.fail("/format", "expected \"sl-runtime-memreport/1\"")) &&
      m.uint_at(root, "instance", "", 1, u32_max, instance) &&
      m.uint_at(root, "frame", "", 0, u64_max, report.frame) &&
      map_array(m, root, "tags", report.tags, map_tag) &&
      map_array(m, root, "allocators", report.allocators, map_allocator) &&
      map_array(m, root, "external", report.external, map_external) &&
      map_array(m, root, "leaks", report.leaks, map_leak) &&
      m.uint_at(root, "upstream_allocs_last_frame", "", 0, u64_max,
                report.upstream_allocs_last_frame);
  if (!ok) {
    return make_error(errc::invalid_argument, "from_json: " + m.error());
  }
  report.instance = static_cast<std::uint32_t>(instance);
  return report;
}

} // namespace sl::runtime::mem
