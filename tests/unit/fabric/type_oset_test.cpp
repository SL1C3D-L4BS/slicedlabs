// FAB-INV-TY-06 (SL-ARCH-001 ARCH1-R15): oset element ids are unique and order keys valid; a
// list of identity-bearing structs is ill-formed (identity-bearing collections are osets).

#include "sl/fabric/schema.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"

#include "../../../engine/fabric/testing/test_schemas.hpp"
#include "../../../engine/fabric/testing/value_gen.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;
namespace tt = sl::fabric::testing::test_types;
using bytes_t = std::vector<std::uint8_t>;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

id_bytes elem(std::string_view key) { return testing::test_id(id_class::element, key).bytes(); }

oset_entry entry(std::string_view key, bytes_t order, value v) {
  return oset_entry{.element = elem(key), .order_key = std::move(order), .v = std::move(v)};
}

type_desc named(std::string_view name, type_kind kind) {
  type_desc t;
  t.name = std::string(name);
  t.id = type_id_of(name);
  t.kind = kind;
  return t;
}

field_desc fld(std::uint32_t no, std::string_view name, std::string_view type) {
  field_desc f;
  f.no = no;
  f.name = std::string(name);
  f.type = type_id_of(type);
  return f;
}

sl::result<void> freeze_set(std::vector<type_desc> types) {
  schema_registry reg;
  for (type_desc &t : types) {
    REQUIRE(reg.add(std::move(t)).has_value());
  }
  return reg.freeze();
}

bool mentions(const sl::result<void> &r, std::string_view text) {
  return !r && r.error().message.contains(text);
}

} // namespace

TEST_CASE("FAB-INV-TY-06: element ids are unique in an oset") {
  const auto dup =
      value::oset({entry("a", {0x10}, value::uint(1)), entry("a", {0x20}, value::uint(2))});
  REQUIRE_FALSE(dup.has_value());
  CHECK(dup.error().code == sl::errc::encode_invalid);
  CHECK(dup.error().message.starts_with("R-SER-DUP"));

  const value s = must(value::oset({entry("a", {0x10}, value::uint(1))}));
  const auto again = s.insert_element({}, elem("a"), {0x30}, value::uint(2));
  REQUIRE_FALSE(again.has_value());
  CHECK(again.error().code == sl::errc::already_exists);
  CHECK(s.elements().size() == 1);
}

TEST_CASE("FAB-INV-TY-06: entries iterate by order key, then element id") {
  std::vector<oset_entry> in = {
      entry("c", {0x80}, value::uint(3)), entry("a", {0x40}, value::uint(1)),
      entry("b", {0x80}, value::uint(2)), entry("d", {0x40, 0x01}, value::uint(4))};
  const value one = must(value::oset(in));
  std::ranges::reverse(in);
  const value two = must(value::oset(in));
  CHECK(one == two); // independent of input order
  const auto es = one.elements();
  REQUIRE(es.size() == 4);
  CHECK(es[0].element == elem("a"));
  CHECK(es[1].element == elem("d")); // 40 01 sorts after 40
  // Equal order keys converge by element id (ARCH2-R48).
  const bool b_first = elem("b") < elem("c");
  CHECK(es[2].element == (b_first ? elem("b") : elem("c")));
  CHECK(es[3].element == (b_first ? elem("c") : elem("b")));

  // move_element reorders; insert_element lands at its key.
  const value moved = must(one.move_element({}, elem("a"), {0xF0}));
  CHECK(moved.elements().back().element == elem("a"));
  const value grown = must(one.insert_element({}, elem("e"), {0x01}, value::uint(5)));
  CHECK(grown.elements().front().element == elem("e"));
  const auto missing = one.move_element({}, elem("zz"), {0x05});
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error().code == sl::errc::not_found);
}

TEST_CASE("FAB-INV-TY-06: order key validity and keys between keys") {
  CHECK_FALSE(is_valid_order_key(bytes_t{}));
  CHECK_FALSE(is_valid_order_key(bytes_t{0x00}));
  CHECK_FALSE(is_valid_order_key(bytes_t{0x61, 0x00}));
  CHECK(is_valid_order_key(bytes_t{0x01}));
  CHECK(is_valid_order_key(bytes_t{0x00, 0x01}));
  CHECK(is_valid_order_key(bytes_t{0xFF, 0xFF}));

  const auto below = [](const bytes_t &a, const bytes_t &b) {
    return std::ranges::lexicographical_compare(a, b);
  };
  // Open bounds, adjacent bytes and 0xFF runs.
  const std::vector<std::pair<bytes_t, bytes_t>> pairs = {
      {{}, {}},
      {{}, {0x01}},
      {{0xFF}, {}},
      {{0x01}, {0x02}},
      {{0x01}, {0x01, 0x01}},
      {{0x7F, 0xFF}, {0x80}},
      {{0xFF, 0xFF}, {}},
      {{0x10}, {0x10, 0x00, 0x01}},
  };
  for (const auto &[lo, hi] : pairs) {
    const bytes_t mid = must(order_key_between(lo, hi));
    CHECK(is_valid_order_key(mid));
    CHECK((lo.empty() || below(lo, mid)));
    CHECK((hi.empty() || below(mid, hi)));
  }
  CHECK_FALSE(order_key_between(bytes_t{0x02}, bytes_t{0x01}).has_value());
  CHECK_FALSE(order_key_between(bytes_t{0x02}, bytes_t{0x02}).has_value());
  CHECK_FALSE(order_key_between(bytes_t{0x02, 0x00}, bytes_t{}).has_value());

  // Seeded random pairs, and repeated insertion at the front, back and middle.
  testing::value_gen gen(42);
  for (int i = 0; i < 2000; ++i) {
    bytes_t a = gen.random_order_key();
    bytes_t b = gen.random_order_key();
    if (a == b) {
      continue;
    }
    if (below(b, a)) {
      std::swap(a, b);
    }
    const bytes_t mid = must(order_key_between(a, b));
    CHECK(is_valid_order_key(mid));
    CHECK(below(a, mid));
    CHECK(below(mid, b));
  }
  bytes_t lo = must(order_key_between({}, {}));
  bytes_t hi = lo;
  for (int i = 0; i < 200; ++i) {
    const bytes_t front = must(order_key_between({}, lo));
    const bytes_t back = must(order_key_between(hi, {}));
    const bytes_t middle = must(order_key_between(front, lo));
    CHECK(below(front, middle));
    CHECK(below(middle, lo));
    lo = front;
    hi = back;
  }
  CHECK(below(lo, hi));
}

TEST_CASE("FAB-INV-TY-06: check refuses invalid order keys and non-element ids") {
  const schema_registry reg = must(testing::make_test_registry());
  const value entity = must(testing::sample_entity("hero"));
  const value comps = *entity.find_field(4);
  const oset_entry first = comps.elements().front();

  // Raw osets with a bad key or a bad element id, swapped into field 4.
  const auto checked = [&](oset_entry e) {
    const value bad = must(value::oset({std::move(e)}));
    std::vector<diagnostic_lite> out;
    const auto r =
        reg.check(must(entity.with({path_segment::field(4)}, bad)), type_id_of(tt::entity), out);
    CHECK_FALSE(r.has_value());
    return out;
  };
  for (const bytes_t &key : {bytes_t{}, bytes_t{0x00}, bytes_t{0x05, 0x00}}) {
    oset_entry e = first;
    e.order_key = key;
    const auto out = checked(e);
    REQUIRE(out.size() == 1);
    CHECK(out[0].rule == "R-TY-ORDER-KEY");
    CHECK(out[0].path == path{path_segment::field(4), path_segment::element(first.element)});
  }
  oset_entry ent = first;
  ent.element = testing::test_id(id_class::entity, "not-an-element").bytes();
  const auto out = checked(ent);
  REQUIRE(out.size() == 1);
  CHECK(out[0].rule == "R-TY-ELEMENT");
  // The value model itself accepts any key and any 16 bytes (SLB/1 does).
  oset_entry raw = first;
  raw.element = id_bytes{};
  raw.order_key = {};
  CHECK(value::oset({raw}).has_value());
}

TEST_CASE("FAB-INV-TY-06: a list of identity-bearing structs is ill-formed") {
  type_desc with_id = named("t.Item", type_kind::struct_);
  with_id.fields = {fld(1, "id", "id"), fld(2, "label", "string")};
  type_desc list = named("t.Items", type_kind::list);
  list.element = with_id.id;
  const auto refused = freeze_set({with_id, list});
  CHECK(mentions(refused, "R-TY-LIST-ID"));
  CHECK(mentions(refused, "t.Items"));

  // Through a tagged id, and a record type as element: refused too.
  type_desc handle = named("t.Handle", type_kind::tagged);
  handle.element = type_id_of("id");
  type_desc tagged_item = named("t.Item", type_kind::struct_);
  tagged_item.fields = {fld(1, "who", "t.Handle")};
  CHECK(mentions(freeze_set({handle, tagged_item, list}), "R-TY-LIST-ID"));
  type_desc rec = named("t.Item", type_kind::struct_);
  rec.record_class = id_class::entity;
  rec.state_class = state_class::document;
  rec.domain = "content.t";
  CHECK(mentions(freeze_set({rec, list}), "R-TY-LIST-ID"));

  // The same element in an oset is fine, and so is a list of plain structs.
  type_desc oset = named("t.Items", type_kind::oset);
  oset.element = with_id.id;
  CHECK(freeze_set({with_id, oset}).has_value());
  type_desc plain = named("t.Item", type_kind::struct_);
  plain.fields = {fld(1, "x", "f32")};
  CHECK(freeze_set({plain, list}).has_value());
  // Lists of scalars and of ids themselves are atomic values, not identity-bearing structs.
  type_desc ids = named("t.Ids", type_kind::list);
  ids.element = type_id_of("id");
  CHECK(freeze_set({ids}).has_value());
}
