// FAB-INV-TY-03 (SL-ARCH-001 ARCH1-R16): with and without leave the original bit-identical and
// share every untouched subtree (pointer equality of the shared nodes).

#include "sl/fabric/value.hpp"

#include "../../../engine/fabric/testing/test_schemas.hpp"
#include "../../../engine/fabric/testing/value_gen.hpp"

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

value str(std::string_view s) { return must(value::string(s)); }

/// A full dump of a value: kinds, payload bytes and float bits, in iteration order.
void render(const value &v, std::string &out) {
  out += std::format("{}(", to_string(v.kind()));
  switch (v.kind()) {
  case value_kind::bool_:
    out += v.as_bool() ? "1" : "0";
    break;
  case value_kind::uint:
    out += std::format("{}", v.as_uint());
    break;
  case value_kind::nint:
    out += std::format("{}", v.nint_magnitude());
    break;
  case value_kind::f32:
    out += std::format("{:08x}", std::bit_cast<std::uint32_t>(v.as_f32()));
    break;
  case value_kind::f64:
    out += std::format("{:016x}", std::bit_cast<std::uint64_t>(v.as_f64()));
    break;
  case value_kind::string:
    out += v.as_string();
    break;
  case value_kind::bytes:
    for (const auto b : v.as_bytes()) {
      out += std::format("{:02x}", b);
    }
    break;
  case value_kind::id:
    for (const auto b : v.as_id_bytes()) {
      out += std::format("{:02x}", b);
    }
    break;
  case value_kind::digest:
    out += std::format("{}:", static_cast<int>(v.as_digest().algo));
    for (const auto b : v.as_digest().bytes) {
      out += std::format("{:02x}", b);
    }
    break;
  case value_kind::list:
    for (const value &x : v.items()) {
      render(x, out);
    }
    break;
  case value_kind::map:
    for (const map_entry &e : v.entries()) {
      render(e.key, out);
      render(e.v, out);
    }
    break;
  case value_kind::struct_:
    for (const field &f : v.fields()) {
      out += std::format("{}=", f.no);
      render(f.v, out);
    }
    break;
  case value_kind::variant:
    out += std::format("{}=", v.case_no());
    render(v.payload(), out);
    break;
  case value_kind::oset:
    for (const oset_entry &e : v.elements()) {
      for (const auto b : e.element) {
        out += std::format("{:02x}", b);
      }
      out += '/';
      for (const auto b : e.order_key) {
        out += std::format("{:02x}", b);
      }
      out += '=';
      render(e.v, out);
    }
    break;
  }
  out += ')';
}

void addresses(const value &v, std::vector<const void *> &out) {
  out.push_back(v.node_address());
  switch (v.kind()) {
  case value_kind::list:
    for (const value &x : v.items()) {
      addresses(x, out);
    }
    break;
  case value_kind::map:
    for (const map_entry &e : v.entries()) {
      addresses(e.key, out);
      addresses(e.v, out);
    }
    break;
  case value_kind::struct_:
    for (const field &f : v.fields()) {
      addresses(f.v, out);
    }
    break;
  case value_kind::variant:
    addresses(v.payload(), out);
    break;
  case value_kind::oset:
    for (const oset_entry &e : v.elements()) {
      addresses(e.v, out);
    }
    break;
  default:
    break;
  }
}

/// Everything observable about a value: its dump and the address of every node.
struct snapshot {
  std::string dump;
  std::vector<const void *> nodes;

  explicit snapshot(const value &v) {
    render(v, dump);
    addresses(v, nodes);
  }
  friend bool operator==(const snapshot &, const snapshot &) = default;
};

/// The children of a container as (segment, value) pairs.
std::vector<std::pair<path_segment, value>> children(const value &v) {
  std::vector<std::pair<path_segment, value>> out;
  switch (v.kind()) {
  case value_kind::struct_:
    for (const field &f : v.fields()) {
      out.emplace_back(path_segment::field(f.no), f.v);
    }
    break;
  case value_kind::map:
    for (const map_entry &e : v.entries()) {
      out.emplace_back(must(path_segment::key(e.key)), e.v);
    }
    break;
  case value_kind::oset:
    for (const oset_entry &e : v.elements()) {
      out.emplace_back(path_segment::element(e.element), e.v);
    }
    break;
  case value_kind::variant:
    out.emplace_back(path_segment::field(v.case_no()), v.payload());
    break;
  default:
    break;
  }
  return out;
}

/// Every child of every container on `p` that is not on `p` is the same node in both values.
void check_shared(const value &before, const value &after, const path &p) {
  value b = before;
  value a = after;
  for (const path_segment &seg : p) {
    for (const auto &[s, child] : children(b)) {
      if (s == seg) {
        continue;
      }
      const auto other = a.get({s});
      REQUIRE(other.has_value());
      CHECK(other->node_address() == child.node_address());
    }
    const auto nb = b.get({seg});
    const auto na = a.get({seg});
    if (!nb || !na) {
      return; // the edited child was inserted or removed here
    }
    b = *nb;
    a = *na;
  }
}

/// A random existing path into `v` (possibly empty).
path random_path(const value &v, testing::rng &r) {
  path p;
  value cur = v;
  while (true) {
    const auto kids = children(cur);
    if (kids.empty() || r.chance(25)) {
      return p;
    }
    const auto &[seg, child] = kids[r.below(kids.size())];
    p.push_back(seg);
    cur = child;
  }
}

} // namespace

TEST_CASE("FAB-INV-TY-03: with leaves the original intact and shares untouched subtrees") {
  const value entity = must(testing::sample_entity("hero"));
  const snapshot before(entity);
  const record_id tag_elem = testing::test_id(id_class::element, "hero/tag");
  const path label = {path_segment::field(4), path_segment::element(tag_elem),
                      path_segment::field(3), path_segment::field(1)};
  const value renamed = must(entity.with(label, str("villain")));

  CHECK(snapshot(entity) == before);
  CHECK(must(entity.get(label)) == str("hero"));
  CHECK(must(renamed.get(label)) == str("villain"));
  CHECK_FALSE(renamed == entity);
  check_shared(entity, renamed, label);
  // Untouched fields are the very same nodes; the nodes on the path are new.
  CHECK(renamed.find_field(1)->node_address() == entity.find_field(1)->node_address());
  CHECK(renamed.find_field(5)->node_address() == entity.find_field(5)->node_address());
  CHECK(renamed.node_address() != entity.node_address());
  CHECK(renamed.find_field(4)->node_address() != entity.find_field(4)->node_address());
  const record_id tr_elem = testing::test_id(id_class::element, "hero/transform");
  CHECK(renamed.find_field(4)->find_element(tr_elem.bytes())->v.node_address() ==
        entity.find_field(4)->find_element(tr_elem.bytes())->v.node_address());

  // Inserting a field or a map key shares the rest too.
  const path owner = {path_segment::field(5), must(path_segment::key(str("zeta")))};
  const value keyed = must(entity.with(owner, str("z")));
  CHECK(snapshot(entity) == before);
  check_shared(entity, keyed, owner);
  CHECK(keyed.find_field(5)->entries().size() == 2);
  // Setting a child to the node it already holds returns the same value.
  const value same = must(entity.with({path_segment::field(1)}, *entity.find_field(1)));
  CHECK(same.node_address() == entity.node_address());
}

TEST_CASE("FAB-INV-TY-03: without and element edits leave the original intact") {
  const value scene = must(testing::sample_scene("level", 6));
  const snapshot before(scene);
  const record_id slot = testing::test_id(id_class::element, "level/slot3");
  const path p = {path_segment::field(2), path_segment::element(slot)};

  const value removed = must(scene.without(p));
  CHECK(snapshot(scene) == before);
  CHECK(removed.find_field(2)->elements().size() == 5);
  check_shared(scene, removed, p);
  CHECK_FALSE(removed.get(p).has_value());
  // Removing what is absent returns the same node.
  CHECK(must(removed.without(p)).node_address() == removed.node_address());
  CHECK(must(scene.without({path_segment::field(3)})).node_address() == scene.node_address());

  const path oset_path = {path_segment::field(2)};
  const value moved = must(scene.move_element(oset_path, slot.bytes(), {0x01}));
  const value inserted = must(scene.insert_element(
      oset_path, testing::test_id(id_class::element, "new").bytes(), {0xFE}, value::uint(1)));
  CHECK(snapshot(scene) == before);
  CHECK(moved.find_field(2)->elements().front().element == slot.bytes());
  CHECK(moved.find_field(1)->node_address() == scene.find_field(1)->node_address());
  CHECK(inserted.find_field(2)->elements().size() == 7);
  for (const oset_entry &e : scene.find_field(2)->elements()) {
    CHECK(moved.find_field(2)->find_element(e.element)->v.node_address() == e.v.node_address());
    CHECK(inserted.find_field(2)->find_element(e.element)->v.node_address() == e.v.node_address());
  }
}

TEST_CASE("FAB-INV-TY-03: random edits over generated values") {
  for (std::uint64_t seed = 1; seed <= 400; ++seed) {
    CAPTURE(seed);
    testing::value_gen gen(seed, {.max_depth = 5, .max_items = 5, .max_chars = 8});
    const value v = gen.any();
    const snapshot before(v);
    const path p = random_path(v, gen.random());
    const value replacement = gen.any();
    CAPTURE(to_string(p));

    const auto edited = v.with(p, replacement);
    if (!edited) {
      // Only the depth limit can refuse an edit of an existing path.
      CHECK(edited.error().code == sl::errc::encode_invalid);
      CHECK(snapshot(v) == before);
      continue;
    }
    CHECK(snapshot(v) == before);
    CHECK(must(edited->get(p)) == replacement);
    check_shared(v, *edited, p);

    if (!p.empty() && v.get(path(p.begin(), p.end() - 1))->kind() != value_kind::variant) {
      const value removed = must(v.without(p));
      CHECK(snapshot(v) == before);
      CHECK_FALSE(removed.get(p).has_value());
      check_shared(v, removed, p);
    }
  }
}

TEST_CASE("FAB-INV-TY-03: path errors leave the original intact") {
  const value entity = must(testing::sample_entity("hero"));
  const snapshot before(entity);
  // A key segment on a struct, a missing intermediate, an element on a map.
  const auto mismatch = entity.with({must(path_segment::key(value::uint(1)))}, value::uint(1));
  REQUIRE_FALSE(mismatch.has_value());
  CHECK(mismatch.error().code == sl::errc::type_mismatch);
  const auto missing =
      entity.with({path_segment::field(6), path_segment::field(1)}, value::uint(1));
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error().code == sl::errc::not_found);
  const auto on_map = entity.get({path_segment::field(5), path_segment::element(record_id{})});
  REQUIRE_FALSE(on_map.has_value());
  CHECK(on_map.error().code == sl::errc::type_mismatch);
  // Lists are atomic: no segment enters one.
  const value with_list =
      must(entity.with({path_segment::field(6)}, must(value::list({must(value::f32(1.0F))}))));
  CHECK(with_list.get({path_segment::field(6), path_segment::field(0)}).error().code ==
        sl::errc::type_mismatch);
  // Without on the empty path or a variant payload.
  CHECK(entity.without({}).error().code == sl::errc::type_mismatch);
  const value variant = must(value::variant(2, value::uint(1)));
  CHECK(variant.without({path_segment::field(2)}).error().code == sl::errc::type_mismatch);
  CHECK(must(variant.with({path_segment::field(2)}, value::uint(5))).payload() == value::uint(5));
  CHECK(variant.with({path_segment::field(3)}, value::uint(5)).error().code == sl::errc::not_found);
  // A path segment key must be a key kind.
  CHECK(path_segment::key(must(value::f64(1.0))).error().code == sl::errc::type_mismatch);
  CHECK(snapshot(entity) == before);
}

TEST_CASE("FAB-INV-TY-03: values are canonical SLB/1 values") {
  // Floats: finite only, -0.0 stored as +0.0 (ARCH1-R27).
  CHECK(value::f64(std::numeric_limits<double>::quiet_NaN()).error().code ==
        sl::errc::encode_invalid);
  CHECK(value::f32(std::numeric_limits<float>::infinity()).error().code ==
        sl::errc::encode_invalid);
  CHECK(std::bit_cast<std::uint64_t>(must(value::f64(-0.0)).as_f64()) == 0);
  CHECK(std::bit_cast<std::uint32_t>(must(value::f32(-0.0F)).as_f32()) == 0);
  CHECK(must(value::f64(-0.0)) == must(value::f64(0.0)));
  // Strings: well-formed UTF-8, no NUL.
  CHECK(value::string("a\xC3\xA9\xF0\x9F\x98\x80").has_value());
  CHECK(value::string(std::string_view("a\0b", 3)).error().code == sl::errc::encode_invalid);
  for (const std::string_view bad :
       {"\xC3", "\xC0\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xFF", "\x80"}) {
    CHECK(value::string(bad).error().message.starts_with("R-SER-UTF8"));
  }
  // Integers: uint 0..2^64-1, nint -2^64..-1.
  CHECK(value::integer(-1).kind() == value_kind::nint);
  CHECK(value::integer(-1).nint_magnitude() == 0);
  CHECK(value::integer(5).kind() == value_kind::uint);
  CHECK(value::integer(std::numeric_limits<std::int64_t>::min()).as_i64() ==
        std::numeric_limits<std::int64_t>::min());
  CHECK_FALSE(value::nint(std::numeric_limits<std::uint64_t>::max()).as_i64().has_value());
  // Maps: keys of key kinds, sorted by encoded bytes, unique.
  const value m = must(value::map({map_entry{.key = str("b"), .v = value::uint(1)},
                                   map_entry{.key = str("aa"), .v = value::uint(2)},
                                   map_entry{.key = value::uint(16384), .v = value::uint(3)},
                                   map_entry{.key = value::uint(129), .v = value::uint(4)},
                                   map_entry{.key = value::integer(-1), .v = value::uint(5)}}));
  std::vector<std::string> order;
  for (const map_entry &e : m.entries()) {
    std::string s;
    render(e.key, s);
    order.push_back(s);
  }
  // 16384 encodes 80 80 01, 129 encodes 81 01; "b" (len 1) before "aa" (len 2).
  CHECK(order ==
        std::vector<std::string>{"uint(16384)", "uint(129)", "nint(0)", "string(b)", "string(aa)"});
  CHECK(value::map({map_entry{.key = str("k"), .v = value::uint(1)},
                    map_entry{.key = str("k"), .v = value::uint(2)}})
            .error()
            .message.starts_with("R-SER-DUP"));
  CHECK(value::map({map_entry{.key = must(value::f32(1.0F)), .v = value::uint(1)}})
            .error()
            .message.starts_with("R-SER-TAG"));
  // Structs: ascending field numbers, unique.
  const value s = must(
      value::struct_({field{.no = 9, .v = value::uint(1)}, field{.no = 2, .v = value::uint(2)}}));
  CHECK(s.fields().front().no == 2);
  CHECK(value::struct_({field{.no = 1, .v = value::uint(1)}, field{.no = 1, .v = value::uint(1)}})
            .error()
            .message.starts_with("R-SER-DUP"));
  // Depth: 64 nested containers are fine, 65 are refused.
  value deep = value::uint(0);
  for (int i = 0; i < 64; ++i) {
    deep = must(value::list({deep}));
  }
  CHECK(deep.depth() == 64);
  CHECK(value::list({deep}).error().message.starts_with("R-SER-LIMIT"));
  CHECK(value::variant(1, deep).error().code == sl::errc::encode_invalid);
  // Equality is structural, not by node.
  CHECK(must(testing::sample_entity("a")) == must(testing::sample_entity("a")));
  CHECK_FALSE(must(testing::sample_entity("a")) == must(testing::sample_entity("b")));
  CHECK(value{} == value::boolean(false));
  CHECK_FALSE(value::boolean(true) == value::boolean(false));
  CHECK(to_string(path{}) == "/");
}
