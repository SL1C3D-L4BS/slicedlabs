// FAB-INV-UNDO-01 (SL-ARCH-002 ARCH2-R20): apply(invert(p)) restores the root digest for 500
// seeded patches over every op kind.

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

} // namespace

TEST_CASE("FAB-INV-UNDO-01: 500 seeded patches over every op kind: invert restores the root") {
  std::vector<std::size_t> kinds(10, 0);
  std::size_t changed_root = 0;
  for (std::uint64_t seed = 0; seed < 500; ++seed) {
    CAPTURE(seed);
    tg::patch_gen gen(seed + 1000);
    const tg::oracle_store base = must(gen.state(5));
    const digest root = base.root_digest();
    const patch n = must(normalize(must(gen.next(base)), base));
    for (const patch_op &op : n.ops) {
      ++kinds[static_cast<std::size_t>(op.kind)];
    }

    tg::oracle_store after = base;
    must_ok(after.apply(n));
    changed_root += after.root_digest() != root ? 1U : 0U;

    tg::oracle_store undone = after;
    must_ok(undone.apply(invert(n)));
    CHECK(undone.root_digest() == root);
    CHECK(undone == base);

    // The same through the engine's overlay: patch, then inverse, ends at the base records.
    staged_state staged(base);
    must_ok(staged.apply(n));
    must_ok(staged.apply(invert(n)));
    CHECK(tg::materialize(base, staged).root_digest() == root);
  }
  for (std::size_t k = 1; k <= 9; ++k) {
    CAPTURE(k);
    CHECK(kinds[k] > 0);
  }
  CHECK(changed_root > 400); // the property is not vacuous
}

TEST_CASE("FAB-INV-UNDO-01: delete then invert restores the same id, type and body") {
  const record_id id = tg::test_id(id_class::entity, "invert/e");
  tg::oracle_store base;
  base.put(id, record_state{.type = type_id_of(tt::entity), .body = must(tg::sample_entity("e"))});
  patch del;
  del.ops.push_back(op_delete(id));
  const patch n = must(normalize(del, base));
  const patch inv = invert(n);
  REQUIRE(inv.ops.size() == 1);
  CHECK(inv.ops[0].kind == op_kind::create_record);
  CHECK(inv.ops[0].target == id);
  tg::oracle_store s = base;
  must_ok(s.apply(n));
  CHECK(s.size() == 0);
  must_ok(s.apply(inv));
  CHECK(s == base);
}
