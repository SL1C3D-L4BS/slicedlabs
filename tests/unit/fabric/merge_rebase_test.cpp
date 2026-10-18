// FAB-INV-MRG-02 (SL-ARCH-002 ARCH2-R08): rebase_if_disjoint equals sequential application;
// overlapping footprints refuse.

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

} // namespace

TEST_CASE("FAB-INV-MRG-02: 500 seeded concurrent pairs: rebase iff disjoint, and it is exact") {
  std::size_t rebased = 0;
  std::size_t refused = 0;
  for (std::uint64_t seed = 0; seed < 500; ++seed) {
    CAPTURE(seed);
    tg::patch_gen gen(seed + 13000);
    const tg::oracle_store base = must(gen.state(seed % 2 == 0 ? 1 : 5));
    // `onto` committed first; `p` was authored on the same base meanwhile.
    const patch onto = must(normalize(must(gen.next(base)), base));
    const patch p = must(normalize(must(gen.next(base)), base));
    const bool disjoint = commutes(footprint_of(p), footprint_of(onto));
    const auto r = rebase_if_disjoint(p, onto);
    REQUIRE(r.has_value() == disjoint);
    if (!r) {
      ++refused;
      CHECK(r.error().code == sl::errc::conflict);
      continue;
    }
    ++rebased;
    CHECK(r->ops == p.ops);
    tg::oracle_store sequential = base;
    must_ok(sequential.apply(onto));
    must_ok(sequential.apply(*r)); // its expectations still hold after `onto`
    tg::oracle_store other_order = base;
    must_ok(other_order.apply(p));
    must_ok(other_order.apply(onto));
    CHECK(sequential.root_digest() == other_order.root_digest());

    // The engine overlay agrees with the oracle.
    staged_state staged(base);
    must_ok(staged.apply(onto));
    must_ok(staged.apply(*r));
    CHECK(tg::materialize(base, staged) == sequential);
  }
  CHECK(rebased > 50);
  CHECK(refused > 50);
}

TEST_CASE("FAB-INV-MRG-02: the refusal names the overlapping entries") {
  const record_id id = tg::test_id(id_class::entity, "rebase/e");
  tg::oracle_store base;
  base.put(id, record_state{.type = type_id_of(tt::entity), .body = must(tg::sample_entity("e"))});
  patch onto;
  onto.ops.push_back(op_unset(id, path{path_segment::field(5)}));
  patch p;
  p.ops.push_back(op_set(
      id, path{path_segment::field(5), must(path_segment::key(must(value::string("owner"))))},
      must(value::string("me"))));
  const auto r = rebase_if_disjoint(must(normalize(p, base)), must(normalize(onto, base)));
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == sl::errc::conflict);
  CHECK(r.error().message.find("/5") != std::string::npos);

  // An edit elsewhere in the record rebases.
  patch q;
  q.ops.push_back(op_set(id, path{path_segment::field(1)}, must(value::string("renamed"))));
  CHECK(rebase_if_disjoint(q, onto).has_value());
}
