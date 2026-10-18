// FAB-INV-MRG-03 (SL-ARCH-002 ARCH2-R48): concurrent oset inserts converge in both orders, ties
// broken by element id.

#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"

#include "../../../engine/fabric/testing/model_store.hpp"
#include "../../../engine/fabric/testing/patch_gen.hpp"
#include "../../../engine/fabric/testing/test_schemas.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;
namespace tg = sl::fabric::testing;
namespace tt = sl::fabric::testing::test_types;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

void must_ok(const sl::result<void> &r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
}

/// The held value of `o` (a failed REQUIRE otherwise).
template <class T> T must_have(const std::optional<T> &o) {
  REQUIRE(o.has_value());
  return o.value_or(T{});
}

const record_id scene = tg::test_id(id_class::entity, "oset/scene");
path entities() { return path{path_segment::field(2)}; }

tg::oracle_store fixture() {
  tg::oracle_store s;
  s.put(scene, record_state{.type = type_id_of(tt::scene), .body = must(tg::sample_scene("s", 3))});
  return s;
}

} // namespace

TEST_CASE("FAB-INV-MRG-03: 300 seeded rounds of concurrent inserts converge in every order") {
  const tg::oracle_store base = fixture();
  const value initial = must(must_have(must(base.read(scene))).body.get(entities()));
  for (std::uint64_t seed = 0; seed < 300; ++seed) {
    CAPTURE(seed);
    tg::patch_gen gen(seed + 14000);
    tg::rng &r = gen.random();
    // Each replica inserts between the same two neighbours, often with the same key.
    const std::size_t gap = static_cast<std::size_t>(r.below(initial.elements().size() + 1));
    const byte_span lo = gap == 0 ? byte_span{} : byte_span(initial.elements()[gap - 1].order_key);
    const byte_span hi = gap == initial.elements().size()
                             ? byte_span{}
                             : byte_span(initial.elements()[gap].order_key);
    const order_key shared = must(between(lo, hi));
    std::vector<patch> inserts;
    const std::size_t replicas = 2 + static_cast<std::size_t>(r.below(4));
    for (std::size_t i = 0; i < replicas; ++i) {
      const order_key key = r.chance(70) ? shared : must(between(lo, shared));
      patch p;
      p.ops.push_back(op_insert(scene, entities(),
                                gen.values().random_id(id_class::element).bytes(), key,
                                value::id(gen.values().random_id(id_class::entity))));
      inserts.push_back(std::move(p));
    }
    for (std::size_t i = 0; i < inserts.size(); ++i) {
      for (std::size_t j = i + 1; j < inserts.size(); ++j) {
        CHECK(commutes(footprint_of(inserts[i]), footprint_of(inserts[j])));
      }
    }
    // Apply in order and in several shuffled orders: every replica ends identical.
    tg::oracle_store reference = base;
    for (const patch &p : inserts) {
      must_ok(reference.apply(p));
    }
    for (int round = 0; round < 4; ++round) {
      std::vector<patch> order = inserts;
      for (std::size_t k = order.size() - 1; k > 0; --k) {
        std::swap(order[k], order[static_cast<std::size_t>(r.below(k + 1))]);
      }
      staged_state replica(base);
      for (const patch &p : order) {
        must_ok(replica.apply(p));
      }
      CHECK(tg::materialize(base, replica) == reference);
    }
    // The converged order is (order key, element id): equal keys sort by element id.
    const value merged = must(must_have(must(reference.read(scene))).body.get(entities()));
    const auto elems = merged.elements();
    REQUIRE(elems.size() == initial.elements().size() + replicas);
    for (std::size_t k = 1; k < elems.size(); ++k) {
      CHECK(std::is_lt(compare_order(elems[k - 1].order_key, elems[k - 1].element,
                                     elems[k].order_key, elems[k].element)));
    }
    // Every insert sits in the gap it was aimed at: the base elements keep their relative order
    // and the inserts are contiguous at the gap.
    for (std::size_t k = 0; k < initial.elements().size(); ++k) {
      const std::size_t at = k < gap ? k : k + replicas;
      CHECK(elems[at].element == initial.elements()[k].element);
    }
  }
}

TEST_CASE("FAB-INV-MRG-03: two replicas, same key, both orders, ties by element id") {
  const tg::oracle_store base = fixture();
  const value initial = must(must_have(must(base.read(scene))).body.get(entities()));
  const order_key key =
      must(between(initial.elements()[0].order_key, initial.elements()[1].order_key));
  const id_bytes low = tg::test_id(id_class::element, "oset/a").bytes();
  const id_bytes high = tg::test_id(id_class::element, "oset/b").bytes();
  REQUIRE(low != high);
  const id_bytes first = std::min(low, high);
  const id_bytes second = std::max(low, high);
  patch a;
  a.ops.push_back(op_insert(scene, entities(), second, key, value::uint(1)));
  patch b;
  b.ops.push_back(op_insert(scene, entities(), first, key, value::uint(2)));

  tg::oracle_store ab = base;
  must_ok(ab.apply(a));
  must_ok(ab.apply(b));
  tg::oracle_store ba = base;
  must_ok(ba.apply(b));
  must_ok(ba.apply(a));
  CHECK(ab == ba);
  const auto elems = must(must_have(must(ab.read(scene))).body.get(entities())).elements();
  REQUIRE(elems.size() == 5);
  CHECK(elems[0].element == initial.elements()[0].element);
  CHECK(elems[1].element == first);
  CHECK(elems[2].element == second);
  CHECK(elems[3].element == initial.elements()[1].element);
}

TEST_CASE("FAB-INV-MRG-03: between and compare_order") {
  const order_key a{0x40};
  const order_key b{0x80};
  const order_key mid = must(between(a, b));
  CHECK(std::is_lt(compare_order(a, {}, mid, {})));
  CHECK(std::is_lt(compare_order(mid, {}, b, {})));
  CHECK(is_valid_order_key(must(between({}, {}))));
  CHECK(is_valid_order_key(must(between(a, {}))));
  CHECK(is_valid_order_key(must(between({}, a))));
  CHECK(must(between(a, a)) == a); // a tie: the element id decides
  CHECK(between(b, a).error().code == sl::errc::invalid_argument);
  CHECK(between(order_key{0x40, 0x00}, order_key{0x40, 0x00}).error().code ==
        sl::errc::invalid_argument);
  const id_bytes e1{1};
  const id_bytes e2{2};
  CHECK(std::is_lt(compare_order(a, e1, a, e2)));
  CHECK(std::is_gt(compare_order(b, e1, a, e2)));
  CHECK(std::is_eq(compare_order(a, e1, a, e1)));
}
