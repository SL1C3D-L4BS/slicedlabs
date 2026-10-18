// FAB-INV-UNDO-03 (SL-ARCH-002 ARCH2-R22): apply(compose(a, b)) equals applying a then b.

#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/value.hpp"

#include "../../../engine/fabric/testing/model_store.hpp"
#include "../../../engine/fabric/testing/patch_gen.hpp"
#include "../../../engine/fabric/testing/test_schemas.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

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

tg::oracle_store applied(tg::oracle_store s, const patch &p) {
  must_ok(s.apply(p));
  return s;
}

} // namespace

TEST_CASE("FAB-INV-UNDO-03: 500 seeded triples: compose equals sequence and is associative") {
  std::size_t coalesced = 0;
  for (std::uint64_t seed = 0; seed < 500; ++seed) {
    CAPTURE(seed);
    // One record so the patches keep touching the same paths and coalescing happens.
    tg::patch_gen gen(seed + 3000);
    const tg::oracle_store s0 = must(gen.state(seed % 3 == 0 ? 1 : 3));
    const bool normalised = seed % 2 == 0;
    const auto make = [&](const tg::oracle_store &s) {
      patch p = must(gen.next(s));
      return normalised ? must(normalize(p, s)) : p;
    };
    const patch a = make(s0);
    const tg::oracle_store s1 = applied(s0, a);
    const patch b = make(s1);
    const tg::oracle_store s2 = applied(s1, b);
    const patch c = make(s2);
    const tg::oracle_store s3 = applied(s2, c);

    const patch ab = must(compose(a, b));
    coalesced += ab.ops.size() < a.ops.size() + b.ops.size() ? 1U : 0U;
    CHECK(applied(s0, ab) == s2);

    const patch left = must(compose(ab, c));
    const patch right = must(compose(a, must(compose(b, c))));
    CHECK(left == right);
    CHECK(applied(s0, left) == s3);

    if (normalised) {
      // Composition of normalised patches is normalised, so a group undoes as one patch.
      CHECK(is_normalized(left));
      CHECK(applied(s3, invert(left)) == s0);
    }
  }
  CHECK(coalesced > 5); // coalescing happens in the random runs too
}

TEST_CASE("FAB-INV-UNDO-03: repeated sets keep the first expectation and the last value") {
  const record_id id = tg::test_id(id_class::entity, "compose/e");
  const path name{path_segment::field(1)};
  const value e = must(value::string("e"));
  const value f = must(value::string("f"));
  const value g = must(value::string("g"));

  patch a;
  a.ops.push_back(op_set(id, name, f, expectation::of(e)));
  patch b;
  b.ops.push_back(op_set(id, path{path_segment::field(2)}, value::uint(1)));
  b.ops.push_back(op_set(id, name, g, expectation::of(f)));
  const patch ab = must(compose(a, b));
  REQUIRE(ab.ops.size() == 2);
  CHECK(ab.ops[0] == op_set(id, name, g, expectation::of(e)));

  // A set then an unset of the same path is one unset expecting the original.
  patch u;
  u.ops.push_back(op_unset(id, name, expectation::of(f)));
  const patch au = must(compose(a, u));
  REQUIRE(au.ops.size() == 1);
  CHECK(au.ops[0] == op_unset(id, name, expectation::of(e)));

  // A later op that expects what the earlier one does not leave can never apply: conflict.
  patch contradict;
  contradict.ops.push_back(op_set(id, name, g, expectation::of(e)));
  const auto bad = compose(a, contradict);
  REQUIRE_FALSE(bad.has_value());
  CHECK(bad.error().code == sl::errc::conflict);

  // A deeper write between two sets of a path blocks coalescing (order matters there).
  patch nested;
  nested.ops.push_back(op_set(id, path{path_segment::field(3)}, must(value::struct_({}))));
  nested.ops.push_back(
      op_set(id, path{path_segment::field(3), path_segment::field(1)}, value::uint(1)));
  nested.ops.push_back(op_set(id, path{path_segment::field(3)}, must(value::struct_({}))));
  CHECK(must(compose(nested, patch{})).ops.size() == 3);

  // Majors must agree; minors take the larger; base and meta come from the first patch.
  patch other;
  other.version.major = 2;
  CHECK(compose(a, other).error().code == sl::errc::invalid_argument);
  patch newer;
  newer.version.minor = 4;
  a.meta.label = "first";
  newer.meta.label = "second";
  const patch an = must(compose(a, newer));
  CHECK(an.version.minor == 4);
  CHECK(an.meta.label == "first");
}

TEST_CASE("FAB-INV-UNDO-03: a group of inverses composed in reverse undoes the group") {
  const record_id id = tg::test_id(id_class::entity, "compose/group");
  tg::oracle_store s0;
  s0.put(id, record_state{.type = type_id_of(tt::entity), .body = must(tg::sample_entity("g"))});
  for (std::uint64_t seed = 0; seed < 100; ++seed) {
    CAPTURE(seed);
    tg::patch_gen gen(seed + 9000);
    tg::oracle_store s = s0;
    patch undo;
    for (int i = 0; i < 4; ++i) {
      const patch n = must(normalize(must(gen.next(s)), s));
      must_ok(s.apply(n));
      undo = must(compose(invert(n), undo));
    }
    CHECK(applied(s, undo) == s0);
  }
}
