// Seeded random stores and patches (WO-043); see patch_gen.hpp.

#include "patch_gen.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "sl/core/assert.hpp"
#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric::testing {

namespace {

template <class T> T ok(result<T> r) {
  SL_ASSERT(r.has_value(), "patch_gen built something invalid");
  return std::move(*r);
}

constexpr std::size_t max_levels = 3;     // nested containers in generated values
constexpr std::uint32_t max_field_no = 8; // field numbers 1..8
constexpr std::uint64_t key_names = 6;    // map keys "k0".."k5"

value key_value(std::uint64_t n) { return ok(value::string(std::format("k{}", n))); }

constexpr std::array<op_kind, 9> all_kinds = {
    op_kind::create_record, op_kind::delete_record, op_kind::set,
    op_kind::unset,         op_kind::insert_elem,   op_kind::remove_elem,
    op_kind::move_elem,     op_kind::retype_record, op_kind::annotate};

void walk(const value &v, const path &at, value_kind parent, std::size_t max_depth,
          std::vector<body_node> &out) {
  out.push_back(body_node{.path = at, .v = v, .parent = parent});
  if (at.size() >= max_depth) {
    return;
  }
  const auto step = [&](path_segment s, const value &child) {
    path p = at;
    p.push_back(std::move(s));
    walk(child, p, v.kind(), max_depth, out);
  };
  switch (v.kind()) {
  case value_kind::struct_:
    for (const field &f : v.fields()) {
      step(path_segment::field(f.no), f.v);
    }
    break;
  case value_kind::map:
    for (const map_entry &e : v.entries()) {
      step(ok(path_segment::key(e.key)), e.v);
    }
    break;
  case value_kind::oset:
    for (const oset_entry &e : v.elements()) {
      step(path_segment::element(e.element), e.v);
    }
    break;
  case value_kind::variant:
    step(path_segment::field(v.case_no()), v.payload());
    break;
  default:
    break;
  }
}

template <class T> const T &pick(rng &r, const std::vector<T> &items) {
  return items[static_cast<std::size_t>(r.below(items.size()))];
}

} // namespace

patch_gen::patch_gen(std::uint64_t seed, patch_gen_options options)
    : values_(seed, value_gen_options{.max_depth = 0, .max_items = 3, .max_chars = 8}),
      options_(options) {
  options_.max_ops = std::max<std::size_t>(options_.max_ops, 1);
  options_.type_pool = std::max<std::size_t>(options_.type_pool, 1);
}

bool patch_gen::prefill() noexcept { return random().chance(options_.prefill_percent); }

type_id patch_gen::type() {
  return type_id_of(std::format("gen.rec{}", random().below(options_.type_pool)));
}

value patch_gen::node(std::size_t levels) {
  rng &r = random();
  if (levels == 0 || r.chance(40)) {
    return values_.any();
  }
  constexpr std::array<value_kind, 6> kinds = {value_kind::struct_, value_kind::struct_,
                                               value_kind::map,     value_kind::oset,
                                               value_kind::variant, value_kind::list};
  return make(std::span(kinds)[static_cast<std::size_t>(r.below(kinds.size()))], levels);
}

value patch_gen::make(value_kind kind, std::size_t levels) {
  rng &r = random();
  const std::size_t below = levels > 0 ? levels - 1 : 0;
  const std::uint64_t n = r.below(4);
  switch (kind) {
  case value_kind::struct_: {
    std::vector<field> fields;
    for (std::uint32_t no = 1; no <= max_field_no && fields.size() < n; ++no) {
      if (r.chance(40)) {
        fields.push_back(field{.no = no, .v = node(below)});
      }
    }
    return ok(value::struct_(std::move(fields)));
  }
  case value_kind::map: {
    std::vector<map_entry> entries;
    for (std::uint64_t k = 0; k < key_names && entries.size() < n; ++k) {
      if (r.chance(40)) {
        entries.push_back(map_entry{.key = key_value(k), .v = node(below)});
      }
    }
    return ok(value::map(std::move(entries)));
  }
  case value_kind::oset: {
    std::vector<oset_entry> entries;
    for (std::uint64_t i = 0; i < n; ++i) {
      std::vector<std::uint8_t> key = values_.random_order_key();
      if (!entries.empty() && r.chance(options_.tie_percent)) {
        key = pick(r, entries).order_key;
      }
      entries.push_back(oset_entry{.element = values_.random_id(id_class::element).bytes(),
                                   .order_key = std::move(key),
                                   .v = node(below)});
    }
    return ok(value::oset(std::move(entries)));
  }
  case value_kind::variant:
    return ok(value::variant(static_cast<std::uint32_t>(1 + r.below(3)), node(below)));
  default: {
    std::vector<value> items;
    items.reserve(static_cast<std::size_t>(n));
    for (std::uint64_t i = 0; i < n; ++i) {
      items.push_back(values_.any());
    }
    return ok(value::list(std::move(items)));
  }
  }
}

value patch_gen::body() {
  rng &r = random();
  std::vector<field> fields;
  const std::uint64_t n = 1 + r.below(5);
  for (std::uint32_t no = 1; no <= max_field_no && fields.size() < n; ++no) {
    if (r.chance(60)) {
      // Field 1 is an oset and field 2 a struct, so element and nested ops find targets.
      value v;
      if (no == 1) {
        v = make(value_kind::oset, max_levels);
      } else if (no == 2) {
        v = make(value_kind::struct_, max_levels);
      } else {
        v = node(max_levels);
      }
      fields.push_back(field{.no = no, .v = std::move(v)});
    }
  }
  return ok(value::struct_(std::move(fields)));
}

patch_meta patch_gen::meta() {
  patch_meta m;
  rng &r = random();
  if (!r.chance(options_.meta_percent)) {
    return m;
  }
  m.label = std::format("edit {}", r.below(1000));
  if (r.chance(50)) {
    m.undo_group = values_.random_id(id_class::operation);
  }
  if (r.chance(50)) {
    m.coalesce_key = std::format("drag/{}", r.below(10));
  }
  if (r.chance(50)) {
    m.idempotency_key = std::format("key-{}", r.next());
  }
  for (std::uint64_t i = r.below(3); i > 0; --i) {
    m.undo_of.push_back(ok(tx_id::from(values_.random_id(id_class::tx))));
  }
  return m;
}

std::vector<body_node> patch_gen::nodes(const value &body) const {
  std::vector<body_node> out;
  walk(body, {}, value_kind::struct_, options_.max_path_depth, out);
  return out;
}

std::vector<std::uint8_t> patch_gen::key_near(const value &oset) {
  rng &r = random();
  const auto elements = oset.elements();
  if (!elements.empty() && r.chance(options_.tie_percent)) {
    return elements[static_cast<std::size_t>(r.below(elements.size()))].order_key;
  }
  if (!elements.empty() && r.chance(50)) {
    // Between two neighbours (or past an end), the way an editor inserts.
    const auto i = static_cast<std::size_t>(r.below(elements.size() + 1));
    const byte_span lo = i == 0 ? byte_span{} : byte_span(elements[i - 1].order_key);
    const byte_span hi = i == elements.size() ? byte_span{} : byte_span(elements[i].order_key);
    if ((lo.empty() || is_valid_order_key(lo)) && (hi.empty() || is_valid_order_key(hi))) {
      if (auto k = between(lo, hi)) {
        return *k;
      }
    }
  }
  return values_.random_order_key();
}

result<oracle_store> patch_gen::state(std::size_t records) {
  oracle_store store;
  while (store.size() < records) {
    store.put(values_.random_id(id_class::entity), record_state{.type = type(), .body = body()});
  }
  return store;
}

result<std::optional<patch_op>> patch_gen::op(const oracle_store &state, op_kind kind) {
  rng &r = random();
  std::vector<record_id> ids;
  ids.reserve(state.size());
  for (const auto &entry : state.records()) {
    ids.push_back(entry.first);
  }
  if (kind == op_kind::annotate) {
    return std::optional<patch_op>(
        op_annotate(ok(value::string(std::format("note {}", r.below(100))))));
  }
  if (kind == op_kind::create_record) {
    record_id id = values_.random_id(id_class::entity);
    while (state.records().contains(id)) {
      id = values_.random_id(id_class::entity);
    }
    return std::optional<patch_op>(op_create(id, type(), body()));
  }
  if (ids.empty()) {
    return std::optional<patch_op>();
  }
  const record_id target = pick(r, ids);
  const auto found = state.records().find(target);
  SL_ASSERT(found != state.records().end(), "patch_gen lost a record");
  const record_state &rec = found->second;
  switch (kind) {
  case op_kind::delete_record: {
    patch_op op = op_delete(target);
    if (prefill()) {
      op.captured = rec.body;
      op.type = rec.type;
    }
    return std::optional<patch_op>(std::move(op));
  }
  case op_kind::retype_record: {
    patch_op op = op_retype(target, type(), body());
    if (prefill()) {
      op.captured = rec.body;
      op.expect_type = rec.type;
    }
    return std::optional<patch_op>(std::move(op));
  }
  default:
    break;
  }
  const std::vector<body_node> all = nodes(rec.body);
  switch (kind) {
  case op_kind::set: {
    if (r.chance(options_.whole_body_percent)) {
      patch_op op = op_set(target, {}, body());
      if (prefill()) {
        op.expect = expectation::of(rec.body);
      }
      return std::optional<patch_op>(std::move(op));
    }
    std::vector<const body_node *> replace;
    std::vector<const body_node *> extend;
    for (const body_node &n : all) {
      if (!n.path.empty()) {
        replace.push_back(&n);
      }
      if (n.path.size() < options_.max_path_depth &&
          (n.v.kind() == value_kind::struct_ || n.v.kind() == value_kind::map)) {
        extend.push_back(&n);
      }
    }
    if (!extend.empty() && (replace.empty() || r.chance(40))) {
      const body_node &n = *pick(r, extend);
      path p = n.path;
      const std::uint64_t which =
          r.below(n.v.kind() == value_kind::struct_ ? max_field_no : key_names);
      if (n.v.kind() == value_kind::struct_) {
        p.push_back(path_segment::field(static_cast<std::uint32_t>(which + 1)));
      } else {
        p.push_back(ok(path_segment::key(key_value(which))));
      }
      const std::size_t levels = max_levels > p.size() ? max_levels - p.size() : 0;
      patch_op op = op_set(target, p, node(levels));
      if (prefill()) {
        auto now = rec.body.get(p);
        op.expect = now ? expectation::of(*now) : expectation::absent();
      }
      return std::optional<patch_op>(std::move(op));
    }
    if (replace.empty()) {
      return std::optional<patch_op>();
    }
    const body_node &n = *pick(r, replace);
    const std::size_t levels = max_levels > n.path.size() ? max_levels - n.path.size() : 0;
    patch_op op = op_set(target, n.path, node(levels));
    if (prefill()) {
      op.expect = expectation::of(n.v);
    }
    return std::optional<patch_op>(std::move(op));
  }
  case op_kind::unset: {
    std::vector<const body_node *> fields;
    for (const body_node &n : all) {
      if (!n.path.empty() && n.path.back().kind() != path_segment::kind_type::element &&
          n.parent != value_kind::variant) {
        fields.push_back(&n);
      }
    }
    if (fields.empty()) {
      return std::optional<patch_op>();
    }
    const body_node &n = *pick(r, fields);
    patch_op op = op_unset(target, n.path);
    if (prefill()) {
      op.expect = expectation::of(n.v);
    }
    return std::optional<patch_op>(std::move(op));
  }
  case op_kind::insert_elem: {
    std::vector<const body_node *> osets;
    for (const body_node &n : all) {
      if (n.v.kind() == value_kind::oset && n.path.size() < options_.max_path_depth) {
        osets.push_back(&n);
      }
    }
    if (osets.empty()) {
      return std::optional<patch_op>();
    }
    const body_node &n = *pick(r, osets);
    id_bytes elem = values_.random_id(id_class::element).bytes();
    while (n.v.find_element(elem) != nullptr) {
      elem = values_.random_id(id_class::element).bytes();
    }
    const std::size_t levels = max_levels > n.path.size() + 1 ? max_levels - n.path.size() - 1 : 0;
    return std::optional<patch_op>(op_insert(target, n.path, elem, key_near(n.v), node(levels)));
  }
  case op_kind::remove_elem:
  case op_kind::move_elem: {
    std::vector<const body_node *> elems;
    for (const body_node &n : all) {
      if (!n.path.empty() && n.path.back().kind() == path_segment::kind_type::element) {
        elems.push_back(&n);
      }
    }
    if (elems.empty()) {
      return std::optional<patch_op>();
    }
    const body_node &n = *pick(r, elems);
    path oset_path = n.path;
    const id_bytes elem = oset_path.back().element_id();
    oset_path.pop_back();
    const value oset = ok(rec.body.get(oset_path));
    const oset_entry *entry = oset.find_element(elem);
    SL_ASSERT(entry != nullptr, "patch_gen lost an element");
    if (kind == op_kind::remove_elem) {
      patch_op op = op_remove(target, oset_path, elem);
      if (prefill()) {
        op.captured = entry->v;
        op.expect_order_key = entry->order_key;
      }
      return std::optional<patch_op>(std::move(op));
    }
    patch_op op = op_move(target, oset_path, elem, key_near(oset));
    if (prefill()) {
      op.expect_order_key = entry->order_key;
    }
    return std::optional<patch_op>(std::move(op));
  }
  default:
    break;
  }
  return std::optional<patch_op>();
}

result<patch> patch_gen::next(const oracle_store &state) {
  rng &r = random();
  oracle_store work = state;
  patch p;
  const std::uint64_t count = 1 + r.below(options_.max_ops);
  for (std::uint64_t i = 0; i < count; ++i) {
    std::vector<op_kind> order(all_kinds.begin(), all_kinds.end());
    for (std::size_t k = order.size() - 1; k > 0; --k) {
      std::swap(order[k], order[static_cast<std::size_t>(r.below(k + 1))]);
    }
    for (const op_kind kind : order) {
      auto made = op(work, kind);
      if (!made) {
        return std::unexpected(std::move(made.error()));
      }
      if (!*made) {
        continue;
      }
      if (auto applied = work.apply(**made); !applied) {
        return make_error(
            applied.error().code,
            std::format("patch_gen made an op that does not apply: {}", applied.error().message));
      }
      p.ops.push_back(std::move(**made));
      break;
    }
  }
  if (r.chance(options_.meta_percent)) {
    digest base;
    for (auto &b : base.bytes) {
      b = static_cast<std::uint8_t>(r.next());
    }
    p.base = base;
  }
  p.meta = meta();
  return p;
}

} // namespace sl::fabric::testing
