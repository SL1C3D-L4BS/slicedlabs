// FAB-INV-REV-07 (SL-ARCH-001 ARCH1-R53): snapshot reads equal the model oracle after seeded
// random operation sequences. Each step draws a patch over every op kind (testing/patch_gen.hpp),
// applies it to the oracle (testing/model_store.hpp oracle_store) and to a staged_state over the
// current snapshot, turns the staged changes into a revision and opens it; every record the
// oracle holds, and every id it held before, must read the same from the snapshot, for_each must
// enumerate exactly the oracle in id order, and the final snapshot must read the same from four
// threads at once.

#include "sl/fabric/deps.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/tree.hpp"

#include "../../../engine/fabric/testing/model_store.hpp"
#include "../../../engine/fabric/testing/patch_gen.hpp"
#include "../../../engine/fabric/testing/tree_fixture.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

// Documents follow the id: three documents and records that belong to none.
record_id document_of(const record_id &id) {
  const unsigned pick = key_of(id)[0] % 4U;
  return pick == 3 ? record_id{} : well_known(id_class::entity, 200 + pick);
}

void check_equal(const snapshot &s, const testing::oracle_store &oracle,
                 const std::set<record_id> &ever) {
  for (const record_id &id : ever) {
    const auto got = must(s.read(id));
    const auto want = must(oracle.read(id));
    CHECK_MESSAGE(got == want, id.to_string());
  }
  std::vector<record_id> order;
  REQUIRE(s.for_each([&](const record_view &v) -> sl::result<void> {
             order.push_back(v.id);
             const auto it = oracle.records().find(v.id);
             REQUIRE(it != oracle.records().end());
             CHECK(v.type == it->second.type);
             CHECK(*v.body == it->second.body);
             CHECK(v.provenance_inputs.empty());
             return {};
           }).has_value());
  std::vector<record_id> want;
  for (const auto &[id, rec] : oracle.records()) {
    want.push_back(id);
  }
  CHECK(order == want);
}

} // namespace

TEST_CASE(
    "FAB-INV-REV-07: snapshot reads equal the model oracle after seeded random op sequences") {
  constexpr std::size_t seeds = 4;
  constexpr std::size_t steps = 25;
  schema_registry reg;
  REQUIRE(testing::add_tree_record_types(reg).has_value());

  for (std::size_t seed = 0; seed < seeds; ++seed) {
    testing::patch_gen gen(0xD1FF'5EEDULL + seed);
    testing::oracle_store oracle = must(gen.state(40));
    memory_backend store;
    std::set<record_id> ever;

    revision_builder genesis(must(snapshot::empty(store)), reg);
    for (const auto &[id, rec] : oracle.records()) {
      ever.insert(id);
      REQUIRE(genesis
                  .put(id, record_write{.type = rec.type,
                                        .document = document_of(id),
                                        .body = rec.body,
                                        .version = std::nullopt})
                  .has_value());
    }
    std::uint32_t tx = 1;
    const built_revision g = must(genesis.build(testing::tree_inputs("genesis", tx++)));
    REQUIRE(testing::commit_built(store, g).has_value());
    snapshot s = must(snapshot::open(store, g.revision));
    check_equal(s, oracle, ever);

    for (std::size_t step = 0; step < steps; ++step) {
      const patch p = must(gen.next(oracle));
      staged_state staged(s);
      REQUIRE(staged.apply(p).has_value());
      REQUIRE(oracle.apply(p).has_value());
      revision_builder b(s, reg);
      REQUIRE(b.stage(staged.changes(),
                      [](const record_id &id, const record_state &) -> sl::result<record_id> {
                        return document_of(id);
                      })
                  .has_value());
      for (const staged_change &c : staged.changes()) {
        ever.insert(c.id);
      }
      const built_revision built =
          must(b.build(testing::tree_inputs(std::format("step {}", step), tx++)));
      REQUIRE(testing::commit_built(store, built).has_value());
      const snapshot next = must(snapshot::open(store, built.revision));
      // The records diff names exactly the ids whose read changed.
      const std::vector<record_delta> deltas = must(s.diff(next));
      for (const record_delta &d : deltas) {
        CHECK(must(s.read(d.id)) != must(next.read(d.id)));
      }
      s = next;
      check_equal(s, oracle, ever);
    }

    // Concurrent readers of one immutable snapshot (ARCH1-R53).
    std::atomic<std::size_t> mismatches{0};
    std::vector<std::thread> readers;
    readers.reserve(4);
    for (int t = 0; t < 4; ++t) {
      readers.emplace_back([&s, &oracle, &mismatches] {
        for (const auto &[id, rec] : oracle.records()) {
          const auto got = s.read(id);
          if (!got || !got->has_value() || **got != rec) {
            mismatches.fetch_add(1);
          }
        }
      });
    }
    for (std::thread &t : readers) {
      t.join();
    }
    CHECK(mismatches.load() == 0);
  }
}
