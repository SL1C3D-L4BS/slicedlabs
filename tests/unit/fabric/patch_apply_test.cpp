// FAB-INV-PAT-01 (SL-ARCH-002 ARCH2-R03): ops apply deterministically and check expect;
// insert_elem on a list is rejected.

#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"

#include "../../../engine/fabric/testing/model_store.hpp"
#include "../../../engine/fabric/testing/patch_gen.hpp"
#include "../../../engine/fabric/testing/test_schemas.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
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

const record_id hero = tg::test_id(id_class::entity, "apply/hero");

/// One entity record (test_schemas sample_entity) plus a lods list at field 6.
tg::oracle_store fixture() {
  value body = must(tg::sample_entity("hero"));
  const value lods =
      must(value::list({must(value::f32(1.0F)), must(value::f32(0.5F)), must(value::f32(0.25F))}));
  body = must(body.with({path_segment::field(6)}, lods));
  tg::oracle_store store;
  store.put(hero, record_state{.type = type_id_of(tt::entity), .body = body});
  return store;
}

id_bytes elem(const char *name) { return tg::test_id(id_class::element, name).bytes(); }

patch single(patch_op op) {
  patch p;
  p.ops.push_back(std::move(op));
  return p;
}

} // namespace

TEST_CASE("FAB-INV-PAT-01: 500 seeded patches apply to a staged_state exactly as to the oracle") {
  std::vector<std::size_t> kinds(10, 0);
  for (std::uint64_t seed = 0; seed < 500; ++seed) {
    CAPTURE(seed);
    tg::patch_gen gen(seed);
    const tg::oracle_store base = must(gen.state(5));
    const patch p = must(gen.next(base));
    for (const patch_op &op : p.ops) {
      ++kinds[static_cast<std::size_t>(op.kind)];
    }

    tg::oracle_store oracle = base;
    must_ok(oracle.apply(p));

    staged_state staged(base);
    must_ok(staged.apply(p));
    CHECK(tg::materialize(base, staged) == oracle);

    // Deterministic: a second overlay over the same base ends identical, change for change.
    staged_state again(base);
    must_ok(again.apply(p));
    CHECK(std::ranges::equal(again.changes(), staged.changes()));
    // The changes are sorted by record id.
    CHECK(std::ranges::is_sorted(staged.changes(), {}, &staged_change::id));
  }
  for (std::size_t k = 1; k <= 9; ++k) {
    CAPTURE(k);
    CHECK(kinds[k] > 0);
  }
}

TEST_CASE(
    "FAB-INV-PAT-01: a filled expectation that does not hold is conflict and writes nothing") {
  const tg::oracle_store base = fixture();
  const path name{path_segment::field(1)};
  const path comps{path_segment::field(4)};
  const id_bytes tag = tg::test_id(id_class::element, "hero/tag").bytes();

  const std::array<patch_op, 6> wrong = {
      op_set(hero, name, must(value::string("villain")), expectation::of(must(value::string("x")))),
      op_unset(hero, path{path_segment::field(5)}, expectation::absent()),
      [&] {
        patch_op op = op_delete(hero);
        op.captured = value::uint(1);
        return op;
      }(),
      [&] {
        patch_op op = op_move(hero, comps, tag, {0x10});
        op.expect_order_key = {0x7f};
        return op;
      }(),
      [&] {
        patch_op op = op_remove(hero, comps, tag);
        op.captured = value::uint(7);
        return op;
      }(),
      [&] {
        patch_op op = op_retype(hero, type_id_of(tt::scene), must(value::struct_({})));
        op.expect_type = type_id_of(tt::mesh);
        return op;
      }(),
  };
  for (const patch_op &op : wrong) {
    CAPTURE(to_string(op.kind));
    staged_state staged(base);
    // A valid first op, then the failing one: all or nothing.
    patch p = single(op_set(hero, path{path_segment::field(2)}, value::uint(1)));
    p.ops.push_back(op);
    const auto r = staged.apply(p);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == sl::errc::conflict);
    CHECK(staged.changes().empty());
    CHECK(must(staged.read(hero)) == must(base.read(hero)));
  }

  // The same ops with correct expectations apply.
  staged_state staged(base);
  must_ok(staged.apply(op_set(hero, name, must(value::string("villain")),
                              expectation::of(must(value::string("hero"))))));
  must_ok(staged.apply(op_unset(hero, path{path_segment::field(2)}, expectation::absent())));
}

TEST_CASE("FAB-INV-PAT-01: insert_elem on a list is rejected; no op enters a list") {
  const tg::oracle_store base = fixture();
  const path lods{path_segment::field(6)};
  staged_state staged(base);

  const auto insert = staged.apply(op_insert(hero, lods, elem("x"), {0x40}, value::uint(1)));
  REQUIRE_FALSE(insert.has_value());
  CHECK(insert.error().code == sl::errc::type_mismatch);

  const auto move = staged.apply(op_move(hero, lods, elem("x"), {0x40}));
  REQUIRE_FALSE(move.has_value());
  CHECK(move.error().code == sl::errc::type_mismatch);

  const auto remove = staged.apply(op_remove(hero, lods, elem("x")));
  REQUIRE_FALSE(remove.has_value());
  CHECK(remove.error().code == sl::errc::type_mismatch);

  const auto into = staged.apply(
      op_set(hero, path{path_segment::field(6), path_segment::field(1)}, must(value::f32(2.0F))));
  REQUIRE_FALSE(into.has_value());
  CHECK(into.error().code == sl::errc::type_mismatch);
  CHECK(staged.changes().empty());

  // The list is replaced whole.
  must_ok(staged.apply(op_set(hero, lods, must(value::list({})))));
  CHECK(must(must_have(must(staged.read(hero))).body.get(lods)) == must(value::list({})));
}

TEST_CASE("FAB-INV-PAT-01: structural refusals name the failing op") {
  const tg::oracle_store base = fixture();
  const path comps{path_segment::field(4)};
  const id_bytes tag = tg::test_id(id_class::element, "hero/tag").bytes();
  const record_id ghost = tg::test_id(id_class::entity, "apply/ghost");
  struct refusal {
    patch_op op;
    sl::errc code = sl::errc::ok;
  };
  const std::array<refusal, 9> cases = {
      refusal{.op = op_create(hero, type_id_of(tt::entity), must(value::struct_({}))),
              .code = sl::errc::already_exists},
      refusal{.op = op_set(ghost, path{path_segment::field(1)}, value::uint(1)),
              .code = sl::errc::not_found},
      refusal{.op = op_delete(ghost), .code = sl::errc::not_found},
      refusal{.op = op_insert(hero, comps, tag, {0x20}, value::uint(1)),
              .code = sl::errc::already_exists},
      refusal{.op = op_insert(hero, comps, elem("new"), {0x20, 0x00}, value::uint(1)),
              .code = sl::errc::invalid_argument},
      refusal{.op = op_unset(hero, path{path_segment::field(4), path_segment::element(tag)}),
              .code = sl::errc::invalid_argument},
      refusal{
          .op = op_set(hero, path{path_segment::field(9), path_segment::field(1)}, value::uint(1)),
          .code = sl::errc::not_found},
      refusal{.op = op_move(hero, comps, elem("absent"), {0x20}), .code = sl::errc::not_found},
      refusal{.op = op_create(ghost, type_id{}, must(value::struct_({}))),
              .code = sl::errc::invalid_argument},
  };
  for (const refusal &c : cases) {
    CAPTURE(to_string(c.op.kind));
    staged_state staged(base);
    const auto r = staged.apply(single(c.op));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == c.code);
    CHECK(r.error().message.starts_with("op 0: "));
    CHECK(staged.changes().empty());
  }

  // annotate changes nothing and needs no record.
  staged_state staged(base);
  must_ok(staged.apply(op_annotate(must(value::string("checkpoint")))));
  CHECK(staged.changes().empty());
}

TEST_CASE("FAB-INV-PAT-01: every op kind applies with its documented effect") {
  const tg::oracle_store base = fixture();
  const path comps{path_segment::field(4)};
  const id_bytes tag = tg::test_id(id_class::element, "hero/tag").bytes();
  const record_id born = tg::test_id(id_class::entity, "apply/born");
  staged_state s(base);

  must_ok(s.apply(op_create(born, type_id_of(tt::entity), must(value::struct_({})))));
  must_ok(s.apply(op_set(born, path{path_segment::field(1)}, must(value::string("b")))));
  must_ok(s.apply(op_insert(hero, comps, elem("new"), {0x60}, value::uint(3))));
  must_ok(s.apply(op_move(hero, comps, tag, {0x20})));
  must_ok(
      s.apply(op_remove(hero, comps, tg::test_id(id_class::element, "hero/transform").bytes())));
  must_ok(s.apply(op_unset(hero, path{path_segment::field(5)})));
  must_ok(s.apply(op_retype(born, type_id_of(tt::scene), must(value::struct_({})))));

  const record_state h = must_have(must(s.read(hero)));
  const value oset = must(h.body.get(comps));
  REQUIRE(oset.elements().size() == 2);
  CHECK(oset.elements()[0].element == tag); // moved to 0x20, first
  CHECK(oset.elements()[1].order_key == std::vector<std::uint8_t>{0x60});
  CHECK_FALSE(h.body.has_field(5));
  CHECK(must_have(must(s.read(born))).type == type_id_of(tt::scene));

  must_ok(s.apply(op_delete(born)));
  CHECK_FALSE(must(s.read(born)).has_value());
  CHECK(s.changes().size() == 2); // hero and born (created, then deleted: still listed)
}
