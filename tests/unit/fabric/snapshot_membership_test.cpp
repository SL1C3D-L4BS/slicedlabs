// FAB-INV-REV-06 (SL-ARCH-001 ARCH1-R51): the membership map equals the projection of record
// document fields. Seeded sequences of revisions write records into six documents (and into no
// document), move them between documents, retire them and write retired ids again; after every
// revision documents(), members() and for_each_in_document() must equal the projection of a
// model map and of the snapshot's own records.

#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/value.hpp"

#include "../../../engine/fabric/testing/tree_fixture.hpp"
#include "../../../engine/fabric/testing/value_gen.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

value body(std::uint64_t n) { return must(value::struct_({field{.no = 1, .v = value::uint(n)}})); }

using projection = std::map<record_id, std::vector<record_id>>; // doc -> ascending members

projection project(const std::map<record_id, record_id> &live) {
  projection out;
  for (const auto &[id, doc] : live) {
    if (!doc.is_null()) {
      out[doc].push_back(id); // std::map iteration: ascending id
    }
  }
  return out;
}

void check_membership(const snapshot &s, const std::map<record_id, record_id> &live,
                      const std::vector<record_id> &docs) {
  const projection model = project(live);
  std::vector<record_id> model_docs;
  for (const auto &[doc, members] : model) {
    model_docs.push_back(doc);
  }
  CHECK(must(s.documents()) == model_docs);

  projection own; // the projection of the snapshot's own records
  std::size_t live_count = 0;
  REQUIRE(s.for_each_entry([&](const snapshot_entry &e) -> sl::result<void> {
             ++live_count;
             if (!e.document.is_null()) {
               own[e.document].push_back(e.id);
             }
             return {};
           }).has_value());
  CHECK(live_count == live.size());
  CHECK(own == model);

  for (const record_id &doc : docs) {
    const auto it = model.find(doc);
    const std::vector<record_id> expect = it == model.end() ? std::vector<record_id>{} : it->second;
    CHECK(must(s.members(doc)) == expect);
    std::vector<record_id> walked;
    REQUIRE(s.for_each_in_document(doc, [&](const snapshot_entry &e) -> sl::result<void> {
               CHECK(e.document == doc);
               walked.push_back(e.id);
               return {};
             }).has_value());
    CHECK(walked == expect);
  }
}

} // namespace

TEST_CASE("FAB-INV-REV-06: membership map equals the projection of record document fields") {
  constexpr std::size_t seeds = 4;
  constexpr std::size_t steps = 20;
  schema_registry reg;
  REQUIRE(testing::add_tree_record_types(reg).has_value());
  std::vector<record_id> docs;
  docs.reserve(6);
  for (std::uint32_t d = 0; d < 6; ++d) {
    docs.push_back(well_known(id_class::entity, 100 + d));
  }
  const std::vector<record_id> pool = testing::tree_ids(120, id_class::entity, "member");

  for (std::size_t seed = 0; seed < seeds; ++seed) {
    memory_backend store;
    testing::rng r(0x3E3B'0000ULL + seed);
    std::map<record_id, record_id> live; // id -> document (null for none)
    snapshot s = must(snapshot::empty(store));
    std::uint32_t tx = 1;
    for (std::size_t step = 0; step < steps; ++step) {
      revision_builder b(s, reg);
      const std::size_t changes = 1 + r.below(step == 0 ? 60 : 12);
      std::set<record_id> touched;
      for (std::size_t k = 0; k < changes; ++k) {
        const record_id &id = pool[r.below(pool.size())];
        if (!touched.insert(id).second) {
          continue;
        }
        const auto it = live.find(id);
        if (it != live.end() && r.chance(25)) {
          REQUIRE(b.retire(id).has_value());
          live.erase(it);
          continue;
        }
        // A new record, a move to another document, or a body edit in place.
        record_id doc = it != live.end() ? it->second : record_id{};
        if (it == live.end() || r.chance(40)) {
          doc = r.chance(15) ? record_id{} : docs[r.below(docs.size())];
        }
        REQUIRE(b.put(id, record_write{.type = type_id_of("gen.rec0"),
                                       .document = doc,
                                       .body = body(r.below(1000)),
                                       .version = std::nullopt})
                    .has_value());
        live[id] = doc;
      }
      const built_revision built =
          must(b.build(testing::tree_inputs(std::format("step {}", step), tx++)));
      REQUIRE(testing::commit_built(store, built).has_value());
      s = must(snapshot::open(store, built.revision));
      check_membership(s, live, docs);
    }
  }
}
