// Patch ops applied to records, the staged overlay and normalisation (WO-043, SL-ARCH-002
// ARCH2-R03, ARCH2-R04; ADR-0018).

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

namespace {

std::string describe_op(const patch_op &op) {
  return std::format("{} of {} at {}", to_string(op.kind), op.target.to_string(),
                     to_string(op.path));
}

std::unexpected<error> op_error(errc code, const patch_op &op, std::string_view why) {
  return make_error(code, std::format("{}: {}", describe_op(op), why));
}

std::unexpected<error> rewrap(const patch_op &op, const error &e) {
  return make_error(e.code, std::format("{}: {}", describe_op(op), e.message));
}

/// The value at `p` in `body`: nullopt when the last segment names an absent field, key, element
/// or variant case; an error when an earlier segment is absent or a segment does not fit.
result<std::optional<value>> lookup(const value &body, const path &p) {
  if (p.empty()) {
    return std::optional<value>(body);
  }
  const path parent(p.begin(), std::prev(p.end()));
  auto holder = body.get(parent);
  if (!holder) {
    return std::unexpected(std::move(holder.error()));
  }
  auto child = holder->get(path{p.back()});
  if (child) {
    return std::optional<value>(std::move(*child));
  }
  if (child.error().code == errc::not_found) {
    return std::optional<value>();
  }
  return std::unexpected(std::move(child.error()));
}

/// The oset entry `elem` of the oset at `p`.
result<oset_entry> element_at(const value &body, const path &p, const id_bytes &elem) {
  auto oset = body.get(p);
  if (!oset) {
    return std::unexpected(std::move(oset.error()));
  }
  if (oset->kind() != value_kind::oset) {
    return make_error(errc::type_mismatch, std::format("element operation on a {}, not an oset",
                                                       to_string(oset->kind())));
  }
  const oset_entry *e = oset->find_element(elem);
  if (e == nullptr) {
    return make_error(errc::not_found, std::format("element {} is absent",
                                                   to_string(path{path_segment::element(elem)})));
  }
  return *e;
}

path with_element(path p, const id_bytes &elem) {
  p.push_back(path_segment::element(elem));
  return p;
}

bool needs_target(op_kind k) noexcept { return k != op_kind::annotate; }

bool record_level(op_kind k) noexcept {
  return k == op_kind::create_record || k == op_kind::delete_record ||
         k == op_kind::retype_record || k == op_kind::annotate;
}

using applied = result<std::optional<record_state>>;

applied keep(record_state r) { return std::optional<record_state>(std::move(r)); }

applied apply_create(const std::optional<record_state> &before, const patch_op &op) {
  if (!op.new_value) {
    return op_error(errc::invalid_argument, op, "create_record needs a body");
  }
  if (op.type.is_null()) {
    return op_error(errc::invalid_argument, op, "create_record needs a type");
  }
  if (before) {
    return op_error(errc::already_exists, op, "the record is live");
  }
  return keep(record_state{.type = op.type, .body = *op.new_value});
}

applied apply_delete(const record_state &rec, const patch_op &op, patch_op *fill) {
  if (op.captured && *op.captured != rec.body) {
    return op_error(errc::conflict, op, "the captured body differs from the record");
  }
  if (!op.type.is_null() && op.type != rec.type) {
    return op_error(errc::conflict, op, "the captured type differs from the record");
  }
  if (fill != nullptr) {
    fill->captured = rec.body;
    fill->type = rec.type;
  }
  return std::optional<record_state>();
}

applied apply_set_unset(const record_state &rec, const patch_op &op, patch_op *fill) {
  const bool is_set = op.kind == op_kind::set;
  if (is_set && !op.new_value) {
    return op_error(errc::invalid_argument, op, "set needs a value");
  }
  if (!is_set && op.path.empty()) {
    return op_error(errc::invalid_argument, op, "unset needs a path");
  }
  if (!is_set && op.path.back().kind() == path_segment::kind_type::element) {
    return op_error(errc::invalid_argument, op, "unset cannot remove an element (remove_elem)");
  }
  auto current = lookup(rec.body, op.path);
  if (!current) {
    return rewrap(op, current.error());
  }
  if (!op.expect.holds(*current)) {
    return op_error(errc::conflict, op, "the expected value does not hold");
  }
  // is_set implies a value (checked above); the test spells it out for the optional.
  auto body =
      is_set && op.new_value ? rec.body.with(op.path, *op.new_value) : rec.body.without(op.path);
  if (!body) {
    return rewrap(op, body.error());
  }
  if (fill != nullptr && op.expect.is_unchecked()) {
    fill->expect = expectation::of(*current);
  }
  return keep(record_state{.type = rec.type, .body = std::move(*body)});
}

applied apply_insert(const record_state &rec, const patch_op &op) {
  if (!op.new_value) {
    return op_error(errc::invalid_argument, op, "insert_elem needs a value");
  }
  if (!is_valid_order_key(op.order_key)) {
    return op_error(errc::invalid_argument, op, "insert_elem needs a valid order key");
  }
  auto body = rec.body.insert_element(op.path, op.elem, op.order_key, *op.new_value);
  if (!body) {
    return rewrap(op, body.error());
  }
  return keep(record_state{.type = rec.type, .body = std::move(*body)});
}

applied apply_remove_move(const record_state &rec, const patch_op &op, patch_op *fill) {
  const bool is_move = op.kind == op_kind::move_elem;
  if (is_move && !is_valid_order_key(op.order_key)) {
    return op_error(errc::invalid_argument, op, "move_elem needs a valid order key");
  }
  auto entry = element_at(rec.body, op.path, op.elem);
  if (!entry) {
    return rewrap(op, entry.error());
  }
  if (!op.expect_order_key.empty() && op.expect_order_key != entry->order_key) {
    return op_error(errc::conflict, op, "the expected order key does not hold");
  }
  if (!is_move && op.captured && *op.captured != entry->v) {
    return op_error(errc::conflict, op, "the captured element differs");
  }
  auto body = is_move ? rec.body.move_element(op.path, op.elem, op.order_key)
                      : rec.body.without(with_element(op.path, op.elem));
  if (!body) {
    return rewrap(op, body.error());
  }
  if (fill != nullptr) {
    fill->expect_order_key = entry->order_key;
    if (!is_move) {
      fill->captured = entry->v;
    }
  }
  return keep(record_state{.type = rec.type, .body = std::move(*body)});
}

applied apply_retype(const record_state &rec, const patch_op &op, patch_op *fill) {
  if (!op.new_value) {
    return op_error(errc::invalid_argument, op, "retype_record needs a body");
  }
  if (op.type.is_null()) {
    return op_error(errc::invalid_argument, op, "retype_record needs a type");
  }
  if (op.captured && *op.captured != rec.body) {
    return op_error(errc::conflict, op, "the captured body differs from the record");
  }
  if (!op.expect_type.is_null() && op.expect_type != rec.type) {
    return op_error(errc::conflict, op, "the expected type does not hold");
  }
  if (fill != nullptr) {
    fill->captured = rec.body;
    fill->expect_type = rec.type;
  }
  return keep(record_state{.type = op.type, .body = *op.new_value});
}

/// apply_op, and when `fill` is set, the op with its empty expectations filled.
applied apply_impl(const std::optional<record_state> &before, const patch_op &op, patch_op *fill) {
  if (needs_target(op.kind) && op.target.is_null()) {
    return op_error(errc::invalid_argument, op, "the op needs a target record");
  }
  if (record_level(op.kind) && !op.path.empty()) {
    return op_error(errc::invalid_argument, op, "the op takes no path");
  }
  if (op.kind == op_kind::annotate) {
    return before;
  }
  if (op.kind == op_kind::create_record) {
    return apply_create(before, op);
  }
  if (!before) {
    return op_error(errc::not_found, op, "the record is absent");
  }
  const record_state &rec = *before;
  switch (op.kind) {
  case op_kind::delete_record:
    return apply_delete(rec, op, fill);
  case op_kind::set:
  case op_kind::unset:
    return apply_set_unset(rec, op, fill);
  case op_kind::insert_elem:
    return apply_insert(rec, op);
  case op_kind::remove_elem:
  case op_kind::move_elem:
    return apply_remove_move(rec, op, fill);
  case op_kind::retype_record:
    return apply_retype(rec, op, fill);
  case op_kind::create_record:
  case op_kind::annotate:
    break;
  }
  return op_error(errc::invalid_argument, op, "unknown op kind");
}

std::unexpected<error> at_index(std::size_t i, const error &e) {
  return make_error(e.code, std::format("op {}: {}", i, e.message));
}

} // namespace

std::string_view to_string(op_kind k) noexcept {
  switch (k) {
  case op_kind::create_record:
    return "create_record";
  case op_kind::delete_record:
    return "delete_record";
  case op_kind::set:
    return "set";
  case op_kind::unset:
    return "unset";
  case op_kind::insert_elem:
    return "insert_elem";
  case op_kind::remove_elem:
    return "remove_elem";
  case op_kind::move_elem:
    return "move_elem";
  case op_kind::retype_record:
    return "retype_record";
  case op_kind::annotate:
    return "annotate";
  }
  return "unknown";
}

std::string_view to_string(reversibility r) noexcept {
  switch (r) {
  case reversibility::reversible:
    return "reversible";
  case reversibility::compensable:
    return "compensable";
  case reversibility::irreversible:
    return "irreversible";
  }
  return "unknown";
}

// ---- expectation --------------------------------------------------------------------------

expectation expectation::absent() noexcept {
  expectation e;
  e.kind_ = kind_type::absent;
  return e;
}

expectation expectation::of(value v) noexcept {
  expectation e;
  e.kind_ = kind_type::present;
  e.v_ = std::move(v);
  return e;
}

expectation expectation::of(const std::optional<value> &v) noexcept {
  return v ? of(*v) : absent();
}

bool expectation::holds(const std::optional<value> &current) const noexcept {
  switch (kind_) {
  case kind_type::unchecked:
    return true;
  case kind_type::absent:
    return !current.has_value();
  case kind_type::present:
    return current.has_value() && *current == v_;
  }
  return false;
}

std::optional<value> expectation::as_optional() const {
  return kind_ == kind_type::present ? std::optional<value>(v_) : std::nullopt;
}

bool operator==(const expectation &a, const expectation &b) noexcept {
  return a.kind_ == b.kind_ && (a.kind_ != expectation::kind_type::present || a.v_ == b.v_);
}

// ---- op constructors ----------------------------------------------------------------------

patch_op op_create(const record_id &target, const type_id &type, value body) {
  patch_op op;
  op.kind = op_kind::create_record;
  op.target = target;
  op.type = type;
  op.new_value = std::move(body);
  return op;
}

patch_op op_delete(const record_id &target) {
  patch_op op;
  op.kind = op_kind::delete_record;
  op.target = target;
  return op;
}

patch_op op_set(const record_id &target, path p, value v, expectation expect) {
  patch_op op;
  op.kind = op_kind::set;
  op.target = target;
  op.path = std::move(p);
  op.new_value = std::move(v);
  op.expect = std::move(expect);
  return op;
}

patch_op op_unset(const record_id &target, path p, expectation expect) {
  patch_op op;
  op.kind = op_kind::unset;
  op.target = target;
  op.path = std::move(p);
  op.expect = std::move(expect);
  return op;
}

patch_op op_insert(const record_id &target, path oset, const id_bytes &elem, order_key key,
                   value v) {
  patch_op op;
  op.kind = op_kind::insert_elem;
  op.target = target;
  op.path = std::move(oset);
  op.elem = elem;
  op.order_key = std::move(key);
  op.new_value = std::move(v);
  return op;
}

patch_op op_remove(const record_id &target, path oset, const id_bytes &elem) {
  patch_op op;
  op.kind = op_kind::remove_elem;
  op.target = target;
  op.path = std::move(oset);
  op.elem = elem;
  return op;
}

patch_op op_move(const record_id &target, path oset, const id_bytes &elem, order_key key) {
  patch_op op;
  op.kind = op_kind::move_elem;
  op.target = target;
  op.path = std::move(oset);
  op.elem = elem;
  op.order_key = std::move(key);
  return op;
}

patch_op op_retype(const record_id &target, const type_id &type, value body) {
  patch_op op;
  op.kind = op_kind::retype_record;
  op.target = target;
  op.type = type;
  op.new_value = std::move(body);
  return op;
}

patch_op op_annotate(value note) {
  patch_op op;
  op.kind = op_kind::annotate;
  op.new_value = std::move(note);
  return op;
}

// ---- apply and normalize ------------------------------------------------------------------

result<std::optional<record_state>> apply_op(const std::optional<record_state> &before,
                                             const patch_op &op) {
  return apply_impl(before, op, nullptr);
}

result<patch_op> normalize_op(const std::optional<record_state> &before, const patch_op &op) {
  patch_op filled = op;
  auto after = apply_impl(before, op, &filled);
  if (!after) {
    return std::unexpected(std::move(after.error()));
  }
  return filled;
}

result<std::optional<record_state>> staged_state::read(const record_id &id) const {
  const auto it = std::ranges::lower_bound(changes_, id, {}, &staged_change::id);
  if (it != changes_.end() && it->id == id) {
    return it->after;
  }
  return base_->read(id);
}

result<void> staged_state::apply(const patch_op &op) {
  if (op.kind == op_kind::annotate) {
    auto checked = apply_impl(std::nullopt, op, nullptr);
    if (!checked) {
      return std::unexpected(std::move(checked.error()));
    }
    return {};
  }
  auto before = read(op.target);
  if (!before) {
    return std::unexpected(std::move(before.error()));
  }
  auto after = apply_impl(*before, op, nullptr);
  if (!after) {
    return std::unexpected(std::move(after.error()));
  }
  const auto it = std::ranges::lower_bound(changes_, op.target, {}, &staged_change::id);
  if (it != changes_.end() && it->id == op.target) {
    it->after = std::move(*after);
  } else {
    changes_.insert(it, staged_change{.id = op.target, .after = std::move(*after)});
  }
  return {};
}

result<void> staged_state::apply(const patch &p) {
  std::vector<staged_change> saved = changes_;
  for (std::size_t i = 0; i < p.ops.size(); ++i) {
    if (auto ok = apply(p.ops[i]); !ok) {
      changes_ = std::move(saved);
      return at_index(i, ok.error());
    }
  }
  return {};
}

result<patch> normalize(const patch &p, const state_reader &state) {
  staged_state staged(state);
  patch out = p;
  for (std::size_t i = 0; i < out.ops.size(); ++i) {
    patch_op &op = out.ops[i];
    std::optional<record_state> before;
    if (op.kind != op_kind::annotate) {
      auto read = staged.read(op.target);
      if (!read) {
        return at_index(i, read.error());
      }
      before = std::move(*read);
    }
    auto filled = normalize_op(before, op);
    if (!filled) {
      return at_index(i, filled.error());
    }
    op = std::move(*filled);
    if (auto ok = staged.apply(op); !ok) {
      return at_index(i, ok.error());
    }
  }
  return out;
}

bool is_normalized(const patch &p) noexcept {
  return std::ranges::all_of(p.ops, [](const patch_op &op) {
    switch (op.kind) {
    case op_kind::delete_record:
      return op.captured.has_value() && !op.type.is_null();
    case op_kind::set:
    case op_kind::unset:
      return !op.expect.is_unchecked();
    case op_kind::remove_elem:
      return op.captured.has_value() && !op.expect_order_key.empty();
    case op_kind::move_elem:
      return !op.expect_order_key.empty();
    case op_kind::retype_record:
      return op.captured.has_value() && !op.expect_type.is_null();
    case op_kind::create_record:
    case op_kind::insert_elem:
    case op_kind::annotate:
      return true;
    }
    return false;
  });
}

} // namespace sl::fabric
