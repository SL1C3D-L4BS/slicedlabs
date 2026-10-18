// SLB/1 encoder and decoder (WO-041, SL-ARCH-001 ARCH1-R26 to ARCH1-R30, ADR-0012). See codec.hpp;
// the byte layout, the check order and the offsets of every rejection follow
// tools/fabric/slb_ref.py.

#include "sl/fabric/codec.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

namespace {

constexpr std::uint8_t tag_false = 0x01;
constexpr std::uint8_t tag_true = 0x02;
constexpr std::uint8_t tag_uint = 0x03;
constexpr std::uint8_t tag_nint = 0x04;
constexpr std::uint8_t tag_f32 = 0x05;
constexpr std::uint8_t tag_f64 = 0x06;
constexpr std::uint8_t tag_string = 0x07;
constexpr std::uint8_t tag_bytes = 0x08;
constexpr std::uint8_t tag_id = 0x09;
constexpr std::uint8_t tag_digest = 0x0A;
constexpr std::uint8_t tag_list = 0x0B;
constexpr std::uint8_t tag_map = 0x0C;
constexpr std::uint8_t tag_struct = 0x0D;
constexpr std::uint8_t tag_variant = 0x0E;
constexpr std::uint8_t tag_oset = 0x0F;

constexpr std::uint64_t max_u32 = std::numeric_limits<std::uint32_t>::max();
constexpr std::size_t id_size = 16;
constexpr std::size_t digest_size = sha256::digest_size;

// ---- encoder ------------------------------------------------------------------------------------

/// Sums encoded sizes and stops counting once the total passes the record body limit, so a value
/// with heavily shared subtrees is refused after at most ~16 Mi visited bytes.
class size_counter {
public:
  void add(std::size_t n) noexcept {
    if (over_) {
      return;
    }
    if (n > max_record_body - total_) {
      over_ = true;
      return;
    }
    total_ += n;
  }
  [[nodiscard]] bool over() const noexcept { return over_; }
  [[nodiscard]] std::size_t total() const noexcept { return total_; }

private:
  std::size_t total_ = 0;
  bool over_ = false;
};

void add_blob(size_counter &s, std::size_t n) noexcept {
  s.add(1);
  s.add(uvarint_size(n));
  s.add(n);
}

void size_of(const value &v, size_counter &s) noexcept {
  if (s.over()) {
    return;
  }
  switch (v.kind()) {
  case value_kind::bool_:
    s.add(1);
    return;
  case value_kind::uint:
    s.add(1 + uvarint_size(v.as_uint()));
    return;
  case value_kind::nint:
    s.add(1 + uvarint_size(v.nint_magnitude()));
    return;
  case value_kind::f32:
    s.add(1 + sizeof(std::uint32_t));
    return;
  case value_kind::f64:
    s.add(1 + sizeof(std::uint64_t));
    return;
  case value_kind::string:
    add_blob(s, v.as_string().size());
    return;
  case value_kind::bytes:
    add_blob(s, v.as_bytes().size());
    return;
  case value_kind::id:
    s.add(1 + id_size);
    return;
  case value_kind::digest:
    s.add(2 + digest_size);
    return;
  case value_kind::list:
    s.add(1 + uvarint_size(v.items().size()));
    for (const value &x : v.items()) {
      size_of(x, s);
    }
    return;
  case value_kind::map:
    s.add(1 + uvarint_size(v.entries().size()));
    for (const map_entry &e : v.entries()) {
      size_of(e.key, s);
      size_of(e.v, s);
    }
    return;
  case value_kind::struct_:
    s.add(1 + uvarint_size(v.fields().size()));
    for (const field &f : v.fields()) {
      s.add(uvarint_size(f.no));
      size_of(f.v, s);
    }
    return;
  case value_kind::variant:
    s.add(1 + uvarint_size(v.case_no()));
    size_of(v.payload(), s);
    return;
  case value_kind::oset:
    s.add(1 + uvarint_size(v.elements().size()));
    for (const oset_entry &e : v.elements()) {
      s.add(id_size);
      s.add(uvarint_size(e.order_key.size()));
      s.add(e.order_key.size());
      size_of(e.v, s);
    }
    return;
  }
}

class slb_writer {
public:
  explicit slb_writer(std::size_t size) { out_.reserve(size); }

  void u8(std::uint8_t b) { out_.push_back(b); }
  void uvarint(std::uint64_t n) {
    std::array<std::uint8_t, max_uvarint_size> tmp{};
    const std::size_t len = encode_uvarint(n, tmp);
    raw(byte_span{tmp}.first(len));
  }
  void raw(byte_span b) { out_.insert(out_.end(), b.begin(), b.end()); }
  template <class T> void little_endian(T bits) {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
      out_.push_back(static_cast<std::uint8_t>(bits >> (8U * i)));
    }
  }

  void write(const value &v) {
    switch (v.kind()) {
    case value_kind::bool_:
      u8(v.as_bool() ? tag_true : tag_false);
      return;
    case value_kind::uint:
      u8(tag_uint);
      uvarint(v.as_uint());
      return;
    case value_kind::nint:
      u8(tag_nint);
      uvarint(v.nint_magnitude());
      return;
    case value_kind::f32:
      u8(tag_f32);
      little_endian(std::bit_cast<std::uint32_t>(v.as_f32()));
      return;
    case value_kind::f64:
      u8(tag_f64);
      little_endian(std::bit_cast<std::uint64_t>(v.as_f64()));
      return;
    case value_kind::string: {
      const std::string_view s = v.as_string();
      u8(tag_string);
      uvarint(s.size());
      out_.insert(out_.end(), s.begin(), s.end());
      return;
    }
    case value_kind::bytes:
      u8(tag_bytes);
      uvarint(v.as_bytes().size());
      raw(v.as_bytes());
      return;
    case value_kind::id:
      u8(tag_id);
      raw(v.as_id_bytes());
      return;
    case value_kind::digest: {
      const digest d = v.as_digest();
      u8(tag_digest);
      u8(static_cast<std::uint8_t>(d.algo));
      raw(d.bytes);
      return;
    }
    default:
      write_container(v);
      return;
    }
  }

  [[nodiscard]] std::vector<std::uint8_t> take() noexcept { return std::move(out_); }

private:
  void write_container(const value &v) {
    switch (v.kind()) {
    case value_kind::list:
      u8(tag_list);
      uvarint(v.items().size());
      for (const value &x : v.items()) {
        write(x);
      }
      return;
    case value_kind::map:
      u8(tag_map);
      uvarint(v.entries().size());
      for (const map_entry &e : v.entries()) {
        write(e.key);
        write(e.v);
      }
      return;
    case value_kind::struct_:
      u8(tag_struct);
      uvarint(v.fields().size());
      for (const field &f : v.fields()) {
        uvarint(f.no);
        write(f.v);
      }
      return;
    case value_kind::variant:
      u8(tag_variant);
      uvarint(v.case_no());
      write(v.payload());
      return;
    case value_kind::oset:
      u8(tag_oset);
      uvarint(v.elements().size());
      for (const oset_entry &e : v.elements()) {
        raw(e.element);
        uvarint(e.order_key.size());
        raw(e.order_key);
        write(e.v);
      }
      return;
    default:
      return;
    }
  }

  std::vector<std::uint8_t> out_;
};

// ---- decoder ------------------------------------------------------------------------------------

errc code_of(std::string_view rule) noexcept {
  if (rule == "R-SER-TRUNC") {
    return errc::decode_truncated;
  }
  if (rule == "R-SER-LIMIT") {
    return errc::decode_limit;
  }
  if (rule == "R-SER-UTF8") {
    return errc::decode_bad_utf8;
  }
  if (rule == "R-SER-NAN") {
    return errc::decode_bad_float;
  }
  if (rule == "R-SER-TAG") {
    return errc::decode_unknown_tag;
  }
  if (rule == "R-SER-UNKNOWN") {
    return errc::decode_unknown_field;
  }
  return errc::decode_noncanonical; // VARINT, ORDER, DUP, NEGZERO, TRAILING
}

std::unexpected<error> reject(std::string_view rule, std::size_t offset, std::string_view what) {
  return make_error(code_of(rule), std::format("{} at offset {}: {}", rule, offset, what));
}

bool is_key_tag(std::uint8_t tag) noexcept {
  return tag == tag_uint || tag == tag_nint || tag == tag_string || tag == tag_id;
}

/// The schema type a decoded container is judged against (strict policy only).
class schema_view {
public:
  explicit schema_view(const schema_registry *schemas) noexcept : schemas_(schemas) {}

  /// `t` with tagged types replaced by their base; null when unresolvable.
  [[nodiscard]] const type_desc *resolve(const type_desc *t) const noexcept {
    for (std::size_t hops = 0; t != nullptr && t->kind == type_kind::tagged; ++hops) {
      if (hops > max_value_depth || !t->element) {
        return nullptr;
      }
      t = schemas_->find(*t->element);
    }
    return t;
  }

  [[nodiscard]] const type_desc *find(const type_id &id) const noexcept {
    return schemas_ == nullptr ? nullptr : resolve(schemas_->find(id));
  }

  /// `t` when its kind matches the container tag, else null (slb_ref.py _kind_matches).
  [[nodiscard]] static const type_desc *matching(const type_desc *t, std::uint8_t tag) noexcept {
    if (t == nullptr) {
      return nullptr;
    }
    switch (tag) {
    case tag_list:
      return t->kind == type_kind::list ? t : nullptr;
    case tag_map:
      return t->kind == type_kind::map ? t : nullptr;
    case tag_struct:
      return t->kind == type_kind::struct_ || t->kind == type_kind::external_ref ? t : nullptr;
    case tag_variant:
      return t->kind == type_kind::variant ? t : nullptr;
    case tag_oset:
      return t->kind == type_kind::oset ? t : nullptr;
    default:
      return nullptr;
    }
  }

  [[nodiscard]] const type_desc *element(const type_desc *t) const noexcept {
    return t != nullptr && t->element ? find(*t->element) : nullptr;
  }
  [[nodiscard]] const type_desc *field_type(const type_desc *t, std::uint32_t no) const noexcept {
    const field_desc *f = t != nullptr ? t->find_field(no) : nullptr;
    return f != nullptr ? find(f->type) : nullptr;
  }
  [[nodiscard]] const type_desc *case_type(const type_desc *t, std::uint32_t no) const noexcept {
    const case_desc *c = t != nullptr ? t->find_case(no) : nullptr;
    return c != nullptr ? find(c->type) : nullptr;
  }

private:
  const schema_registry *schemas_;
};

std::strong_ordering compare_bytes(byte_span a, byte_span b) noexcept {
  return std::lexicographical_compare_three_way(a.begin(), a.end(), b.begin(), b.end());
}

class slb_reader {
public:
  slb_reader(byte_span data, unknown_policy policy, const schema_registry *schemas) noexcept
      : in_(data), policy_(policy), schema_(schemas) {}

  [[nodiscard]] result<value> top(const type_desc *td) {
    auto v = read(0, td);
    if (!v) {
      return v;
    }
    if (pos_ < in_.size()) {
      return reject("R-SER-TRAILING", pos_, std::format("{} trailing byte(s)", in_.size() - pos_));
    }
    return v;
  }

  [[nodiscard]] const schema_view &schema() const noexcept { return schema_; }

private:
  [[nodiscard]] std::unexpected<error> truncated() const {
    return reject("R-SER-TRUNC", in_.size(), "input ends early");
  }

  [[nodiscard]] result<std::uint8_t> byte() {
    if (pos_ >= in_.size()) {
      return truncated();
    }
    return in_[pos_++];
  }

  [[nodiscard]] result<byte_span> take(std::size_t n) {
    if (n > in_.size() - pos_) {
      return truncated();
    }
    const byte_span out = in_.subspan(pos_, n);
    pos_ += n;
    return out;
  }

  [[nodiscard]] result<std::uint64_t> varint() {
    const std::size_t start = pos_;
    std::uint64_t v = 0;
    for (unsigned i = 0; i < max_uvarint_size; ++i) {
      const auto b = byte();
      if (!b) {
        return std::unexpected(b.error());
      }
      if (i == max_uvarint_size - 1 && *b > 1) {
        return reject("R-SER-VARINT", start, "more than 64 bits");
      }
      v |= static_cast<std::uint64_t>(*b & 0x7FU) << (7U * i);
      if ((*b & 0x80U) == 0) {
        if (*b == 0 && i > 0) {
          return reject("R-SER-VARINT", start, "overlong");
        }
        return v;
      }
    }
    return reject("R-SER-VARINT", start, "more than 64 bits");
  }

  /// A count of items that each take at least one byte: reserving more than the bytes left would
  /// allocate for input that is not there.
  [[nodiscard]] std::size_t reservable(std::uint64_t n) const noexcept {
    return static_cast<std::size_t>(std::min<std::uint64_t>(n, in_.size() - pos_));
  }

  [[nodiscard]] result<value> read(std::size_t depth, const type_desc *td) {
    const std::size_t off = pos_;
    const auto tag = byte();
    if (!tag) {
      return std::unexpected(tag.error());
    }
    switch (*tag) {
    case tag_false:
      return value::boolean(false);
    case tag_true:
      return value::boolean(true);
    case tag_uint:
      return varint().transform([](std::uint64_t n) { return value::uint(n); });
    case tag_nint:
      return varint().transform([](std::uint64_t n) { return value::nint(n); });
    case tag_f32:
    case tag_f64:
      return read_float(*tag, off);
    case tag_string:
    case tag_bytes:
      return read_blob(*tag);
    case tag_id: {
      const auto raw = take(id_size);
      if (!raw) {
        return std::unexpected(raw.error());
      }
      id_bytes b{};
      std::ranges::copy(*raw, b.begin());
      return value::id(b);
    }
    case tag_digest: {
      const auto algo = byte();
      if (!algo) {
        return std::unexpected(algo.error());
      }
      const auto raw = take(digest_size);
      if (!raw) {
        return std::unexpected(raw.error());
      }
      digest d;
      d.algo = static_cast<hash_algo>(*algo);
      std::ranges::copy(*raw, d.bytes.begin());
      return value::digest(d);
    }
    case tag_list:
    case tag_map:
    case tag_struct:
    case tag_variant:
    case tag_oset:
      if (depth + 1 > max_value_depth) {
        return reject("R-SER-LIMIT", off, "depth over 64");
      }
      return read_container(*tag, depth + 1, schema_view::matching(td, *tag));
    default:
      return reject("R-SER-TAG", off, std::format("unknown tag {:#04x}", *tag));
    }
  }

  [[nodiscard]] result<value> read_float(std::uint8_t tag, std::size_t off) {
    if (tag == tag_f32) {
      const auto raw = take(sizeof(std::uint32_t));
      if (!raw) {
        return std::unexpected(raw.error());
      }
      std::uint32_t bits = 0;
      for (std::size_t i = 0; i < raw->size(); ++i) {
        bits |= static_cast<std::uint32_t>((*raw)[i]) << (8U * i);
      }
      if (((bits >> 23U) & 0xFFU) == 0xFFU) {
        return reject("R-SER-NAN", off, "NaN or infinity");
      }
      if (bits == 0x80000000U) {
        return reject("R-SER-NEGZERO", off, "-0.0");
      }
      return value::f32(std::bit_cast<float>(bits));
    }
    const auto raw = take(sizeof(std::uint64_t));
    if (!raw) {
      return std::unexpected(raw.error());
    }
    std::uint64_t bits = 0;
    for (std::size_t i = 0; i < raw->size(); ++i) {
      bits |= static_cast<std::uint64_t>((*raw)[i]) << (8U * i);
    }
    if (((bits >> 52U) & 0x7FFU) == 0x7FFU) {
      return reject("R-SER-NAN", off, "NaN or infinity");
    }
    if (bits == 0x8000000000000000ULL) {
      return reject("R-SER-NEGZERO", off, "-0.0");
    }
    return value::f64(std::bit_cast<double>(bits));
  }

  [[nodiscard]] result<value> read_blob(std::uint8_t tag) {
    const std::size_t len_off = pos_;
    const auto n = varint();
    if (!n) {
      return std::unexpected(n.error());
    }
    if (*n > max_byte_length) {
      return reject("R-SER-LIMIT", len_off, "length over 2^24");
    }
    const auto raw = take(static_cast<std::size_t>(*n));
    if (!raw) {
      return std::unexpected(raw.error());
    }
    if (tag == tag_bytes) {
      return value::bytes(*raw);
    }
    const std::size_t content = pos_ - raw->size();
    if (!detail::utf8_valid(*raw)) {
      return reject("R-SER-UTF8", content, "ill-formed UTF-8");
    }
    if (std::ranges::find(*raw, std::uint8_t{0}) != raw->end()) {
      return reject("R-SER-UTF8", content, "NUL in string");
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): UTF-8 bytes viewed as chars
    const std::string_view text(reinterpret_cast<const char *>(raw->data()), raw->size());
    return value::string(text);
  }

  [[nodiscard]] result<value> read_container(std::uint8_t tag, std::size_t depth,
                                             const type_desc *td) {
    if (tag == tag_variant) {
      const std::size_t case_off = pos_;
      const auto c = varint();
      if (!c) {
        return std::unexpected(c.error());
      }
      if (*c > max_u32) {
        return reject("R-SER-LIMIT", case_off, "variant case outside u32");
      }
      const auto case_no = static_cast<std::uint32_t>(*c);
      auto payload = read(depth, schema_.case_type(td, case_no));
      if (!payload) {
        return payload;
      }
      return value::variant(case_no, std::move(*payload));
    }
    const std::size_t count_off = pos_;
    const auto n = varint();
    if (!n) {
      return std::unexpected(n.error());
    }
    if (*n > max_container_items) {
      return reject("R-SER-LIMIT", count_off, "more than 2^24 items");
    }
    switch (tag) {
    case tag_list:
      return read_list(*n, depth, td);
    case tag_map:
      return read_map(*n, depth, td);
    case tag_struct:
      return read_struct(*n, depth, td);
    default:
      return read_oset(*n, depth, td);
    }
  }

  [[nodiscard]] result<value> read_list(std::uint64_t n, std::size_t depth, const type_desc *td) {
    std::vector<value> items;
    items.reserve(reservable(n));
    const type_desc *element = schema_.element(td);
    for (std::uint64_t i = 0; i < n; ++i) {
      auto x = read(depth, element);
      if (!x) {
        return x;
      }
      items.push_back(std::move(*x));
    }
    return value::list(std::move(items));
  }

  [[nodiscard]] result<value> read_map(std::uint64_t n, std::size_t depth, const type_desc *td) {
    std::vector<map_entry> entries;
    entries.reserve(reservable(n));
    const type_desc *element = schema_.element(td);
    byte_span prev;
    for (std::uint64_t i = 0; i < n; ++i) {
      const std::size_t start = pos_;
      if (start < in_.size() && !is_key_tag(in_[start])) {
        return reject("R-SER-TAG", start, std::format("map key tag {:#04x}", in_[start]));
      }
      auto key = read(depth, nullptr);
      if (!key) {
        return key;
      }
      const byte_span this_key = in_.subspan(start, pos_ - start);
      if (i > 0) {
        const auto order = compare_bytes(this_key, prev);
        if (std::is_eq(order)) {
          return reject("R-SER-DUP", start, "duplicate key");
        }
        if (std::is_lt(order)) {
          return reject("R-SER-ORDER", start, "key out of order");
        }
      }
      prev = this_key;
      auto v = read(depth, element);
      if (!v) {
        return v;
      }
      entries.push_back(map_entry{.key = std::move(*key), .v = std::move(*v)});
    }
    return value::map(std::move(entries));
  }

  [[nodiscard]] result<value> read_struct(std::uint64_t n, std::size_t depth, const type_desc *td) {
    std::vector<field> fields;
    fields.reserve(reservable(n));
    for (std::uint64_t i = 0; i < n; ++i) {
      const std::size_t start = pos_;
      const auto no = varint();
      if (!no) {
        return std::unexpected(no.error());
      }
      if (*no > max_u32) {
        return reject("R-SER-LIMIT", start, "field number outside u32");
      }
      const auto field_no = static_cast<std::uint32_t>(*no);
      if (i > 0 && field_no == fields.back().no) {
        return reject("R-SER-DUP", start, "duplicate key");
      }
      if (i > 0 && field_no < fields.back().no) {
        return reject("R-SER-ORDER", start, "key out of order");
      }
      if (policy_ == unknown_policy::strict && td != nullptr &&
          td->find_field(field_no) == nullptr) {
        return reject("R-SER-UNKNOWN", start,
                      std::format("field {} is not in the schema", field_no));
      }
      auto v = read(depth, schema_.field_type(td, field_no));
      if (!v) {
        return v;
      }
      fields.push_back(field{.no = field_no, .v = std::move(*v)});
    }
    return value::struct_(std::move(fields));
  }

  [[nodiscard]] result<value> read_oset(std::uint64_t n, std::size_t depth, const type_desc *td) {
    std::vector<oset_entry> entries;
    entries.reserve(reservable(n));
    const type_desc *element = schema_.element(td);
    std::set<id_bytes> seen;
    for (std::uint64_t i = 0; i < n; ++i) {
      const std::size_t start = pos_;
      const auto eid = take(id_size);
      if (!eid) {
        return std::unexpected(eid.error());
      }
      const std::size_t klen_off = pos_;
      const auto klen = varint();
      if (!klen) {
        return std::unexpected(klen.error());
      }
      if (*klen > max_byte_length) {
        return reject("R-SER-LIMIT", klen_off, "order key over 2^24 bytes");
      }
      const auto okey = take(static_cast<std::size_t>(*klen));
      if (!okey) {
        return std::unexpected(okey.error());
      }
      oset_entry e;
      std::ranges::copy(*eid, e.element.begin());
      if (i > 0) {
        const oset_entry &last = entries.back();
        auto order = compare_bytes(*okey, last.order_key);
        if (std::is_eq(order)) {
          order = compare_bytes(*eid, last.element);
        }
        if (std::is_eq(order)) {
          return reject("R-SER-DUP", start, "duplicate oset entry");
        }
        if (std::is_lt(order)) {
          return reject("R-SER-ORDER", start, "oset entry out of order");
        }
      }
      if (!seen.insert(e.element).second) {
        return reject("R-SER-DUP", start, "duplicate oset element id");
      }
      e.order_key.assign(okey->begin(), okey->end());
      auto v = read(depth, element);
      if (!v) {
        return v;
      }
      e.v = std::move(*v);
      entries.push_back(std::move(e));
    }
    return value::oset(std::move(entries));
  }

  byte_span in_;
  std::size_t pos_ = 0;
  unknown_policy policy_;
  schema_view schema_;
};

result<value> decode_with(byte_span data, unknown_policy policy, const schema_registry *schemas,
                          const type_desc *td) {
  if (data.size() > max_record_body) {
    return reject("R-SER-LIMIT", 0, "record body over 16 MiB");
  }
  slb_reader r(data, policy, schemas);
  return r.top(td);
}

constexpr std::string_view hex_digits = "0123456789abcdef";

int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

} // namespace

namespace detail {

bool utf8_valid(byte_span b) noexcept {
  std::size_t i = 0;
  const auto cont = [&](std::size_t at, std::uint8_t lo, std::uint8_t hi) noexcept {
    return at < b.size() && b[at] >= lo && b[at] <= hi;
  };
  while (i < b.size()) {
    const std::uint8_t c = b[i];
    if (c < 0x80U) {
      i += 1;
    } else if (c >= 0xC2U && c <= 0xDFU) {
      if (!cont(i + 1, 0x80, 0xBF)) {
        return false;
      }
      i += 2;
    } else if (c >= 0xE0U && c <= 0xEFU) {
      const std::uint8_t lo = c == 0xE0U ? 0xA0 : 0x80;
      const std::uint8_t hi = c == 0xEDU ? 0x9F : 0xBF;
      if (!cont(i + 1, lo, hi) || !cont(i + 2, 0x80, 0xBF)) {
        return false;
      }
      i += 3;
    } else if (c >= 0xF0U && c <= 0xF4U) {
      const std::uint8_t lo = c == 0xF0U ? 0x90 : 0x80;
      const std::uint8_t hi = c == 0xF4U ? 0x8F : 0xBF;
      if (!cont(i + 1, lo, hi) || !cont(i + 2, 0x80, 0xBF) || !cont(i + 3, 0x80, 0xBF)) {
        return false;
      }
      i += 4;
    } else {
      return false;
    }
  }
  return true;
}

} // namespace detail

result<std::vector<std::uint8_t>> encode(const value &v) {
  size_counter size;
  size_of(v, size);
  if (size.over()) {
    return make_error(errc::encode_invalid, "R-SER-LIMIT: record body over 16 MiB");
  }
  slb_writer w(size.total());
  w.write(v);
  return w.take();
}

result<value> decode(byte_span data, unknown_policy policy) {
  return decode_with(data, policy, nullptr, nullptr);
}

result<value> decode(byte_span data, unknown_policy policy, type_id type,
                     const schema_registry &schemas) {
  const schema_view view(&schemas);
  if (schemas.find(type) == nullptr) {
    return make_error(errc::schema_unknown,
                      std::format("type {} is not registered", type.to_string()));
  }
  return decode_with(data, policy, &schemas, view.find(type));
}

digest record_object_id(byte_span payload) noexcept {
  return hash_object(frame_header{.kind = object_kind::record}, payload);
}

result<std::string> digest_text(const digest &d) {
  std::string_view name;
  switch (d.algo) {
  case hash_algo::sha256:
    name = "sha256";
    break;
  case hash_algo::blake3:
    name = "blake3";
    break;
  default:
    return make_error(
        errc::type_mismatch,
        std::format("R-TXT-TYPE: digest algo {} has no text form", static_cast<unsigned>(d.algo)));
  }
  std::string out(name);
  out += ':';
  out += to_hex(d.bytes);
  return out;
}

std::string to_hex(byte_span b) {
  std::string out;
  out.reserve(b.size() * 2);
  for (const std::uint8_t x : b) {
    out += hex_digits[x >> 4U];
    out += hex_digits[x & 0x0FU];
  }
  return out;
}

result<std::vector<std::uint8_t>> from_hex(std::string_view text) {
  if (text.size() % 2 != 0) {
    return make_error(errc::invalid_argument, "hex text of odd length");
  }
  std::vector<std::uint8_t> out;
  out.reserve(text.size() / 2);
  for (std::size_t i = 0; i < text.size(); i += 2) {
    const int hi = hex_value(text[i]);
    const int lo = hex_value(text[i + 1]);
    if (hi < 0 || lo < 0) {
      return make_error(errc::invalid_argument, std::format("not hex at offset {}", i));
    }
    out.push_back(
        static_cast<std::uint8_t>((static_cast<unsigned>(hi) << 4U) | static_cast<unsigned>(lo)));
  }
  return out;
}

} // namespace sl::fabric
