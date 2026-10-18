// FAB-INV-SCH-05 (SL-ARCH-001 ARCH1-R25): a migration chain composes and gives identical output
// bytes for identical input. The fixture project (tests/fixtures/fabric/migration/store.json) is
// written under content.demo v1, then a workspace over the same store with the v2 module plans and
// runs the migration: item 1.1 -> 2.0 is one hop of two steps (remove_field with a loss note,
// add_field with a fill), note 1.0 -> 3.0 is a chain of two hops (native 1 -> 2, retype 2 -> 3).
// The result equals the v2 fixture, every record carries the target version, the transaction is
// the system actor's with epistemic class calculated, and two runs from scratch give the same
// bytes, record objects, root and revision. A missing hop is migration_missing_step.

#include "sl/core/error.hpp"
#include "sl/fabric/authority.hpp"
#include "sl/fabric/clock.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/migration.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/provenance.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/workspace.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;
using sl::errc;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

void must_ok(const sl::result<void> &r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
}

json_value read_json(std::string_view name) {
  const std::string path = std::string(SL_FABRIC_FIXTURES_DIR) + "/migration/" + std::string(name);
  std::ifstream in(path, std::ios::binary);
  std::ostringstream s;
  s << in.rdbuf();
  return must(parse_json(s.str()));
}

const json_value &store_fixture() {
  static const json_value doc = read_json("store.json");
  return doc;
}

record_id record_named(std::string_view key) noexcept {
  return derive(id_class::entity, well_known(id_class::namespace_, 1), "wo057/" + std::string(key));
}

actor_id editor() noexcept {
  return actor_id::from(
             derive(id_class::actor, well_known(id_class::namespace_, 1), "wo057/editor"))
      .value_or(actor_id{});
}

tx_origin origin_of(const actor_id &a) {
  tx_origin o;
  o.actor = a;
  o.subsystem = "content";
  return o;
}

// Options over the module `name`: content.demo claimed by content, the editor (human) and the
// migration system actor (tool) registered.
workspace_options options_for(std::string_view name) {
  workspace_options o;
  for (type_desc &t : must(types_from_module(read_json(name)))) {
    must_ok(o.schemas.add(std::move(t)));
  }
  must_ok(o.schemas.freeze());
  must_ok(o.domains.claim(domain_spec{.name = "content.demo",
                                      .owner = subsystem::content,
                                      .state_classes = {state_class::document},
                                      .types = {}}));
  must_ok(o.domains.bind_types(o.schemas));
  must_ok(o.actors.add(actor{.id = editor(), .kind = actor_kind::human, .grants = {}}));
  must_ok(
      o.actors.add(actor{.id = migration_system_actor(), .kind = actor_kind::tool, .grants = {}}));
  o.clock = std::make_shared<manual_clock>(1'000'000);
  o.ids = std::make_shared<deterministic_source>(57, "wo057");
  return o;
}

// Writes the records of fixture section `section` (v1) into `store` under its module.
void load_v1(const std::shared_ptr<store_backend> &store) {
  const json_value &v1 = *store_fixture().find("v1");
  auto ws = must(workspace::create_with(store, options_for(v1.find("module")->text)));
  tx_options o;
  o.origin = origin_of(editor());
  auto t = must(ws->manager().begin(std::move(o)));
  for (const json_value &r : v1.find("records")->items) {
    const type_id type = type_id_of(r.find("type")->text);
    value body = must(from_text(r.find("body")->text, type, ws->schemas()));
    must_ok(t.stage(op_create(record_named(r.find("key")->text), type, std::move(body))));
  }
  (void)must(t.commit());
}

sl::result<value> count_words(const value &body) {
  const value *text = body.find_field(1);
  if (text == nullptr || text->kind() != value_kind::string) {
    return sl::make_error(errc::type_mismatch, "a note needs its text");
  }
  std::uint64_t words = 0;
  bool in_word = false;
  for (const char c : text->as_string()) {
    const bool space = c == ' ';
    words += !space && !in_word ? 1 : 0;
    in_word = !space;
  }
  return body.with({path_segment::field(2)}, value::uint(words));
}

sl::result<value> widen_uint(const value &v) {
  if (v.kind() != value_kind::uint) {
    return sl::make_error(errc::type_mismatch, "words must be a uint");
  }
  return v;
}

// The v1 -> v2 steps; without the note's 2 -> 3 hop when `complete` is false.
migration_registry steps(bool complete = true) {
  migration_registry r;
  must_ok(r.add_converter("demo.note.words", count_words));
  must_ok(r.add_converter("demo.uint.widen", widen_uint));
  const type_id item = type_id_of("content.demo.item");
  const type_id note = type_id_of("content.demo.note");
  must_ok(r.add(migration_step{.type = item,
                               .from = {.major = 1, .minor = 1},
                               .to = {.major = 2, .minor = 0},
                               .kind = step_kind::remove_field,
                               .field = 4,
                               .fill = std::nullopt,
                               .converter = "",
                               .loss = "item.legacy text is dropped"}));
  must_ok(r.add(migration_step{.type = item,
                               .from = {.major = 1, .minor = 1},
                               .to = {.major = 2, .minor = 0},
                               .kind = step_kind::add_field,
                               .field = 5,
                               .fill = must(value::string("untagged")),
                               .converter = "",
                               .loss = ""}));
  must_ok(r.add(migration_step{.type = note,
                               .from = {.major = 1, .minor = 0},
                               .to = {.major = 2, .minor = 0},
                               .kind = step_kind::native,
                               .field = 0,
                               .fill = std::nullopt,
                               .converter = "demo.note.words",
                               .loss = ""}));
  if (complete) {
    must_ok(r.add(migration_step{.type = note,
                                 .from = {.major = 2, .minor = 0},
                                 .to = {.major = 3, .minor = 0},
                                 .kind = step_kind::retype,
                                 .field = 2,
                                 .fill = std::nullopt,
                                 .converter = "demo.uint.widen",
                                 .loss = ""}));
  }
  return r;
}

struct migrated {
  migration_plan plan;
  commit_receipt receipt;
  std::vector<snapshot_entry> entries; // in fixture order
  std::vector<std::vector<std::uint8_t>> bytes;
  digest root;
  std::optional<digest> revision;
  std::uint64_t commits = 0;
};

// The whole pipeline from an empty store.
migrated migrate_from_scratch() {
  auto store = std::make_shared<memory_backend>();
  load_v1(store);
  const json_value &v2 = *store_fixture().find("v2");
  auto ws = must(workspace::create_with(store, options_for(v2.find("module")->text)));
  const migration_registry reg = steps();
  migrated out;
  out.plan = must(plan_migration(must(ws->manager().head()), ws->schemas(), reg));
  out.receipt = must(run_migration(ws->manager(), out.plan, origin_of(editor())));
  const snapshot head = must(ws->manager().head());
  for (const json_value &r : v2.find("records")->items) {
    const snapshot_entry e = must(head.get(record_named(r.find("key")->text)));
    CAPTURE(r.find("key")->text);
    CHECK(e.type == type_id_of(r.find("type")->text));
    CHECK(e.version == ws->schemas().find(e.type)->version);
    CHECK(e.body == must(from_text(r.find("body")->text, e.type, ws->schemas())));
    out.bytes.push_back(must(encode(e.body)));
    out.entries.push_back(e);
  }
  out.root = head.root_digest();
  out.revision = head.revision_digest();
  out.commits = ws->manager().commits();

  // One transaction by the system actor, epistemic class calculated.
  const auto rev = must(decode_revision(must(ws->store().get(out.revision.value_or(digest{})))));
  const auto tx = must(decode_tx_record(must(ws->store().get(rev.tx))));
  CHECK(tx.actor == migration_system_actor().id());
  CHECK(tx.origin.epistemic == epistemic_class::calculated);
  CHECK(tx.origin.subsystem == "05"); // "content", stored as its owner code
  return out;
}

} // namespace

TEST_CASE("FAB-INV-SCH-05: a migration chain composes and gives identical output bytes for "
          "identical input") {
  const migrated first = migrate_from_scratch();

  // The plan: every record, each with its chain, nothing preserved.
  REQUIRE(first.plan.records.size() == 4);
  CHECK(first.plan.preserved.empty());
  CHECK(first.plan.losses() == std::vector<std::string>{"item.legacy text is dropped"});
  for (const planned_record &r : first.plan.records) {
    CAPTURE(r.id.to_string());
    if (r.type == type_id_of("content.demo.item")) {
      CHECK(r.from == type_version{.major = 1, .minor = 1});
      CHECK(r.to == type_version{.major = 2, .minor = 0});
      REQUIRE(r.steps.size() == 2);
      CHECK(r.steps[0].kind == step_kind::remove_field);
      CHECK(r.steps[1].kind == step_kind::add_field);
    } else {
      CHECK(r.type == type_id_of("content.demo.note"));
      CHECK(r.from == type_version{.major = 1, .minor = 0});
      CHECK(r.to == type_version{.major = 3, .minor = 0});
      REQUIRE(r.steps.size() == 2); // two hops composed
      CHECK(r.steps[0].kind == step_kind::native);
      CHECK(r.steps[1].kind == step_kind::retype);
      CHECK(r.steps[1].from == type_version{.major = 2, .minor = 0});
    }
  }
  CHECK(first.commits == 1);

  // Identical input, identical output: bytes, record objects, root and revision.
  const migrated second = migrate_from_scratch();
  CHECK(second.plan.records == first.plan.records);
  CHECK(second.bytes == first.bytes);
  REQUIRE(second.entries.size() == first.entries.size());
  for (std::size_t i = 0; i < first.entries.size(); ++i) {
    CHECK(second.entries[i].object == first.entries[i].object);
  }
  CHECK(second.root == first.root);
  CHECK(second.revision == first.revision);
  CHECK(second.receipt.revision == first.receipt.revision);
}

TEST_CASE("FAB-INV-SCH-05: a missing hop is migration_missing_step naming the type and version") {
  auto store = std::make_shared<memory_backend>();
  load_v1(store);
  const json_value &v2 = *store_fixture().find("v2");
  auto ws = must(workspace::create_with(store, options_for(v2.find("module")->text)));
  const migration_registry reg = steps(false);
  auto plan = plan_migration(must(ws->manager().head()), ws->schemas(), reg);
  REQUIRE_FALSE(plan.has_value());
  CHECK(plan.error().code == errc::migration_missing_step);
  CAPTURE(plan.error().message);
  CHECK(plan.error().message.contains("content.demo.note"));
  CHECK(plan.error().message.contains("from 2.0"));
  CHECK(ws->manager().commits() == 0);

  // The registry refuses an ambiguous hop and a lossy step without a note.
  migration_registry r = steps();
  const type_id note = type_id_of("content.demo.note");
  CHECK(r.add(migration_step{.type = note,
                             .from = {.major = 1, .minor = 0},
                             .to = {.major = 3, .minor = 0},
                             .kind = step_kind::identity,
                             .field = 0,
                             .fill = std::nullopt,
                             .converter = "",
                             .loss = ""})
            .error()
            .code == errc::already_exists);
  CHECK(r.add(migration_step{.type = note,
                             .from = {.major = 3, .minor = 0},
                             .to = {.major = 4, .minor = 0},
                             .kind = step_kind::remove_field,
                             .field = 1,
                             .fill = std::nullopt,
                             .converter = "",
                             .loss = ""})
            .error()
            .code == errc::migration_lossy);
  CHECK(r.add(migration_step{.type = note,
                             .from = {.major = 3, .minor = 0},
                             .to = {.major = 4, .minor = 0},
                             .kind = step_kind::native,
                             .field = 0,
                             .fill = std::nullopt,
                             .converter = "no.such.converter",
                             .loss = ""})
            .error()
            .code == errc::not_found);
}
