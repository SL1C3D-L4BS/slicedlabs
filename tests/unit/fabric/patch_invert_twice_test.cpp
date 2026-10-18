// FAB-INV-UNDO-02 (SL-ARCH-002 ARCH2-R20): invert(invert(p)) equals p.

#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/value.hpp"

#include "../../../engine/fabric/testing/model_store.hpp"
#include "../../../engine/fabric/testing/patch_gen.hpp"
#include "../../../engine/fabric/testing/test_schemas.hpp"

#include <doctest/doctest.h>

#include <array>
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

} // namespace

TEST_CASE("FAB-INV-UNDO-02: invert(invert(p)) == p for 500 seeded normalised patches") {
  for (std::uint64_t seed = 0; seed < 500; ++seed) {
    CAPTURE(seed);
    tg::patch_gen gen(seed + 2000);
    const tg::oracle_store base = must(gen.state(5));
    const patch n = must(normalize(must(gen.next(base)), base));
    const patch once = invert(n);
    CHECK(is_normalized(once));
    CHECK(invert(once) == n);
    // The inverse keeps version, base and meta (the undo manager rewrites base and undo_of).
    CHECK(once.version == n.version);
    CHECK(once.base == n.base);
    CHECK(once.meta == n.meta);
    CHECK(once.ops.size() == n.ops.size());
  }
}

TEST_CASE("FAB-INV-UNDO-02: each op kind inverts to its SL-ARCH-002 inverse and back") {
  const record_id id = tg::test_id(id_class::entity, "twice/e");
  const id_bytes el = tg::test_id(id_class::element, "twice/el").bytes();
  const path name{path_segment::field(1)};
  const path comps{path_segment::field(4)};
  const value a = must(value::string("a"));
  const value b = must(value::string("b"));
  const type_id t1 = type_id_of(tt::entity);
  const type_id t2 = type_id_of(tt::scene);

  patch_op del = op_delete(id);
  del.captured = a;
  del.type = t1;
  patch_op rem = op_remove(id, comps, el);
  rem.captured = a;
  rem.expect_order_key = {0x40};
  patch_op mov = op_move(id, comps, el, {0x80});
  mov.expect_order_key = {0x40};
  patch_op ret = op_retype(id, t2, b);
  ret.captured = a;
  ret.expect_type = t1;
  patch_op irreversible = op_set(id, name, b, expectation::of(a));
  irreversible.rev = reversibility::irreversible;

  struct row {
    patch_op op;
    op_kind inverse = op_kind::annotate;
  };
  const std::array<row, 11> rows = {
      row{.op = op_create(id, t1, a), .inverse = op_kind::delete_record},
      row{.op = del, .inverse = op_kind::create_record},
      row{.op = op_set(id, name, b, expectation::of(a)), .inverse = op_kind::set},
      row{.op = op_set(id, name, b, expectation::absent()), .inverse = op_kind::unset},
      row{.op = op_unset(id, name, expectation::of(a)), .inverse = op_kind::set},
      row{.op = op_unset(id, name, expectation::absent()), .inverse = op_kind::unset},
      row{.op = op_insert(id, comps, el, {0x40}, a), .inverse = op_kind::remove_elem},
      row{.op = rem, .inverse = op_kind::insert_elem},
      row{.op = mov, .inverse = op_kind::move_elem},
      row{.op = ret, .inverse = op_kind::retype_record},
      row{.op = irreversible, .inverse = op_kind::set},
  };
  for (const row &r : rows) {
    CAPTURE(to_string(r.op.kind));
    patch p;
    p.ops.push_back(r.op);
    const patch inv = invert(p);
    REQUIRE(inv.ops.size() == 1);
    CHECK(inv.ops[0].kind == r.inverse);
    CHECK(inv.ops[0].rev == r.op.rev);
    CHECK(invert(inv) == p);
  }
  patch note;
  note.ops.push_back(op_annotate(a));
  CHECK(invert(note) == note);
}
