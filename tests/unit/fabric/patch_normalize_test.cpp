// FAB-INV-PAT-04 (SL-ARCH-002 ARCH2-R04): normalize fills expect and captured from the staged
// state; a normalised patch is self-inverting.

#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/value.hpp"

#include "../../../engine/fabric/testing/model_store.hpp"
#include "../../../engine/fabric/testing/patch_gen.hpp"
#include "../../../engine/fabric/testing/test_schemas.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
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

/// The held value of `o` (a failed REQUIRE otherwise).
template <class T> T must_have(const std::optional<T> &o) {
  REQUIRE(o.has_value());
  return o.value_or(T{});
}

std::optional<value> at(const value &body, const path &p) {
  auto v = body.get(p);
  return v ? std::optional<value>(*v) : std::nullopt;
}

/// Checks that `filled` is `authored` with exactly the expectations the oracle state implies.
void check_filled(const patch_op &authored, const patch_op &filled,
                  const std::optional<record_state> &before) {
  CHECK(filled.kind == authored.kind);
  CHECK(filled.target == authored.target);
  CHECK(filled.path == authored.path);
  CHECK(filled.new_value == authored.new_value);
  CHECK(filled.order_key == authored.order_key);
  CHECK(filled.rev == authored.rev);
  switch (authored.kind) {
  case op_kind::set:
  case op_kind::unset:
    REQUIRE(before);
    CHECK(filled.expect == expectation::of(at(must_have(before).body, authored.path)));
    break;
  case op_kind::delete_record:
    REQUIRE(before);
    CHECK(filled.captured == must_have(before).body);
    CHECK(filled.type == must_have(before).type);
    break;
  case op_kind::remove_elem:
  case op_kind::move_elem: {
    REQUIRE(before);
    const value oset = must(must_have(before).body.get(authored.path));
    const oset_entry *e = oset.find_element(authored.elem);
    REQUIRE(e != nullptr);
    CHECK(filled.expect_order_key == e->order_key);
    if (authored.kind == op_kind::remove_elem) {
      CHECK(filled.captured == e->v);
    }
    break;
  }
  case op_kind::retype_record:
    REQUIRE(before);
    CHECK(filled.captured == must_have(before).body);
    CHECK(filled.expect_type == must_have(before).type);
    CHECK(filled.type == authored.type);
    break;
  case op_kind::create_record:
  case op_kind::insert_elem:
  case op_kind::annotate:
    CHECK(filled == authored);
    break;
  }
}

} // namespace

TEST_CASE("FAB-INV-PAT-04: normalize fills every expectation from the state each op sees") {
  for (std::uint64_t seed = 0; seed < 500; ++seed) {
    CAPTURE(seed);
    tg::patch_gen gen(seed + 7000);
    const tg::oracle_store base = must(gen.state(5));
    const patch p = must(gen.next(base));
    const patch n = must(normalize(p, base));
    CHECK(is_normalized(n));
    REQUIRE(n.ops.size() == p.ops.size());
    CHECK(n.meta == p.meta);
    CHECK(n.base == p.base);

    tg::oracle_store step = base;
    for (std::size_t i = 0; i < p.ops.size(); ++i) {
      CAPTURE(i);
      const std::optional<record_state> before =
          p.ops[i].kind == op_kind::annotate ? std::nullopt : must(step.read(p.ops[i].target));
      check_filled(p.ops[i], n.ops[i], before);
      must_ok(step.apply(n.ops[i]));
    }

    // Idempotent, and the filled patch applies like the authored one.
    CHECK(must(normalize(n, base)) == n);
    tg::oracle_store authored = base;
    must_ok(authored.apply(p));
    CHECK(step == authored);

    // Self-inverting: the inverse needs no lookup and restores the state.
    tg::oracle_store undone = step;
    must_ok(undone.apply(invert(n)));
    CHECK(undone == base);
  }
}

TEST_CASE("FAB-INV-PAT-04: an author's expectation is checked, never replaced") {
  const record_id id = tg::test_id(id_class::entity, "normalize/e");
  tg::oracle_store base;
  base.put(id, record_state{.type = type_id_of(tt::entity), .body = must(tg::sample_entity("e"))});
  const path name{path_segment::field(1)};

  patch right;
  right.ops.push_back(
      op_set(id, name, must(value::string("f")), expectation::of(must(value::string("e")))));
  CHECK(must(normalize(right, base)) == right);

  patch wrong;
  wrong.ops.push_back(op_set(id, name, must(value::string("f")), expectation::absent()));
  const auto r = normalize(wrong, base);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == sl::errc::conflict);

  // Expectations follow the ops before them in the same patch.
  patch twice;
  twice.ops.push_back(op_set(id, name, must(value::string("f"))));
  twice.ops.push_back(op_set(id, name, must(value::string("g"))));
  twice.ops.push_back(op_unset(id, name));
  const patch n = must(normalize(twice, base));
  CHECK(n.ops[0].expect == expectation::of(must(value::string("e"))));
  CHECK(n.ops[1].expect == expectation::of(must(value::string("f"))));
  CHECK(n.ops[2].expect == expectation::of(must(value::string("g"))));
  // A set of an absent field expects absence; its inverse is an unset.
  patch add;
  add.ops.push_back(op_set(id, path{path_segment::field(2)}, value::uint(2)));
  const patch na = must(normalize(add, base));
  CHECK(na.ops[0].expect.is_absent());
  CHECK(invert(na).ops[0].kind == op_kind::unset);
}
