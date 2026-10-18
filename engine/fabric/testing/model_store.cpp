// The test oracle store (WO-043). Plain, independent op semantics; see oracle_store.hpp.

#include "model_store.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric::testing {

namespace {

std::unexpected<error> refuse(errc code, const patch_op &op, std::string_view why) {
  return make_error(
      code, std::format("model: {} of {}: {}", to_string(op.kind), op.target.to_string(), why));
}

/// The value at `p`, nullopt when only the last step is missing; an error when the parent is.
result<std::optional<value>> at_path(const value &body, const path &p) {
  if (p.empty()) {
    return std::optional<value>(body);
  }
  path parent = p;
  parent.pop_back();
  auto holder = body.get(parent);
  if (!holder) {
    return std::unexpected(holder.error());
  }
  const path_segment &last = p.back();
  const value *found = nullptr;
  switch (last.kind()) {
  case path_segment::kind_type::field:
    if (holder->kind() == value_kind::struct_) {
      found = holder->find_field(last.field_no());
    } else if (holder->kind() == value_kind::variant) {
      found = holder->case_no() == last.field_no() ? &holder->payload() : nullptr;
    } else {
      return make_error(errc::type_mismatch, "model: field step into a non-struct");
    }
    break;
  case path_segment::kind_type::key:
    if (holder->kind() != value_kind::map) {
      return make_error(errc::type_mismatch, "model: key step into a non-map");
    }
    found = holder->find_key(last.key_value());
    break;
  case path_segment::kind_type::element: {
    if (holder->kind() != value_kind::oset) {
      return make_error(errc::type_mismatch, "model: element step into a non-oset");
    }
    const oset_entry *e = holder->find_element(last.element_id());
    found = e != nullptr ? &e->v : nullptr;
    break;
  }
  }
  return found != nullptr ? std::optional<value>(*found) : std::nullopt;
}

bool expect_ok(const expectation &e, const std::optional<value> &now) {
  if (e.is_unchecked()) {
    return true;
  }
  if (e.is_absent()) {
    return !now;
  }
  return now && *now == e.expected();
}

void feed(sha256 &h, std::uint64_t n) {
  std::array<std::uint8_t, 8> b{};
  std::size_t shift = 0;
  for (std::uint8_t &x : b) {
    x = static_cast<std::uint8_t>(n >> shift);
    shift += 8;
  }
  h.update(b);
}

void feed_value(sha256 &h, const value &v) {
  feed(h, static_cast<std::uint8_t>(v.kind()));
  switch (v.kind()) {
  case value_kind::bool_:
    feed(h, v.as_bool() ? 1U : 0U);
    break;
  case value_kind::uint:
    feed(h, v.as_uint());
    break;
  case value_kind::nint:
    feed(h, v.nint_magnitude());
    break;
  case value_kind::f32:
    feed(h, std::bit_cast<std::uint32_t>(v.as_f32()));
    break;
  case value_kind::f64:
    feed(h, std::bit_cast<std::uint64_t>(v.as_f64()));
    break;
  case value_kind::string:
    feed(h, v.as_string().size());
    h.update(v.as_string());
    break;
  case value_kind::bytes:
    feed(h, v.as_bytes().size());
    h.update(v.as_bytes());
    break;
  case value_kind::id:
    h.update(v.as_id_bytes());
    break;
  case value_kind::digest: {
    const digest d = v.as_digest();
    feed(h, static_cast<std::uint8_t>(d.algo));
    h.update(d.bytes);
    break;
  }
  case value_kind::list:
    feed(h, v.items().size());
    for (const value &x : v.items()) {
      feed_value(h, x);
    }
    break;
  case value_kind::map:
    feed(h, v.entries().size());
    for (const map_entry &e : v.entries()) {
      feed_value(h, e.key);
      feed_value(h, e.v);
    }
    break;
  case value_kind::struct_:
    feed(h, v.fields().size());
    for (const field &f : v.fields()) {
      feed(h, f.no);
      feed_value(h, f.v);
    }
    break;
  case value_kind::variant:
    feed(h, v.case_no());
    feed_value(h, v.payload());
    break;
  case value_kind::oset:
    feed(h, v.elements().size());
    for (const oset_entry &e : v.elements()) {
      h.update(e.element);
      feed(h, e.order_key.size());
      h.update(e.order_key);
      feed_value(h, e.v);
    }
    break;
  }
}

} // namespace

result<std::optional<record_state>> oracle_store::read(const record_id &id) const {
  const auto it = records_.find(id);
  return it != records_.end() ? std::optional<record_state>(it->second) : std::nullopt;
}

void oracle_store::put(const record_id &id, record_state r) { records_[id] = std::move(r); }

void oracle_store::erase(const record_id &id) { records_.erase(id); }

result<void> oracle_store::apply(const patch &p) {
  oracle_store copy = *this;
  for (const patch_op &op : p.ops) {
    if (auto ok = copy.apply(op); !ok) {
      return ok;
    }
  }
  *this = std::move(copy);
  return {};
}

result<void> oracle_store::apply(const patch_op &op) {
  if (op.kind == op_kind::annotate) {
    return {};
  }
  const auto it = records_.find(op.target);
  if (op.kind == op_kind::create_record) {
    if (it != records_.end()) {
      return refuse(errc::already_exists, op, "live");
    }
    if (!op.new_value || op.type.is_null() || op.target.is_null() || !op.path.empty()) {
      return refuse(errc::invalid_argument, op, "malformed");
    }
    records_.emplace(op.target, record_state{.type = op.type, .body = *op.new_value});
    return {};
  }
  if (it == records_.end()) {
    return refuse(errc::not_found, op, "absent");
  }
  record_state &rec = it->second;
  switch (op.kind) {
  case op_kind::delete_record:
    if ((op.captured && *op.captured != rec.body) || (!op.type.is_null() && op.type != rec.type)) {
      return refuse(errc::conflict, op, "captured differs");
    }
    records_.erase(it);
    return {};
  case op_kind::set: {
    auto now = at_path(rec.body, op.path);
    if (!now) {
      return std::unexpected(now.error());
    }
    if (!op.new_value) {
      return refuse(errc::invalid_argument, op, "no value");
    }
    if (!expect_ok(op.expect, *now)) {
      return refuse(errc::conflict, op, "expect");
    }
    auto body = rec.body.with(op.path, *op.new_value);
    if (!body) {
      return std::unexpected(body.error());
    }
    rec.body = *body;
    return {};
  }
  case op_kind::unset: {
    if (op.path.empty() || op.path.back().kind() == path_segment::kind_type::element) {
      return refuse(errc::invalid_argument, op, "bad unset path");
    }
    auto now = at_path(rec.body, op.path);
    if (!now) {
      return std::unexpected(now.error());
    }
    if (!expect_ok(op.expect, *now)) {
      return refuse(errc::conflict, op, "expect");
    }
    auto body = rec.body.without(op.path);
    if (!body) {
      return std::unexpected(body.error());
    }
    rec.body = *body;
    return {};
  }
  case op_kind::insert_elem: {
    if (!op.new_value || !is_valid_order_key(op.order_key)) {
      return refuse(errc::invalid_argument, op, "malformed");
    }
    auto body = rec.body.insert_element(op.path, op.elem, op.order_key, *op.new_value);
    if (!body) {
      return std::unexpected(body.error());
    }
    rec.body = *body;
    return {};
  }
  case op_kind::remove_elem:
  case op_kind::move_elem: {
    auto oset = rec.body.get(op.path);
    if (!oset) {
      return std::unexpected(oset.error());
    }
    if (oset->kind() != value_kind::oset) {
      return refuse(errc::type_mismatch, op, "not an oset");
    }
    const oset_entry *e = oset->find_element(op.elem);
    if (e == nullptr) {
      return refuse(errc::not_found, op, "no element");
    }
    if (!op.expect_order_key.empty() && op.expect_order_key != e->order_key) {
      return refuse(errc::conflict, op, "order key");
    }
    if (op.kind == op_kind::remove_elem) {
      if (op.captured && *op.captured != e->v) {
        return refuse(errc::conflict, op, "captured");
      }
      path p = op.path;
      p.push_back(path_segment::element(op.elem));
      auto body = rec.body.without(p);
      if (!body) {
        return std::unexpected(body.error());
      }
      rec.body = *body;
      return {};
    }
    if (!is_valid_order_key(op.order_key)) {
      return refuse(errc::invalid_argument, op, "bad key");
    }
    auto body = rec.body.move_element(op.path, op.elem, op.order_key);
    if (!body) {
      return std::unexpected(body.error());
    }
    rec.body = *body;
    return {};
  }
  case op_kind::retype_record:
    if (!op.new_value || op.type.is_null()) {
      return refuse(errc::invalid_argument, op, "malformed");
    }
    if ((op.captured && *op.captured != rec.body) ||
        (!op.expect_type.is_null() && op.expect_type != rec.type)) {
      return refuse(errc::conflict, op, "captured differs");
    }
    rec = record_state{.type = op.type, .body = *op.new_value};
    return {};
  case op_kind::create_record:
  case op_kind::annotate:
    break;
  }
  return {};
}

digest oracle_store::root_digest() const {
  sha256 h;
  h.update(std::string_view("model-root/1"));
  feed(h, records_.size());
  for (const auto &[id, rec] : records_) {
    h.update(id.bytes());
    h.update(rec.type.id().bytes());
    feed_value(h, rec.body);
  }
  return digest{.algo = hash_algo::sha256, .bytes = h.finish()};
}

oracle_store materialize(const oracle_store &base, const staged_state &staged) {
  oracle_store out = base;
  for (const staged_change &c : staged.changes()) {
    if (c.after) {
      out.put(c.id, *c.after);
    } else {
      out.erase(c.id);
    }
  }
  return out;
}

} // namespace sl::fabric::testing
