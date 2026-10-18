// FAB-INV-SCH-06 (SL-ARCH-001 ARCH1-R25): a failed migration commits nothing and leaves the old
// revision readable. Over the v1 fixture project, a v2 workspace runs migrations that fail three
// ways: a native converter error (path "/"), a retype converter error (path "/2") and a converted
// body the schema validator refuses (R-TY-REQUIRED). Each is migration_failed naming the record
// and the path; the head, the commit count and every v1 record object stay as they were and the
// planned revision still reads. A ref that moved after planning is stale_base, also with nothing
// written.

#include "sl/core/error.hpp"
#include "sl/fabric/authority.hpp"
#include "sl/fabric/clock.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/migration.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/patch.hpp"
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

workspace_options options_for(std::string_view module) {
  workspace_options o;
  for (type_desc &t : must(types_from_module(read_json(module)))) {
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

// The v1 project in `store`; returns its live records.
std::vector<snapshot_entry> load_v1(const std::shared_ptr<store_backend> &store) {
  const json_value doc = read_json("store.json");
  const json_value &v1 = *doc.find("v1");
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
  std::vector<snapshot_entry> out;
  must_ok(must(ws->manager().head()).for_each_entry([&](const snapshot_entry &e) {
    out.push_back(e);
    return sl::result<void>{};
  }));
  return out;
}

sl::result<value> count_words_failing_on_valley(const value &body) {
  const value *text = body.find_field(1);
  if (text == nullptr || text->as_string().contains("valley")) {
    return sl::make_error(errc::invalid_argument, "cannot count the words of this note");
  }
  return body.with({path_segment::field(2)}, value::uint(1));
}

sl::result<value> count_nothing(const value &body) { return body; } // leaves required words out

sl::result<value> refuse(const value & /*body*/) {
  return sl::make_error(errc::type_mismatch, "words cannot widen");
}

sl::result<value> keep(const value &v) { return v; }

sl::result<value> count_one(const value &body) {
  return body.with({path_segment::field(2)}, value::uint(1));
}

// v1 -> v2 steps with the note converters given.
migration_registry steps_with(sl::result<value> (*words)(const value &),
                              sl::result<value> (*widen)(const value &)) {
  migration_registry r;
  must_ok(r.add_converter("demo.note.words", words));
  must_ok(r.add_converter("demo.uint.widen", widen));
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
  must_ok(r.add(migration_step{.type = note,
                               .from = {.major = 2, .minor = 0},
                               .to = {.major = 3, .minor = 0},
                               .kind = step_kind::retype,
                               .field = 2,
                               .fill = std::nullopt,
                               .converter = "demo.uint.widen",
                               .loss = ""}));
  return r;
}

// Runs a migration with `reg` that must fail with `code`; nothing may change.
sl::error fails_cleanly(workspace &ws, const std::vector<snapshot_entry> &v1,
                        const migration_registry &reg, errc code, bool move_head_first = false) {
  const snapshot before = must(ws.manager().head());
  const migration_plan plan = must(plan_migration(before, ws.schemas(), reg));
  std::uint64_t commits = ws.manager().commits();
  snapshot expected_head = before;
  if (move_head_first) {
    // Another edit lands after planning: an annotate-only commit moves the ref.
    tx_options o;
    o.origin = origin_of(editor());
    auto t = must(ws.manager().begin(std::move(o)));
    must_ok(t.stage(op_annotate(value::uint(7))));
    (void)must(t.commit());
    ++commits;
    expected_head = must(ws.manager().head());
  }
  auto r = run_migration(ws.manager(), plan, origin_of(editor()));
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == code);
  CHECK(ws.manager().commits() == commits);
  const snapshot after = must(ws.manager().head());
  CHECK(after.revision_digest() == expected_head.revision_digest());
  CHECK(after.root_digest() == before.root_digest());
  // The planned (old) revision still reads, record for record.
  const snapshot old = must(ws.manager().at(plan.base.value_or(digest{})));
  for (const snapshot_entry &e : v1) {
    CHECK(must(old.get(e.id)) == e);
    CHECK(must(after.get(e.id)) == e);
  }
  return r.error();
}

} // namespace

TEST_CASE("FAB-INV-SCH-06: a failed migration commits nothing and leaves the old revision "
          "readable") {
  auto store = std::make_shared<memory_backend>();
  const std::vector<snapshot_entry> v1 = load_v1(store);
  REQUIRE(v1.size() == 4);
  const json_value doc = read_json("store.json");
  auto ws = must(workspace::create_with(store, options_for(doc.find("v2")->find("module")->text)));
  const std::string sky = record_named("note/sky").to_string();
  const std::string empty = record_named("note/empty").to_string();

  SUBCASE("a native converter error names the record and the root path") {
    const sl::error e = fails_cleanly(*ws, v1, steps_with(count_words_failing_on_valley, keep),
                                      errc::migration_failed);
    CAPTURE(e.message);
    CHECK(e.message.contains("record " + sky));
    CHECK(e.message.contains(" at /: step native demo.note.words"));
    CHECK(e.message.contains("cannot count"));
  }
  SUBCASE("a retype converter error names the field path") {
    const sl::error e =
        fails_cleanly(*ws, v1, steps_with(count_one, refuse), errc::migration_failed);
    CAPTURE(e.message);
    CHECK((e.message.contains("record " + sky) || e.message.contains("record " + empty)));
    CHECK(e.message.contains(" at /2: step retype demo.uint.widen"));
  }
  SUBCASE("a body the validators refuse names the record and the diagnostic") {
    const sl::error e =
        fails_cleanly(*ws, v1, steps_with(count_nothing, keep), errc::migration_failed);
    CAPTURE(e.message);
    CHECK((e.message.contains("record " + sky) || e.message.contains("record " + empty)));
    CHECK(e.message.contains("R-TY-REQUIRED"));
  }
  SUBCASE("a ref that moved after planning is stale_base") {
    (void)fails_cleanly(*ws, v1, steps_with(count_nothing, keep), errc::stale_base, true);
  }
}
