// FAB-INV-MRG-01 (SL-ARCH-002 ARCH2-R45): commutes is symmetric, and when true both orders give
// the same root.

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

struct tally {
  std::size_t pairs = 0;
  std::size_t commuting = 0;
  std::size_t same_record = 0; // commuting pairs that touch one record
};

/// Two patches drawn independently on one base: symmetry always; when they commute, both
/// normalised orders apply (each one's expectations survive the other) and agree.
void check_pair(std::uint64_t seed, std::size_t records, tally &t) {
  CAPTURE(seed);
  tg::patch_gen gen(seed);
  const tg::oracle_store base = must(gen.state(records));
  const patch p = must(normalize(must(gen.next(base)), base));
  const patch q = must(normalize(must(gen.next(base)), base));
  const footprint fp = footprint_of(p);
  const footprint fq = footprint_of(q);
  ++t.pairs;
  REQUIRE(commutes(fp, fq) == commutes(fq, fp));
  CHECK(fp.overlaps(fq).size() == fq.overlaps(fp).size());
  if (!commutes(fp, fq)) {
    return;
  }
  ++t.commuting;
  for (const footprint_entry &e : fp.entries) {
    for (const footprint_entry &o : fq.entries) {
      t.same_record += e.record == o.record ? 1U : 0U;
    }
  }
  tg::oracle_store pq = base;
  REQUIRE(pq.apply(p).has_value());
  const auto q_after_p = pq.apply(q);
  REQUIRE_MESSAGE(q_after_p.has_value(), q_after_p.error().message);
  tg::oracle_store qp = base;
  REQUIRE(qp.apply(q).has_value());
  const auto p_after_q = qp.apply(p);
  REQUIRE_MESSAGE(p_after_q.has_value(), p_after_q.error().message);
  CHECK(pq.root_digest() == qp.root_digest());
  CHECK(pq == qp);
}

} // namespace

TEST_CASE("FAB-INV-MRG-01: 500 seeded pairs over several records") {
  tally t;
  for (std::uint64_t seed = 0; seed < 500; ++seed) {
    check_pair(seed + 11000, 6, t);
  }
  CHECK(t.pairs == 500);
  CHECK(t.commuting > 100);
}

TEST_CASE("FAB-INV-MRG-01: 500 seeded pairs inside one record (disjoint paths)") {
  tally t;
  for (std::uint64_t seed = 0; seed < 500; ++seed) {
    check_pair(seed + 12000, 1, t);
  }
  CHECK(t.commuting > 30);
  CHECK(t.same_record > 30); // commuting edits of one record happen
}

TEST_CASE("FAB-INV-MRG-01: hand cases") {
  const record_id id = tg::test_id(id_class::entity, "commutes/e");
  const id_bytes e1 = tg::test_id(id_class::element, "commutes/1").bytes();
  const id_bytes e2 = tg::test_id(id_class::element, "commutes/2").bytes();
  const path comps{path_segment::field(4)};
  const auto fp = [](const patch_op &op) { return footprint_of(op); };

  CHECK(commutes(fp(op_set(id, path{path_segment::field(1)}, value::uint(1))),
                 fp(op_set(id, path{path_segment::field(2)}, value::uint(2)))));
  CHECK_FALSE(commutes(
      fp(op_set(id, path{path_segment::field(5)}, value::uint(1))),
      fp(op_set(id, path{path_segment::field(5), path_segment::field(1)}, value::uint(2)))));
  CHECK(commutes(fp(op_insert(id, comps, e1, {0x40}, value::uint(1))),
                 fp(op_insert(id, comps, e2, {0x40}, value::uint(2)))));
  CHECK_FALSE(commutes(fp(op_insert(id, comps, e1, {0x40}, value::uint(1))),
                       fp(op_insert(id, comps, e1, {0x50}, value::uint(2)))));
  CHECK_FALSE(commutes(fp(op_delete(id)), fp(op_move(id, comps, e1, {0x40}))));
  CHECK(commutes(fp(op_annotate(value::uint(0))), fp(op_delete(id))));
  CHECK(commutes(fp(op_retype(id, type_id_of(tt::scene), value::uint(0))),
                 fp(op_delete(tg::test_id(id_class::entity, "commutes/other")))));
}
