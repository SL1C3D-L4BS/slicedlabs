// Canonical values (WO-039, SL-ARCH-001 ARCH1-R14 to ARCH1-R16, ADR-0012). See value.hpp.

#include "sl/fabric/value.hpp"

#include "sl/core/assert.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <format>
#include <limits>
#include <variant>

namespace sl::fabric {

namespace detail {

using value_payload = std::variant<std::monostate, std::string, std::vector<std::uint8_t>, id_bytes,
                                   fabric::digest, std::vector<value>, std::vector<map_entry>,
                                   std::vector<field>, value, std::vector<oset_entry>>;

/// One immutable node. `word` holds bool, uint, the nint magnitude, the float bits or the variant
/// case; `data` the rest.
struct value_node {
  value_kind kind = value_kind::bool_;
  std::uint8_t depth = 0;
  std::uint64_t word = 0;
  value_payload data;
};

struct value_access {
  static value make(value_kind kind, std::uint64_t word, value_payload data, std::size_t depth) {
    auto n = std::make_shared<value_node>();
    n->kind = kind;
    n->word = word;
    n->depth = static_cast<std::uint8_t>(depth);
    n->data = std::move(data);
    return value{std::shared_ptr<const value_node>(std::move(n))};
  }
  static const value_node *node(const value &v) noexcept { return v.node_.get(); }
};

} // namespace detail

namespace {

using detail::value_access;
using detail::value_node;

std::unexpected<error> encode_invalid(std::string_view rule, std::string_view why) {
  return make_error(errc::encode_invalid, std::format("{}: {}", rule, why));
}

std::unexpected<error> limit_error(std::string_view what, std::size_t n) {
  return encode_invalid("R-SER-LIMIT", std::format("{} of {} exceeds 2^24", what, n));
}

/// RFC 3629 well-formed UTF-8 (no overlongs, surrogates or code points above U+10FFFF) with no
/// NUL byte; the offset of the first bad byte otherwise.
std::optional<std::size_t> utf8_violation(std::string_view s) noexcept {
  const std::size_t n = s.size();
  const auto byte = [&s](std::size_t i) noexcept { return static_cast<unsigned char>(s[i]); };
  std::size_t i = 0;
  while (i < n) {
    const unsigned c = byte(i);
    if (c == 0) {
      return i;
    }
    if (c < 0x80U) {
      ++i;
      continue;
    }
    std::size_t len = 0;
    unsigned lo = 0x80U;
    unsigned hi = 0xBFU;
    if (c >= 0xC2U && c <= 0xDFU) {
      len = 2;
    } else if (c >= 0xE0U && c <= 0xEFU) {
      len = 3;
      lo = c == 0xE0U ? 0xA0U : 0x80U;
      hi = c == 0xEDU ? 0x9FU : 0xBFU;
    } else if (c >= 0xF0U && c <= 0xF4U) {
      len = 4;
      lo = c == 0xF0U ? 0x90U : 0x80U;
      hi = c == 0xF4U ? 0x8FU : 0xBFU;
    } else {
      return i;
    }
    if (n - i < len) {
      return i;
    }
    for (std::size_t k = 1; k < len; ++k) {
      const unsigned b = byte(i + k);
      const unsigned min = k == 1 ? lo : 0x80U;
      const unsigned max = k == 1 ? hi : 0xBFU;
      if (b < min || b > max) {
        return i;
      }
    }
    i += len;
  }
  return std::nullopt;
}

template <class T> const T &payload_of(const value &v) noexcept {
  const value_node *n = value_access::node(v);
  SL_ASSERT(n != nullptr, "value accessor used on the wrong kind");
  const T *p = n == nullptr ? nullptr : std::get_if<T>(&n->data);
  SL_ASSERT(p != nullptr, "value accessor used on the wrong kind");
  if (p == nullptr) [[unlikely]] {
    std::abort(); // an installed assert handler returned; never dereference null
  }
  return *p;
}

const value_node &node_of(const value &v, value_kind k) noexcept {
  const value_node *n = value_access::node(v);
  const bool matches = n != nullptr && n->kind == k;
  SL_ASSERT(matches, "value accessor used on the wrong kind");
  if (!matches) [[unlikely]] {
    std::abort(); // an installed assert handler returned; never dereference null
  }
  return *n;
}

// Container construction from items already in canonical order.

std::size_t child_depth(std::span<const value> items) noexcept {
  std::size_t d = 0;
  for (const value &v : items) {
    d = std::max(d, v.depth());
  }
  return d;
}

std::size_t child_depth(std::span<const map_entry> entries) noexcept {
  std::size_t d = 0;
  for (const map_entry &e : entries) {
    d = std::max(d, e.v.depth());
  }
  return d;
}

std::size_t child_depth(std::span<const field> fields) noexcept {
  std::size_t d = 0;
  for (const field &f : fields) {
    d = std::max(d, f.v.depth());
  }
  return d;
}

std::size_t child_depth(std::span<const oset_entry> entries) noexcept {
  std::size_t d = 0;
  for (const oset_entry &e : entries) {
    d = std::max(d, e.v.depth());
  }
  return d;
}

template <class Vec> result<value> make_container(value_kind kind, Vec items) {
  if (items.size() > max_container_items) {
    return limit_error("item count", items.size());
  }
  const std::size_t depth = child_depth(std::span(std::as_const(items))) + 1;
  if (depth > max_value_depth) {
    return encode_invalid("R-SER-LIMIT", std::format("{} nested containers exceed 64", depth));
  }
  return value_access::make(kind, 0, std::move(items), depth);
}

result<value> make_variant(std::uint32_t case_no, value payload) {
  const std::size_t depth = payload.depth() + 1;
  if (depth > max_value_depth) {
    return encode_invalid("R-SER-LIMIT", std::format("{} nested containers exceed 64", depth));
  }
  return value_access::make(value_kind::variant, case_no, std::move(payload), depth);
}

bool oset_less(const oset_entry &a, const oset_entry &b) noexcept {
  if (const auto c = std::lexicographical_compare_three_way(a.order_key.begin(), a.order_key.end(),
                                                            b.order_key.begin(), b.order_key.end());
      std::is_neq(c)) {
    return std::is_lt(c);
  }
  return a.element < b.element;
}

/// The position of `element` in an oset, or npos.
constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

std::size_t element_index(std::span<const oset_entry> entries, const id_bytes &element) noexcept {
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].element == element) {
      return i;
    }
  }
  return npos;
}

std::string hex(std::span<const std::uint8_t> b) {
  std::string out;
  out.reserve(b.size() * 2);
  for (const std::uint8_t x : b) {
    out += std::format("{:02x}", x);
  }
  return out;
}

std::string id_text(const id_bytes &raw) {
  const auto id = record_id::from_bytes(raw);
  return id ? id->to_string() : hex(raw);
}

std::string key_text(const value &k) {
  switch (k.kind()) {
  case value_kind::uint:
    return std::format("{}", k.as_uint());
  case value_kind::nint: {
    const std::uint64_t m = k.nint_magnitude();
    if (m == std::numeric_limits<std::uint64_t>::max()) {
      return "-18446744073709551616";
    }
    return std::format("-{}", m + 1);
  }
  case value_kind::string: {
    std::string out = "\"";
    for (const char ch : k.as_string()) {
      if (ch == '"' || ch == '\\') {
        out += '\\';
      }
      out += ch;
    }
    out += '"';
    return out;
  }
  case value_kind::id:
    return id_text(k.as_id_bytes());
  default:
    return std::string(to_string(k.kind()));
  }
}

std::string segment_text(const path_segment &s) {
  switch (s.kind()) {
  case path_segment::kind_type::field:
    return std::format("{}", s.field_no());
  case path_segment::kind_type::key:
    return std::format("[{}]", key_text(s.key_value()));
  case path_segment::kind_type::element:
    return std::format("<{}>", id_text(s.element_id()));
  }
  return "?";
}

std::unexpected<error> mismatch(const path_segment &s, const value &v) {
  return make_error(errc::type_mismatch, std::format("path segment {} does not apply to a {}",
                                                     segment_text(s), to_string(v.kind())));
}

std::unexpected<error> absent(const path_segment &s) {
  return make_error(errc::not_found, std::format("path segment {} is absent", segment_text(s)));
}

/// The child `s` names in `parent`.
result<value> child_of(const value &parent, const path_segment &s) {
  switch (s.kind()) {
  case path_segment::kind_type::field:
    if (parent.kind() == value_kind::struct_) {
      const value *v = parent.find_field(s.field_no());
      return v != nullptr ? result<value>(*v) : absent(s);
    }
    if (parent.kind() == value_kind::variant) {
      return parent.case_no() == s.field_no() ? result<value>(parent.payload()) : absent(s);
    }
    return mismatch(s, parent);
  case path_segment::kind_type::key:
    if (parent.kind() == value_kind::map) {
      const value *v = parent.find_key(s.key_value());
      return v != nullptr ? result<value>(*v) : absent(s);
    }
    return mismatch(s, parent);
  case path_segment::kind_type::element:
    if (parent.kind() == value_kind::oset) {
      const oset_entry *e = parent.find_element(s.element_id());
      return e != nullptr ? result<value>(e->v) : absent(s);
    }
    return mismatch(s, parent);
  }
  return mismatch(s, parent);
}

/// `parent` with the child `s` names set to `v` (inserted when a field or key is absent).
result<value> set_child(const value &parent, const path_segment &s, value v) {
  switch (s.kind()) {
  case path_segment::kind_type::field: {
    if (parent.kind() == value_kind::variant) {
      if (parent.case_no() != s.field_no()) {
        return absent(s);
      }
      return make_variant(s.field_no(), std::move(v));
    }
    if (parent.kind() != value_kind::struct_) {
      return mismatch(s, parent);
    }
    const auto old = parent.fields();
    std::vector<field> fields(old.begin(), old.end());
    const auto it = std::ranges::lower_bound(fields, s.field_no(), {}, &field::no);
    if (it != fields.end() && it->no == s.field_no()) {
      if (it->v.node_address() == v.node_address()) {
        return parent;
      }
      it->v = std::move(v);
    } else {
      fields.insert(it, field{.no = s.field_no(), .v = std::move(v)});
    }
    return make_container(value_kind::struct_, std::move(fields));
  }
  case path_segment::kind_type::key: {
    if (parent.kind() != value_kind::map) {
      return mismatch(s, parent);
    }
    const auto old = parent.entries();
    std::vector<map_entry> entries(old.begin(), old.end());
    const auto it = std::ranges::lower_bound(
        entries, s.key_value(),
        [](const value &a, const value &b) { return std::is_lt(compare_keys(a, b)); },
        &map_entry::key);
    if (it != entries.end() && std::is_eq(compare_keys(it->key, s.key_value()))) {
      if (it->v.node_address() == v.node_address()) {
        return parent;
      }
      it->v = std::move(v);
    } else {
      entries.insert(it, map_entry{.key = s.key_value(), .v = std::move(v)});
    }
    return make_container(value_kind::map, std::move(entries));
  }
  case path_segment::kind_type::element: {
    if (parent.kind() != value_kind::oset) {
      return mismatch(s, parent);
    }
    const auto old = parent.elements();
    const std::size_t i = element_index(old, s.element_id());
    if (i == npos) {
      return absent(s);
    }
    if (old[i].v.node_address() == v.node_address()) {
      return parent;
    }
    std::vector<oset_entry> entries(old.begin(), old.end());
    entries[i].v = std::move(v);
    return make_container(value_kind::oset, std::move(entries));
  }
  }
  return mismatch(s, parent);
}

/// `parent` without the child `s` names; `parent` itself when that child is absent.
result<value> remove_child(const value &parent, const path_segment &s) {
  switch (s.kind()) {
  case path_segment::kind_type::field: {
    if (parent.kind() != value_kind::struct_) {
      return mismatch(s, parent);
    }
    const auto old = parent.fields();
    const auto it = std::ranges::lower_bound(old, s.field_no(), {}, &field::no);
    if (it == old.end() || it->no != s.field_no()) {
      return parent;
    }
    std::vector<field> fields(old.begin(), it);
    fields.insert(fields.end(), std::next(it), old.end());
    return make_container(value_kind::struct_, std::move(fields));
  }
  case path_segment::kind_type::key: {
    if (parent.kind() != value_kind::map) {
      return mismatch(s, parent);
    }
    const auto old = parent.entries();
    std::vector<map_entry> entries;
    entries.reserve(old.size());
    bool found = false;
    for (const map_entry &e : old) {
      if (!found && std::is_eq(compare_keys(e.key, s.key_value()))) {
        found = true;
        continue;
      }
      entries.push_back(e);
    }
    if (!found) {
      return parent;
    }
    return make_container(value_kind::map, std::move(entries));
  }
  case path_segment::kind_type::element: {
    if (parent.kind() != value_kind::oset) {
      return mismatch(s, parent);
    }
    const auto old = parent.elements();
    const std::size_t i = element_index(old, s.element_id());
    if (i == npos) {
      return parent;
    }
    std::vector<oset_entry> entries;
    entries.reserve(old.size() - 1);
    for (std::size_t k = 0; k < old.size(); ++k) {
      if (k != i) {
        entries.push_back(old[k]);
      }
    }
    return make_container(value_kind::oset, std::move(entries));
  }
  }
  return mismatch(s, parent);
}

/// Rebuilds `node` along `segs` (non-empty): `at_last(parent, last)` makes the new parent of the
/// last segment, and every container above it is copied with one child replaced. Untouched
/// siblings are shared.
template <class F>
result<value> edit(const value &node, std::span<const path_segment> segs, const F &at_last) {
  if (segs.size() == 1) {
    return at_last(node, segs.front());
  }
  auto child = child_of(node, segs.front());
  if (!child) {
    return std::unexpected(std::move(child.error()));
  }
  auto updated = edit(*child, segs.subspan(1), at_last);
  if (!updated) {
    return updated;
  }
  if (updated->node_address() == child->node_address()) {
    return node;
  }
  return set_child(node, segs.front(), std::move(*updated));
}

/// Applies `op` to the oset at `p` (the value itself for the empty path).
template <class Op> result<value> edit_oset(const value &root, const path &p, const Op &op) {
  if (p.empty()) {
    return op(root);
  }
  return edit(root, p, [&op](const value &parent, const path_segment &last) -> result<value> {
    auto target = child_of(parent, last);
    if (!target) {
      return target;
    }
    auto changed = op(*target);
    if (!changed) {
      return changed;
    }
    return set_child(parent, last, std::move(*changed));
  });
}

std::unexpected<error> not_an_oset(const value &v) {
  return make_error(errc::type_mismatch,
                    std::format("element operation on a {}, not an oset", to_string(v.kind())));
}

/// `entries` with `e` inserted at its canonical position.
std::vector<oset_entry> inserted(std::span<const oset_entry> entries, oset_entry e) {
  std::vector<oset_entry> out(entries.begin(), entries.end());
  const auto it = std::ranges::lower_bound(out, e, oset_less);
  out.insert(it, std::move(e));
  return out;
}

/// The SLB/1 head of a map key (tag, then the LEB128 integer or length) and its body bytes.
struct key_bytes {
  std::array<std::uint8_t, 1 + max_uvarint_size> head{};
  std::size_t head_size = 0;
  std::string_view text; // a string key's bytes
  byte_span raw;         // an id key's bytes

  [[nodiscard]] std::size_t size() const noexcept { return head_size + text.size() + raw.size(); }
  [[nodiscard]] std::uint8_t operator[](std::size_t i) const noexcept {
    if (i < head_size) {
      return std::span(head)[i];
    }
    i -= head_size;
    return text.empty() ? raw[i] : static_cast<std::uint8_t>(text[i]);
  }
};

key_bytes encode_key(const value &k) noexcept {
  key_bytes out;
  out.head[0] = static_cast<std::uint8_t>(k.kind());
  std::span<std::uint8_t, max_uvarint_size> rest(out.head.data() + 1, max_uvarint_size);
  switch (k.kind()) {
  case value_kind::uint:
    out.head_size = 1 + encode_uvarint(k.as_uint(), rest);
    break;
  case value_kind::nint:
    out.head_size = 1 + encode_uvarint(k.nint_magnitude(), rest);
    break;
  case value_kind::string: {
    const std::string_view s = k.as_string();
    out.head_size = 1 + encode_uvarint(s.size(), rest);
    out.text = s;
    break;
  }
  case value_kind::id:
    out.head_size = 1;
    out.raw = k.as_id_bytes();
    break;
  default:
    out.head_size = 1;
    break;
  }
  return out;
}

} // namespace

std::string_view to_string(value_kind k) noexcept {
  switch (k) {
  case value_kind::bool_:
    return "bool";
  case value_kind::uint:
    return "uint";
  case value_kind::nint:
    return "nint";
  case value_kind::f32:
    return "f32";
  case value_kind::f64:
    return "f64";
  case value_kind::string:
    return "string";
  case value_kind::bytes:
    return "bytes";
  case value_kind::id:
    return "id";
  case value_kind::digest:
    return "digest";
  case value_kind::list:
    return "list";
  case value_kind::map:
    return "map";
  case value_kind::struct_:
    return "struct";
  case value_kind::variant:
    return "variant";
  case value_kind::oset:
    return "oset";
  }
  return "unknown";
}

std::strong_ordering compare_keys(const value &a, const value &b) noexcept {
  const key_bytes ka = encode_key(a);
  const key_bytes kb = encode_key(b);
  const std::size_t n = std::min(ka.size(), kb.size());
  for (std::size_t i = 0; i < n; ++i) {
    if (ka[i] != kb[i]) {
      return ka[i] <=> kb[i];
    }
  }
  return ka.size() <=> kb.size();
}

// Scalars.

value value::boolean(bool b) {
  return b ? value_access::make(value_kind::bool_, 1, std::monostate{}, 0) : value{};
}

value value::uint(std::uint64_t v) {
  return value_access::make(value_kind::uint, v, std::monostate{}, 0);
}

value value::integer(std::int64_t v) {
  if (v >= 0) {
    return uint(static_cast<std::uint64_t>(v));
  }
  // -(m + 1) = v  =>  m = -(v + 1), which fits for every negative int64.
  return nint(static_cast<std::uint64_t>(-(v + 1)));
}

value value::nint(std::uint64_t magnitude) {
  return value_access::make(value_kind::nint, magnitude, std::monostate{}, 0);
}

result<value> value::f32(float v) {
  if (!std::isfinite(v)) {
    return encode_invalid("R-SER-NAN", "f32 is NaN or infinite");
  }
  if (v == 0.0F) {
    v = 0.0F; // -0.0 is stored as +0.0 (ARCH1-R27)
  }
  return value_access::make(value_kind::f32, std::bit_cast<std::uint32_t>(v), std::monostate{}, 0);
}

result<value> value::f64(double v) {
  if (!std::isfinite(v)) {
    return encode_invalid("R-SER-NAN", "f64 is NaN or infinite");
  }
  if (v == 0.0) {
    v = 0.0;
  }
  return value_access::make(value_kind::f64, std::bit_cast<std::uint64_t>(v), std::monostate{}, 0);
}

result<value> value::string(std::string_view text) {
  if (text.size() > max_byte_length) {
    return limit_error("string length", text.size());
  }
  if (const auto bad = utf8_violation(text)) {
    return encode_invalid("R-SER-UTF8",
                          std::format("ill-formed UTF-8 or NUL at string byte {}", *bad));
  }
  return value_access::make(value_kind::string, 0, std::string(text), 0);
}

result<value> value::bytes(byte_span b) {
  if (b.size() > max_byte_length) {
    return limit_error("bytes length", b.size());
  }
  return value_access::make(value_kind::bytes, 0, std::vector<std::uint8_t>(b.begin(), b.end()), 0);
}

value value::id(const record_id &id) { return value::id(id.bytes()); }

value value::id(const id_bytes &raw) { return value_access::make(value_kind::id, 0, raw, 0); }

value value::digest(const fabric::digest &d) {
  return value_access::make(value_kind::digest, 0, d, 0);
}

// Containers.

result<value> value::list(std::vector<value> items) {
  return make_container(value_kind::list, std::move(items));
}

result<value> value::map(std::vector<map_entry> entries) {
  for (const map_entry &e : entries) {
    if (!is_key_kind(e.key.kind())) {
      return encode_invalid(
          "R-SER-TAG",
          std::format("a map key is a {}, not uint, nint, string or id", to_string(e.key.kind())));
    }
  }
  std::ranges::stable_sort(entries, [](const map_entry &a, const map_entry &b) {
    return std::is_lt(compare_keys(a.key, b.key));
  });
  const auto dup = std::ranges::adjacent_find(entries, [](const map_entry &a, const map_entry &b) {
    return std::is_eq(compare_keys(a.key, b.key));
  });
  if (dup != entries.end()) {
    return encode_invalid("R-SER-DUP", std::format("map key {} repeats", key_text(dup->key)));
  }
  return make_container(value_kind::map, std::move(entries));
}

result<value> value::struct_(std::vector<field> fields) {
  std::ranges::stable_sort(fields, {}, &field::no);
  const auto dup = std::ranges::adjacent_find(
      fields, [](const field &a, const field &b) { return a.no == b.no; });
  if (dup != fields.end()) {
    return encode_invalid("R-SER-DUP", std::format("struct field {} repeats", dup->no));
  }
  return make_container(value_kind::struct_, std::move(fields));
}

result<value> value::variant(std::uint32_t case_no, value payload) {
  return make_variant(case_no, std::move(payload));
}

result<value> value::oset(std::vector<oset_entry> entries) {
  for (const oset_entry &e : entries) {
    if (e.order_key.size() > max_byte_length) {
      return limit_error("order key length", e.order_key.size());
    }
  }
  std::vector<id_bytes> ids;
  ids.reserve(entries.size());
  for (const oset_entry &e : entries) {
    ids.push_back(e.element);
  }
  std::ranges::sort(ids);
  if (const auto dup = std::ranges::adjacent_find(ids); dup != ids.end()) {
    return encode_invalid("R-SER-DUP", std::format("oset element {} repeats", id_text(*dup)));
  }
  std::ranges::sort(entries, oset_less);
  return make_container(value_kind::oset, std::move(entries));
}

// Observers.

value_kind value::kind() const noexcept { return node_ ? node_->kind : value_kind::bool_; }

std::size_t value::depth() const noexcept { return node_ ? node_->depth : 0; }

bool value::as_bool() const noexcept {
  SL_ASSERT(kind() == value_kind::bool_, "value accessor used on the wrong kind");
  return node_ != nullptr && node_->word != 0;
}

std::uint64_t value::as_uint() const noexcept { return node_of(*this, value_kind::uint).word; }

std::uint64_t value::nint_magnitude() const noexcept {
  return node_of(*this, value_kind::nint).word;
}

float value::as_f32() const noexcept {
  return std::bit_cast<float>(static_cast<std::uint32_t>(node_of(*this, value_kind::f32).word));
}

double value::as_f64() const noexcept {
  return std::bit_cast<double>(node_of(*this, value_kind::f64).word);
}

std::string_view value::as_string() const noexcept {
  (void)node_of(*this, value_kind::string);
  return payload_of<std::string>(*this);
}

byte_span value::as_bytes() const noexcept {
  (void)node_of(*this, value_kind::bytes);
  return payload_of<std::vector<std::uint8_t>>(*this);
}

const id_bytes &value::as_id_bytes() const noexcept {
  (void)node_of(*this, value_kind::id);
  return payload_of<id_bytes>(*this);
}

fabric::digest value::as_digest() const noexcept {
  (void)node_of(*this, value_kind::digest);
  return payload_of<fabric::digest>(*this);
}

std::span<const value> value::items() const noexcept {
  (void)node_of(*this, value_kind::list);
  return payload_of<std::vector<value>>(*this);
}

std::span<const map_entry> value::entries() const noexcept {
  (void)node_of(*this, value_kind::map);
  return payload_of<std::vector<map_entry>>(*this);
}

std::span<const field> value::fields() const noexcept {
  (void)node_of(*this, value_kind::struct_);
  return payload_of<std::vector<field>>(*this);
}

std::uint32_t value::case_no() const noexcept {
  return static_cast<std::uint32_t>(node_of(*this, value_kind::variant).word);
}

const value &value::payload() const noexcept {
  (void)node_of(*this, value_kind::variant);
  return payload_of<value>(*this);
}

std::span<const oset_entry> value::elements() const noexcept {
  (void)node_of(*this, value_kind::oset);
  return payload_of<std::vector<oset_entry>>(*this);
}

std::optional<std::int64_t> value::as_i64() const noexcept {
  constexpr auto max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (kind() == value_kind::uint && as_uint() <= max) {
    return static_cast<std::int64_t>(as_uint());
  }
  if (kind() == value_kind::nint && nint_magnitude() <= max) {
    return -static_cast<std::int64_t>(nint_magnitude()) - 1;
  }
  return std::nullopt;
}

result<record_id> value::as_record_id() const {
  if (kind() != value_kind::id) {
    return make_error(errc::type_mismatch,
                      std::format("a {} is not an id", fabric::to_string(kind())));
  }
  return record_id::from_bytes(as_id_bytes());
}

const value *value::find_field(std::uint32_t no) const noexcept {
  if (kind() != value_kind::struct_) {
    return nullptr;
  }
  const auto fs = fields();
  const auto it = std::ranges::lower_bound(fs, no, {}, &field::no);
  return it != fs.end() && it->no == no ? &it->v : nullptr;
}

const value *value::find_key(const value &key) const noexcept {
  if (kind() != value_kind::map || !is_key_kind(key.kind())) {
    return nullptr;
  }
  const auto es = entries();
  const auto it = std::ranges::lower_bound(
      es, key, [](const value &a, const value &b) { return std::is_lt(compare_keys(a, b)); },
      &map_entry::key);
  return it != es.end() && std::is_eq(compare_keys(it->key, key)) ? &it->v : nullptr;
}

const oset_entry *value::find_element(const id_bytes &element) const noexcept {
  if (kind() != value_kind::oset) {
    return nullptr;
  }
  const auto es = elements();
  const std::size_t i = element_index(es, element);
  return i == npos ? nullptr : &es[i];
}

// Paths.

result<value> value::get(const path &p) const {
  value cur = *this;
  for (const path_segment &s : p) {
    auto next = child_of(cur, s);
    if (!next) {
      return next;
    }
    cur = std::move(*next);
  }
  return cur;
}

result<value> value::with(const path &p, value v) const {
  if (p.empty()) {
    return v;
  }
  return edit(*this, p, [&v](const value &parent, const path_segment &last) {
    return set_child(parent, last, v);
  });
}

result<value> value::without(const path &p) const {
  if (p.empty()) {
    return make_error(errc::type_mismatch, "without() needs a non-empty path");
  }
  return edit(*this, p, [](const value &parent, const path_segment &last) -> result<value> {
    if (parent.kind() == value_kind::variant) {
      return make_error(errc::type_mismatch, "a variant payload cannot be removed");
    }
    return remove_child(parent, last);
  });
}

result<value> value::insert_element(const path &oset_path, const id_bytes &element,
                                    const std::vector<std::uint8_t> &order_key, value v) const {
  if (order_key.size() > max_byte_length) {
    return limit_error("order key length", order_key.size());
  }
  return edit_oset(*this, oset_path, [&](const value &oset) -> result<value> {
    if (oset.kind() != value_kind::oset) {
      return not_an_oset(oset);
    }
    if (oset.find_element(element) != nullptr) {
      return make_error(errc::already_exists,
                        std::format("oset element {} already exists", id_text(element)));
    }
    return make_container(
        value_kind::oset,
        inserted(oset.elements(), oset_entry{.element = element, .order_key = order_key, .v = v}));
  });
}

result<value> value::move_element(const path &oset_path, const id_bytes &element,
                                  const std::vector<std::uint8_t> &order_key) const {
  if (order_key.size() > max_byte_length) {
    return limit_error("order key length", order_key.size());
  }
  return edit_oset(*this, oset_path, [&](const value &oset) -> result<value> {
    if (oset.kind() != value_kind::oset) {
      return not_an_oset(oset);
    }
    const auto old = oset.elements();
    const std::size_t i = element_index(old, element);
    if (i == npos) {
      return make_error(errc::not_found,
                        std::format("oset element {} is absent", id_text(element)));
    }
    std::vector<oset_entry> rest;
    rest.reserve(old.size());
    for (std::size_t k = 0; k < old.size(); ++k) {
      if (k != i) {
        rest.push_back(old[k]);
      }
    }
    return make_container(
        value_kind::oset,
        inserted(rest, oset_entry{.element = element, .order_key = order_key, .v = old[i].v}));
  });
}

bool operator==(const value &a, const value &b) noexcept {
  if (a.node_ == b.node_) {
    return true;
  }
  if (a.kind() != b.kind()) {
    return false;
  }
  switch (a.kind()) {
  case value_kind::bool_:
    return a.as_bool() == b.as_bool();
  case value_kind::uint:
  case value_kind::nint:
  case value_kind::f32:
  case value_kind::f64:
    return a.node_->word == b.node_->word;
  case value_kind::string:
    return a.as_string() == b.as_string();
  case value_kind::bytes:
    return std::ranges::equal(a.as_bytes(), b.as_bytes());
  case value_kind::id:
    return a.as_id_bytes() == b.as_id_bytes();
  case value_kind::digest:
    return a.as_digest() == b.as_digest();
  case value_kind::list:
    return std::ranges::equal(a.items(), b.items());
  case value_kind::map:
    return std::ranges::equal(a.entries(), b.entries());
  case value_kind::struct_:
    return std::ranges::equal(a.fields(), b.fields());
  case value_kind::variant:
    return a.case_no() == b.case_no() && a.payload() == b.payload();
  case value_kind::oset:
    return std::ranges::equal(a.elements(), b.elements());
  }
  return false;
}

// Path segments.

path_segment path_segment::field(std::uint32_t no) noexcept {
  path_segment s;
  s.kind_ = kind_type::field;
  s.no_ = no;
  return s;
}

result<path_segment> path_segment::key(value k) {
  if (!is_key_kind(k.kind())) {
    return make_error(errc::type_mismatch,
                      std::format("a {} cannot be a map key", fabric::to_string(k.kind())));
  }
  path_segment s;
  s.kind_ = kind_type::key;
  s.key_ = std::move(k);
  return s;
}

path_segment path_segment::element(const id_bytes &element) noexcept {
  path_segment s;
  s.kind_ = kind_type::element;
  s.element_ = element;
  return s;
}

path_segment path_segment::element(const record_id &element) noexcept {
  return path_segment::element(element.bytes());
}

bool operator==(const path_segment &a, const path_segment &b) noexcept {
  return std::is_eq(a <=> b);
}

std::strong_ordering operator<=>(const path_segment &a, const path_segment &b) noexcept {
  if (a.kind_ != b.kind_) {
    return a.kind_ <=> b.kind_;
  }
  switch (a.kind_) {
  case path_segment::kind_type::field:
    return a.no_ <=> b.no_;
  case path_segment::kind_type::key:
    return compare_keys(a.key_, b.key_);
  case path_segment::kind_type::element:
    return a.element_ <=> b.element_;
  }
  return std::strong_ordering::equal;
}

std::string to_string(const path &p) {
  if (p.empty()) {
    return "/";
  }
  std::string out;
  for (const path_segment &s : p) {
    out += '/';
    out += segment_text(s);
  }
  return out;
}

} // namespace sl::fabric
