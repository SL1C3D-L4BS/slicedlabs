// FAB-INV-REV-04 (SL-ARCH-001 ARCH1-R50): the revision object id covers parents, root, schema_set,
// tx and generation, and no time field exists. Each field is changed in turn and must change the
// oid; the revision, tx, root and record object layouts are checked field by field for any time
// field; the builder's revisions chain generations, carry the codec's schema_set digest, are
// reproducible from the same inputs, move no ref, and close under store verify.

#include "sl/fabric/codec.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/gen/fabric.history.gen.hpp"
#include "sl/fabric/gen/system.gen.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/value.hpp"

#include "../../../engine/fabric/testing/tree_fixture.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;
namespace history = sl::fabric::gen::fabric_history;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

bool timeish(std::string_view name) {
  std::string lower;
  for (const char c : name) {
    lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  const std::vector<std::string_view> words = {"time", "date", "clock", "stamp",
                                               "when", "_at",  "wall"};
  return std::ranges::any_of(words, [&lower](std::string_view w) { return lower.contains(w); });
}

std::vector<std::uint32_t> field_numbers(const object &o) {
  const value v = must(decode(o.payload));
  std::vector<std::uint32_t> out;
  for (const field &f : v.fields()) {
    out.push_back(f.no);
  }
  return out;
}

value body(std::uint64_t n) { return must(value::struct_({field{.no = 1, .v = value::uint(n)}})); }

} // namespace

TEST_CASE("FAB-INV-REV-04: revision oid covers parents, root, schema_set, tx and generation") {
  history::revision base;
  base.parents = {testing::tree_target(1), testing::tree_target(2)};
  base.root = testing::tree_target(3);
  base.schema_set = testing::tree_target(4);
  base.tx = testing::tree_target(5);
  base.generation = 7;
  const digest id = must(encode_revision(base)).id;
  CHECK(must(encode_revision(base)).id == id); // pure: no clock, no counter

  std::vector<history::revision> variants(7, base);
  variants[0].parents.pop_back();
  variants[1].parents = {testing::tree_target(2), testing::tree_target(1)}; // order counts
  variants[2].root = testing::tree_target(13);
  variants[3].schema_set = testing::tree_target(14);
  variants[4].tx = testing::tree_target(15);
  variants[5].generation = 8;
  variants[6].parents.clear();
  std::set<digest> seen{id};
  for (const history::revision &v : variants) {
    const object o = must(encode_revision(v));
    CHECK(o.header.kind == object_kind::revision);
    CHECK(seen.insert(o.id).second);
    CHECK(must(decode_revision(o)) == v);
  }
}

TEST_CASE("FAB-INV-REV-04: no time field exists in revision, tx, root or record objects") {
  const schema_registry sys = must(gen::system_registry());
  for (const std::string_view name : {"fabric.history.revision", "fabric.history.tx_record",
                                      "fabric.history.origin", "fabric.identity.tombstone"}) {
    const type_desc *t = sys.find(name);
    REQUIRE_MESSAGE(t != nullptr, name);
    for (const field_desc &f : t->fields) {
      CHECK_MESSAGE(!timeish(f.name), name << "." << f.name);
    }
  }
  const type_desc *rev = sys.find("fabric.history.revision");
  REQUIRE(rev != nullptr);
  REQUIRE(rev->fields.size() == 5);
  CHECK(rev->fields[0].name == "parents");
  CHECK(rev->fields[1].name == "root");
  CHECK(rev->fields[2].name == "schema_set");
  CHECK(rev->fields[3].name == "tx");
  CHECK(rev->fields[4].name == "generation");

  const record_object rec{.id = well_known(id_class::entity, 9),
                          .type = type_id_of("gen.rec0"),
                          .document = well_known(id_class::entity, 10),
                          .version = type_version{.major = 1, .minor = 2},
                          .body = body(3)};
  const object ro = must(encode_record_object(rec));
  CHECK(ro.header.kind == object_kind::record);
  CHECK(ro.id == record_object_id(ro.payload));
  CHECK(field_numbers(ro) == std::vector<std::uint32_t>{1, 2, 3, 4, 5, 6});
  CHECK(must(decode_record_object(ro)) == rec);
  record_object loose = rec;
  loose.document = record_id{};
  const object lo = must(encode_record_object(loose));
  CHECK(field_numbers(lo) == std::vector<std::uint32_t>{1, 2, 4, 5, 6});
  CHECK(must(decode_record_object(lo)) == loose);
  CHECK(decode_record_object(make_object(object_kind::blob, ro.payload)).error().code ==
        sl::errc::object_corrupt);

  const object root = must(encode_root_object(
      root_object{.records = testing::tree_target(1), .membership = testing::tree_target(2)}));
  CHECK(field_numbers(root) == std::vector<std::uint32_t>{1, 2});
  const object tx = must(encode_tx_record(testing::tree_tx("label")));
  for (const std::uint32_t no : field_numbers(tx)) {
    CHECK(no <= 7);
  }
}

TEST_CASE("FAB-INV-REV-04: builder revisions chain generations and carry the schema_set digest") {
  memory_backend store;
  schema_registry reg;
  REQUIRE(testing::add_tree_record_types(reg).has_value());
  const digest schema_set = must(schema_set_digest(reg));
  const std::vector<record_id> ids = testing::tree_ids(40, id_class::entity, "rev");
  const record_id doc = ids[0];

  revision_builder genesis(must(snapshot::empty(store)), reg);
  for (std::size_t i = 0; i < ids.size(); ++i) {
    REQUIRE(genesis
                .put(ids[i], record_write{.type = type_id_of("gen.rec1"),
                                          .document = doc,
                                          .body = body(i),
                                          .version = std::nullopt})
                .has_value());
  }
  const built_revision g = must(genesis.build(testing::tree_inputs("genesis", 1)));
  CHECK(g.batch.refs.empty());
  CHECK(g.batch.intents.empty());
  CHECK(g.revision_value.parents.empty());
  CHECK(g.revision_value.generation == 0);
  CHECK(g.revision_value.schema_set == schema_set);
  CHECK(g.revision_value.root == g.root);
  CHECK(g.revision_value.tx == g.tx);
  CHECK(g.tx == must(encode_tx_record(testing::tree_tx("genesis"))).id);
  CHECK(g.batch.objects.back().id == g.revision);
  // Same base and inputs: the same revision, byte for byte (no clock enters it).
  CHECK(must(genesis.build(testing::tree_inputs("genesis", 1))).revision == g.revision);
  REQUIRE(testing::commit_built(store, g).has_value());
  CHECK(must(store.list_refs()).empty());

  const snapshot s0 = must(snapshot::open(store, g.revision));
  CHECK(s0.root_digest() == g.root);
  CHECK(s0.revision_digest() == g.revision);
  CHECK(s0.generation() == 0);
  CHECK(must(s0.get(ids[5])).version == type_version{.major = 1, .minor = 1});

  revision_builder next(s0, reg);
  REQUIRE(next.put(ids[3], record_write{.type = type_id_of("gen.rec2"),
                                        .document = doc,
                                        .body = body(99),
                                        .version = type_version{.major = 1, .minor = 0}})
              .has_value());
  REQUIRE(next.retire(ids[4]).has_value());
  const built_revision n1 = must(next.build(testing::tree_inputs("edit", 2)));
  CHECK(n1.revision_value.parents == history::digest_list{g.revision});
  CHECK(n1.revision_value.generation == 1);
  REQUIRE(testing::commit_built(store, n1).has_value());
  const snapshot s1 = must(snapshot::open(store, n1.revision));
  CHECK(must(s1.get(ids[3])).version == type_version{.major = 1, .minor = 0});
  CHECK(s1.get(ids[4]).error().code == sl::errc::not_found);
  const auto tomb_entry = must(s1.entry(ids[4]));
  REQUIRE(tomb_entry.has_value());
  const tree_entry tomb = tomb_entry.value_or(tree_entry{});
  CHECK(tomb.kind == entry_kind::tombstone);
  const auto t = must(decode_tombstone(must(store.get(tomb.target))));
  CHECK(t.last_record == must(s0.get(ids[4])).object);
  CHECK(t.retired_by == well_known(id_class::tx, 2));

  // A merge: a second child of s0, then a revision over s1 naming it as an extra parent.
  revision_builder side(s0, reg);
  REQUIRE(side.put(ids[7], record_write{.type = type_id_of("gen.rec0"),
                                        .document = doc,
                                        .body = body(70),
                                        .version = std::nullopt})
              .has_value());
  const built_revision b1 = must(side.build(testing::tree_inputs("side", 3)));
  REQUIRE(testing::commit_built(store, b1).has_value());
  revision_builder merge(s1, reg);
  revision_inputs in = testing::tree_inputs("merge", 4);
  in.extra_parents = {b1.revision};
  const built_revision m = must(merge.build(in));
  CHECK(m.revision_value.parents == history::digest_list{n1.revision, b1.revision});
  CHECK(m.revision_value.generation == 2);
  in.extra_parents = {n1.revision};
  CHECK(merge.build(in).error().code == sl::errc::invalid_argument);
  REQUIRE(testing::commit_built(store, m).has_value());

  // Errors: retiring an id that is not live; a type with no registered version.
  revision_builder bad(s1, reg);
  REQUIRE(bad.retire(ids[4]).has_value());
  CHECK(bad.build(testing::tree_inputs("bad", 5)).error().code == sl::errc::not_found);
  revision_builder unknown(s1, reg);
  REQUIRE(unknown
              .put(ids[8], record_write{.type = type_id_of("gen.nope"),
                                        .document = doc,
                                        .body = body(1),
                                        .version = std::nullopt})
              .has_value());
  CHECK(unknown.build(testing::tree_inputs("unknown", 6)).error().code == sl::errc::schema_unknown);

  // Closure: with the tx records' patch stand-in stored, every revision's closure is present.
  commit_batch patch;
  patch.objects = {make_object(object_kind::patch, std::vector<std::uint8_t>{1, 2, 3})};
  const digest patch_id = patch.objects.front().id;
  REQUIRE(testing::backdoor::commit(store, std::move(patch)).has_value());
  history::tx_record tx = testing::tree_tx("x");
  tx.patch = patch_id;
  revision_builder head(must(snapshot::open(store, m.revision)), reg);
  REQUIRE(head.put(ids[9], record_write{.type = type_id_of("gen.rec0"),
                                        .document = record_id{},
                                        .body = body(9),
                                        .version = std::nullopt})
              .has_value());
  const built_revision h = must(head.build(
      revision_inputs{.tx = tx, .tx_id = well_known(id_class::tx, 7), .extra_parents = {}}));
  commit_batch with_ref = h.batch;
  with_ref.refs.push_back(
      ref_update{.name = "main", .expected = std::nullopt, .desired = h.revision});
  REQUIRE(testing::backdoor::commit(store, std::move(with_ref)).has_value());
  const revision_links links;
  const verify_report report =
      must(store.verify(verify_scope{.hashes = true, .reachability = true, .links = &links}));
  // The earlier revisions' tx records name the shared stand-in patch digest, never stored.
  for (const verify_finding &f : report.findings) {
    CHECK(f.code == sl::errc::object_missing);
    CHECK(f.id == testing::tree_target(0xFFFF'FFFFULL));
  }
  CHECK(report.objects_reached > 40);
}
