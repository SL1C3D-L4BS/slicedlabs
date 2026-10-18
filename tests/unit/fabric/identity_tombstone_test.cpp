// FAB-INV-LC-02 (SL-ARCH-001 ARCH1-R05; ADR-0011): every id class that names a record gets a
// tombstone when the record is deleted: the records-trie leaf becomes a tombstone leaf whose
// tombstone object (fabric.identity.tombstone) keeps the id, the object id of the last record
// object written under it and the retiring tx id; the tombstone stays through later commits and a
// reopen of the store, and the classes that name no record (tx, event, element, operation) cannot
// be written or deleted at all. Seeded over the memory backend: random edit histories over records
// of every class, then a delete, always leave the last record object id in the tombstone.

#include "sl/fabric/clock.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/identity.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/tree.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/test_schemas.hpp"
#include "../../../engine/fabric/testing/tx_fixture.hpp"
#include "../../../engine/fabric/testing/undo_fixture.hpp"
#include "../../../engine/fabric/testing/value_gen.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;
namespace tg = sl::fabric::testing;
using sl::errc;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

void must_ok(const sl::result<void> &r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
}

// Every assigned class, and those that may name a record.
std::vector<id_class> all_classes() {
  std::vector<id_class> out;
  for (std::uint8_t n = 1; n <= 15; ++n) {
    out.push_back(static_cast<id_class>(n));
  }
  return out;
}

std::vector<id_class> record_classes() {
  std::vector<id_class> out;
  for (id_class c : all_classes()) {
    if (names_record(c)) {
      out.push_back(c);
    }
  }
  return out;
}

std::string type_name_of(id_class c) { return std::format("content.scene.Rec_{}", to_string(c)); }

type_id type_of(id_class c) { return type_id_of(type_name_of(c)); }

// A record type of class `c` in domain content.scene: struct {1 label string}.
type_desc record_type(id_class c) {
  type_desc t;
  t.name = type_name_of(c);
  t.id = type_id_of(t.name);
  t.kind = type_kind::struct_;
  t.domain = "content.scene";
  t.record_class = c;
  t.state_class = state_class::document;
  field_desc f;
  f.no = 1;
  f.name = "label";
  f.type = type_id_of("string");
  t.fields = {f};
  return t;
}

// scene_options with one record type per record class added to the test schemas.
workspace_options every_class_options(std::uint64_t seed = 1) {
  workspace_options o = must(tg::scene_options(std::make_shared<manual_clock>(1), seed));
  schema_registry reg;
  must_ok(tg::add_test_schemas(reg));
  for (id_class c : record_classes()) {
    must_ok(reg.add(record_type(c)));
  }
  must_ok(reg.freeze());
  domain_registry domains;
  must_ok(domains.claim({.name = "content.scene",
                         .owner = subsystem::content,
                         .state_classes = {state_class::document},
                         .types = {}}));
  must_ok(domains.claim({.name = "content.mesh",
                         .owner = subsystem::content,
                         .state_classes = {state_class::document},
                         .types = {}}));
  must_ok(domains.claim({.name = "asset.cooked",
                         .owner = subsystem::asset,
                         .state_classes = {state_class::derived},
                         .types = {}}));
  must_ok(domains.bind_types(reg));
  o.schemas = std::move(reg);
  o.domains = std::move(domains);
  return o;
}

value labelled(std::string_view label) {
  return must(value::struct_({field{.no = 1, .v = must(value::string(label))}}));
}

record_id id_of(id_class c, std::string_view name) {
  return derive(c, well_known(id_class::namespace_, 1), name);
}

} // namespace

TEST_CASE("FAB-INV-LC-02: every record class gets a tombstone keeping the last record oid") {
  auto store = std::make_shared<memory_backend>();
  auto ws = must(workspace::create_with(store, every_class_options()));
  transaction_manager &m = ws->manager();
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());

  std::vector<patch_op> creates;
  for (id_class c : record_classes()) {
    creates.push_back(op_create(id_of(c, "subject"), type_of(c), labelled("v1")));
  }
  must(tg::commit_ops(m, editor, creates));
  // A second version, so the tombstone must keep the last object, not the first.
  std::vector<patch_op> edits;
  for (id_class c : record_classes()) {
    edits.push_back(
        op_set(id_of(c, "subject"), {path_segment::field(1)}, must(value::string("v2"))));
  }
  must(tg::commit_ops(m, editor, edits));
  std::map<record_id, digest> last;
  const snapshot live = must(m.head());
  for (id_class c : record_classes()) {
    last.emplace(id_of(c, "subject"), must(live.get(id_of(c, "subject"))).object);
  }

  std::vector<patch_op> deletes;
  for (id_class c : record_classes()) {
    deletes.push_back(op_delete(id_of(c, "subject")));
  }
  const commit_receipt retired = must(tg::commit_ops(m, editor, deletes));
  const snapshot after = must(m.head());
  for (id_class c : record_classes()) {
    CAPTURE(to_string(c));
    const record_id id = id_of(c, "subject");
    const auto leaf = must(after.entry(id));
    REQUIRE(leaf.has_value());
    CHECK(leaf.value_or(tree_entry{}).kind == entry_kind::tombstone);
    // The tombstone object, read straight from the store, keeps id, last record and retirer.
    const tombstone t =
        must(decode_tombstone(must(store->get(leaf.value_or(tree_entry{}).target))));
    CHECK(t.id == id);
    CHECK(t.last_record == last.at(id));
    CHECK(t.retired_by == retired.tx.id());
    CHECK(must(tombstone_of(after, id)) == std::optional<tombstone>(t));
    CHECK(must(lifetime_of(after, id)) == id_lifetime::tombstoned);
    // The last record object is still in the store (history), and the record reads as absent.
    CHECK(must(decode_record_object(must(store->get(t.last_record)))).id == id);
    CHECK(must(after.read(id)) == std::nullopt);
  }
  // Tombstones are permanent: later commits and a manager reopened over the store keep them.
  must(tg::commit_ops(m, editor, {must(tg::create_entity("later"))}));
  ws.reset();
  auto reopened = must(workspace::create_with(store, every_class_options()));
  const snapshot head = must(reopened->manager().head());
  CHECK(must(head.entry_count()) == record_classes().size() + 1);
  for (id_class c : record_classes()) {
    CAPTURE(to_string(c));
    const auto t = must(tombstone_of(head, id_of(c, "subject")));
    REQUIRE(t.has_value());
    CHECK(t.value_or(tombstone{}).last_record == last.at(id_of(c, "subject")));
  }
}

TEST_CASE("FAB-INV-LC-02: the classes that name no record are never written nor retired") {
  auto ws = must(workspace::create_memory(every_class_options()));
  transaction_manager &m = ws->manager();
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  CHECK(record_classes().size() == 11);
  for (id_class c : all_classes()) {
    if (names_record(c)) {
      continue;
    }
    CAPTURE(to_string(c));
    const record_id id = id_of(c, "subject");
    // A create under it is refused at the structural stage, so no leaf (and no tombstone) exists.
    tx_options o;
    o.origin = editor;
    auto t = must(m.begin(o));
    must_ok(t.stage(op_create(id, type_of(id_class::entity), labelled("x"))));
    CHECK(t.commit().error().code == errc::validation_failed);
    CHECK(must(lifetime_of(must(m.head()), id)) == id_lifetime::unborn);
  }
  CHECK_FALSE(names_record(id_class::none));
  CHECK_FALSE(names_record(static_cast<id_class>(16)));
}

TEST_CASE("FAB-INV-LC-02: seeded edit histories leave the last record oid in every tombstone") {
  const std::size_t seeds = tg::sweep_scaled(20);
  std::size_t checked = 0;
  for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
    CAPTURE(seed);
    auto ws = must(workspace::create_memory(every_class_options(seed)));
    transaction_manager &m = ws->manager();
    const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
    tg::rng random(seed);
    const std::vector<id_class> classes = record_classes();
    std::vector<record_id> ids;
    std::vector<patch_op> creates;
    for (std::size_t i = 0; i < 8; ++i) {
      const id_class c = classes[random.below(classes.size())];
      const record_id id = must(m.mint(c));
      ids.push_back(id);
      creates.push_back(op_create(id, type_of(c), labelled(std::format("{}/0", i))));
    }
    must(tg::commit_ops(m, editor, creates));
    // A few random relabels, then delete each record in its own transaction.
    for (int step = 0; step < 6; ++step) {
      const record_id &id = ids[random.below(ids.size())];
      must(tg::commit_ops(m, editor,
                          {op_set(id, {path_segment::field(1)},
                                  must(value::string(std::format("s{}", random.next()))))}));
    }
    for (const record_id &id : ids) {
      const digest object = must(must(m.head()).get(id)).object;
      const commit_receipt d = must(tg::commit_ops(m, editor, {op_delete(id)}));
      const auto t = must(tombstone_of(must(m.head()), id));
      REQUIRE(t.has_value());
      CHECK(t.value_or(tombstone{}).last_record == object);
      CHECK(t.value_or(tombstone{}).retired_by == d.tx.id());
      ++checked;
    }
  }
  CHECK(checked == seeds * 8);
}
