// Patch to value and back (WO-043, SL-ARCH-002 ARCH2-R03, ARCH2-R49; ADR-0019).

#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

namespace {

// Field numbers of the value form (patch.hpp to_value).
namespace pf {
constexpr std::uint32_t major = 1;
constexpr std::uint32_t minor = 2;
constexpr std::uint32_t base = 3;
constexpr std::uint32_t ops = 4;
constexpr std::uint32_t meta = 5;
} // namespace pf

namespace of {
constexpr std::uint32_t kind = 1;
constexpr std::uint32_t target = 2;
constexpr std::uint32_t path = 3;
constexpr std::uint32_t new_value = 4;
constexpr std::uint32_t expect = 5;
constexpr std::uint32_t captured = 6;
constexpr std::uint32_t type = 7;
constexpr std::uint32_t elem = 8;
constexpr std::uint32_t order_key = 9;
constexpr std::uint32_t expect_order_key = 10;
constexpr std::uint32_t rev = 11;
constexpr std::uint32_t expect_type = 12;
constexpr std::uint32_t last = expect_type;
} // namespace of

namespace mf {
constexpr std::uint32_t label = 1;
constexpr std::uint32_t undo_group = 2;
constexpr std::uint32_t coalesce_key = 3;
constexpr std::uint32_t idempotency_key = 4;
constexpr std::uint32_t undo_of = 5;
constexpr std::uint32_t last = undo_of;
} // namespace mf

constexpr std::uint32_t expect_absent_case = 1;
constexpr std::uint32_t expect_value_case = 2;
constexpr std::uint32_t seg_field_case = 1;
constexpr std::uint32_t seg_key_case = 2;
constexpr std::uint32_t seg_element_case = 3;

std::unexpected<error> malformed(std::string_view what) {
  return make_error(errc::type_mismatch, std::format("patch value: {}", what));
}

// ---- encoding -----------------------------------------------------------------------------

result<value> path_value(const path &p) {
  std::vector<value> segs;
  segs.reserve(p.size());
  for (const path_segment &s : p) {
    result<value> seg;
    switch (s.kind()) {
    case path_segment::kind_type::field:
      seg = value::variant(seg_field_case, value::uint(s.field_no()));
      break;
    case path_segment::kind_type::key:
      seg = value::variant(seg_key_case, s.key_value());
      break;
    case path_segment::kind_type::element:
      seg = value::variant(seg_element_case, value::id(s.element_id()));
      break;
    }
    if (!seg) {
      return seg;
    }
    segs.push_back(std::move(*seg));
  }
  return value::list(std::move(segs));
}

result<value> op_value(const patch_op &op) {
  std::vector<field> fields;
  fields.push_back({.no = of::kind, .v = value::uint(static_cast<std::uint8_t>(op.kind))});
  if (!op.target.is_null()) {
    fields.push_back({.no = of::target, .v = value::id(op.target)});
  }
  if (!op.path.empty()) {
    auto p = path_value(op.path);
    if (!p) {
      return p;
    }
    fields.push_back({.no = of::path, .v = std::move(*p)});
  }
  if (op.new_value) {
    fields.push_back({.no = of::new_value, .v = *op.new_value});
  }
  if (!op.expect.is_unchecked()) {
    auto e = op.expect.is_absent() ? value::variant(expect_absent_case, value::boolean(false))
                                   : value::variant(expect_value_case, op.expect.expected());
    if (!e) {
      return e;
    }
    fields.push_back({.no = of::expect, .v = std::move(*e)});
  }
  if (op.captured) {
    fields.push_back({.no = of::captured, .v = *op.captured});
  }
  if (!op.type.is_null()) {
    fields.push_back({.no = of::type, .v = value::id(op.type.id())});
  }
  if (op.elem != id_bytes{}) {
    fields.push_back({.no = of::elem, .v = value::id(op.elem)});
  }
  for (const auto &[no, key] : {std::pair{of::order_key, &op.order_key},
                                std::pair{of::expect_order_key, &op.expect_order_key}}) {
    if (!key->empty()) {
      auto b = value::bytes(*key);
      if (!b) {
        return b;
      }
      fields.push_back({.no = no, .v = std::move(*b)});
    }
  }
  if (op.rev != reversibility::reversible) {
    fields.push_back({.no = of::rev, .v = value::uint(static_cast<std::uint8_t>(op.rev))});
  }
  if (!op.expect_type.is_null()) {
    fields.push_back({.no = of::expect_type, .v = value::id(op.expect_type.id())});
  }
  return value::struct_(std::move(fields));
}

result<value> meta_value(const patch_meta &m) {
  std::vector<field> fields;
  for (const auto &[no, text] :
       {std::pair{mf::label, &m.label}, std::pair{mf::coalesce_key, &m.coalesce_key},
        std::pair{mf::idempotency_key, &m.idempotency_key}}) {
    if (!text->empty()) {
      auto s = value::string(*text);
      if (!s) {
        return s;
      }
      fields.push_back({.no = no, .v = std::move(*s)});
    }
  }
  if (!m.undo_group.is_null()) {
    fields.push_back({.no = mf::undo_group, .v = value::id(m.undo_group)});
  }
  if (!m.undo_of.empty()) {
    std::vector<value> ids;
    ids.reserve(m.undo_of.size());
    for (const tx_id &t : m.undo_of) {
      ids.push_back(value::id(t.id()));
    }
    auto list = value::list(std::move(ids));
    if (!list) {
      return list;
    }
    fields.push_back({.no = mf::undo_of, .v = std::move(*list)});
  }
  return value::struct_(std::move(fields));
}

// ---- decoding -----------------------------------------------------------------------------

/// Checks that `v` is a struct holding only field numbers 1..last (unless `lenient`).
result<void> check_struct(const value &v, std::uint32_t last, bool lenient, std::string_view what) {
  if (v.kind() != value_kind::struct_) {
    return malformed(std::format("{} is a {}, not a struct", what, to_string(v.kind())));
  }
  if (!lenient) {
    for (const field &f : v.fields()) {
      if (f.no < 1 || f.no > last) {
        return malformed(std::format("{} has unknown field {}", what, f.no));
      }
    }
  }
  return {};
}

result<std::uint64_t> want_uint(const value &v, std::string_view what) {
  if (v.kind() != value_kind::uint) {
    return malformed(std::format("{} is not a uint", what));
  }
  return v.as_uint();
}

result<record_id> want_id(const value &v, std::string_view what) {
  if (v.kind() != value_kind::id) {
    return malformed(std::format("{} is not an id", what));
  }
  return record_id::from_bytes(v.as_id_bytes());
}

result<type_id> want_type_id(const value &v, std::string_view what) {
  auto id = want_id(v, what);
  if (!id) {
    return std::unexpected(std::move(id.error()));
  }
  return type_id::from(*id);
}

result<order_key> want_bytes(const value &v, std::string_view what) {
  if (v.kind() != value_kind::bytes) {
    return malformed(std::format("{} is not bytes", what));
  }
  const byte_span b = v.as_bytes();
  return order_key(b.begin(), b.end());
}

result<std::string> want_string(const value &v, std::string_view what) {
  if (v.kind() != value_kind::string) {
    return malformed(std::format("{} is not a string", what));
  }
  return std::string(v.as_string());
}

result<path> path_from(const value &v) {
  if (v.kind() != value_kind::list) {
    return malformed("op path is not a list");
  }
  path p;
  p.reserve(v.items().size());
  for (const value &seg : v.items()) {
    if (seg.kind() != value_kind::variant) {
      return malformed("path segment is not a variant");
    }
    const value &payload = seg.payload();
    switch (seg.case_no()) {
    case seg_field_case: {
      if (payload.kind() != value_kind::uint ||
          payload.as_uint() > std::numeric_limits<std::uint32_t>::max()) {
        return malformed("field segment is not a u32");
      }
      p.push_back(path_segment::field(static_cast<std::uint32_t>(payload.as_uint())));
      break;
    }
    case seg_key_case: {
      auto key = path_segment::key(payload);
      if (!key) {
        return std::unexpected(std::move(key.error()));
      }
      p.push_back(std::move(*key));
      break;
    }
    case seg_element_case:
      if (payload.kind() != value_kind::id) {
        return malformed("element segment is not an id");
      }
      p.push_back(path_segment::element(payload.as_id_bytes()));
      break;
    default:
      return malformed(std::format("unknown path segment case {}", seg.case_no()));
    }
  }
  return p;
}

result<patch_op> op_from(const value &v, bool known_minor) {
  if (auto ok = check_struct(v, of::last, !known_minor, "op"); !ok) {
    return std::unexpected(std::move(ok.error()));
  }
  const value *kind = v.find_field(of::kind);
  if (kind == nullptr) {
    return malformed("op has no kind");
  }
  auto k = want_uint(*kind, "op kind");
  if (!k) {
    return std::unexpected(std::move(k.error()));
  }
  if (*k < static_cast<std::uint8_t>(op_kind::create_record) ||
      *k > static_cast<std::uint8_t>(op_kind::annotate)) {
    return make_error(errc::format_unsupported,
                      std::format("unknown op kind {} under patch major {}", *k, patch_major));
  }
  patch_op op;
  op.kind = static_cast<op_kind>(*k);
  if (const value *t = v.find_field(of::target)) {
    auto id = want_id(*t, "op target");
    if (!id) {
      return std::unexpected(std::move(id.error()));
    }
    op.target = *id;
  }
  if (const value *p = v.find_field(of::path)) {
    auto path_ok = path_from(*p);
    if (!path_ok) {
      return std::unexpected(std::move(path_ok.error()));
    }
    op.path = std::move(*path_ok);
  }
  if (const value *nv = v.find_field(of::new_value)) {
    op.new_value = *nv;
  }
  if (const value *e = v.find_field(of::expect)) {
    if (e->kind() != value_kind::variant) {
      return malformed("op expect is not a variant");
    }
    if (e->case_no() == expect_absent_case) {
      op.expect = expectation::absent();
    } else if (e->case_no() == expect_value_case) {
      op.expect = expectation::of(e->payload());
    } else {
      return malformed(std::format("unknown expect case {}", e->case_no()));
    }
  }
  if (const value *c = v.find_field(of::captured)) {
    op.captured = *c;
  }
  for (const auto &[no, out] :
       {std::pair{of::type, &op.type}, std::pair{of::expect_type, &op.expect_type}}) {
    if (const value *t = v.find_field(no)) {
      auto id = want_type_id(*t, "op type");
      if (!id) {
        return std::unexpected(std::move(id.error()));
      }
      *out = *id;
    }
  }
  if (const value *e = v.find_field(of::elem)) {
    if (e->kind() != value_kind::id) {
      return malformed("op elem is not an id");
    }
    op.elem = e->as_id_bytes();
  }
  for (const auto &[no, out] : {std::pair{of::order_key, &op.order_key},
                                std::pair{of::expect_order_key, &op.expect_order_key}}) {
    if (const value *b = v.find_field(no)) {
      auto key = want_bytes(*b, "op order key");
      if (!key) {
        return std::unexpected(std::move(key.error()));
      }
      *out = std::move(*key);
    }
  }
  if (const value *r = v.find_field(of::rev)) {
    auto rev = want_uint(*r, "op rev");
    if (!rev) {
      return std::unexpected(std::move(rev.error()));
    }
    if (*rev < static_cast<std::uint8_t>(reversibility::reversible) ||
        *rev > static_cast<std::uint8_t>(reversibility::irreversible)) {
      return malformed(std::format("unknown reversibility {}", *rev));
    }
    op.rev = static_cast<reversibility>(*rev);
  }
  return op;
}

result<patch_meta> meta_from(const value &v, bool known_minor) {
  if (auto ok = check_struct(v, mf::last, !known_minor, "meta"); !ok) {
    return std::unexpected(std::move(ok.error()));
  }
  patch_meta m;
  for (const auto &[no, out] :
       {std::pair{mf::label, &m.label}, std::pair{mf::coalesce_key, &m.coalesce_key},
        std::pair{mf::idempotency_key, &m.idempotency_key}}) {
    if (const value *s = v.find_field(no)) {
      auto text = want_string(*s, "meta text");
      if (!text) {
        return std::unexpected(std::move(text.error()));
      }
      *out = std::move(*text);
    }
  }
  if (const value *g = v.find_field(mf::undo_group)) {
    auto id = want_id(*g, "meta undo group");
    if (!id) {
      return std::unexpected(std::move(id.error()));
    }
    m.undo_group = *id;
  }
  if (const value *u = v.find_field(mf::undo_of)) {
    if (u->kind() != value_kind::list) {
      return malformed("meta undo_of is not a list");
    }
    for (const value &item : u->items()) {
      auto id = want_id(item, "meta undo_of item");
      if (!id) {
        return std::unexpected(std::move(id.error()));
      }
      auto tx = tx_id::from(*id);
      if (!tx) {
        return std::unexpected(std::move(tx.error()));
      }
      m.undo_of.push_back(*tx);
    }
  }
  return m;
}

} // namespace

result<value> to_value(const patch &p) {
  std::vector<field> fields;
  fields.push_back({.no = pf::major, .v = value::uint(p.version.major)});
  fields.push_back({.no = pf::minor, .v = value::uint(p.version.minor)});
  if (p.base) {
    fields.push_back({.no = pf::base, .v = value::digest(*p.base)});
  }
  std::vector<value> ops;
  ops.reserve(p.ops.size());
  for (const patch_op &op : p.ops) {
    auto v = op_value(op);
    if (!v) {
      return v;
    }
    ops.push_back(std::move(*v));
  }
  auto list = value::list(std::move(ops));
  if (!list) {
    return list;
  }
  fields.push_back({.no = pf::ops, .v = std::move(*list)});
  auto meta = meta_value(p.meta);
  if (!meta) {
    return meta;
  }
  fields.push_back({.no = pf::meta, .v = std::move(*meta)});
  return value::struct_(std::move(fields));
}

result<patch> from_value(const value &v) {
  if (v.kind() != value_kind::struct_) {
    return malformed("not a struct");
  }
  const value *major = v.find_field(pf::major);
  const value *minor = v.find_field(pf::minor);
  if (major == nullptr || minor == nullptr) {
    return malformed("missing version");
  }
  auto maj = want_uint(*major, "major");
  auto min = want_uint(*minor, "minor");
  if (!maj || !min) {
    return malformed("version is not a uint pair");
  }
  if (*maj != patch_major) {
    return make_error(errc::format_unsupported,
                      std::format("patch major {} (this reader knows {})", *maj, patch_major));
  }
  if (*min > std::numeric_limits<std::uint32_t>::max()) {
    return malformed("minor is not a u32");
  }
  const bool known_minor = *min <= patch_minor;
  if (auto ok = check_struct(v, pf::meta, !known_minor, "patch"); !ok) {
    return std::unexpected(std::move(ok.error()));
  }
  patch p;
  p.version = patch_version{.major = patch_major, .minor = static_cast<std::uint32_t>(*min)};
  if (const value *b = v.find_field(pf::base)) {
    if (b->kind() != value_kind::digest) {
      return malformed("base is not a digest");
    }
    p.base = b->as_digest();
  }
  if (const value *ops = v.find_field(pf::ops)) {
    if (ops->kind() != value_kind::list) {
      return malformed("ops is not a list");
    }
    p.ops.reserve(ops->items().size());
    for (std::size_t i = 0; i < ops->items().size(); ++i) {
      auto op = op_from(ops->items()[i], known_minor);
      if (!op) {
        return make_error(op.error().code, std::format("op {}: {}", i, op.error().message));
      }
      p.ops.push_back(std::move(*op));
    }
  }
  if (const value *m = v.find_field(pf::meta)) {
    auto meta = meta_from(*m, known_minor);
    if (!meta) {
      return std::unexpected(std::move(meta.error()));
    }
    p.meta = std::move(*meta);
  }
  return p;
}

} // namespace sl::fabric
