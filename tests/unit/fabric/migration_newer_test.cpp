// FAB-INV-SCH-07 (SL-ARCH-001 ARCH1-R23): a record from a newer minor is preserved byte-identically
// and refused for write. A workspace with content.demo.item 1.1 writes two items (one holding
// field 3, which only minor 1 declares); a workspace over the same store whose registry knows
// item 1.0 (and registers newer_minor_validator) reads them with the unknown field kept, refuses
// set and delete on them (R-SCH-NEWER, read_only_newer) while a note at its own version stays
// writable, and keeps both record objects unchanged through other commits and through a
// migration, which lists them as preserved.

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
#include "sl/fabric/validate.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/workspace.hpp"

#include <doctest/doctest.h>

#include <algorithm>
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

// The v1 module; with `older`, item is cut back to 1.0 (field 3 dropped, nothing deprecated).
workspace_options options(bool older) {
  workspace_options o;
  for (type_desc &t : must(types_from_module(read_json("content.demo.v1.sltype.json")))) {
    if (older && t.name == "content.demo.item") {
      t.version.minor = 0;
      std::erase_if(t.fields, [](const field_desc &f) { return f.since_minor > 0; });
      for (field_desc &f : t.fields) {
        f.deprecated_in.reset();
      }
    }
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
  o.validators.push_back(std::make_shared<newer_minor_validator>());
  return o;
}

sl::result<commit_receipt> commit_ops(workspace &ws, const std::vector<patch_op> &ops,
                                      std::vector<diagnostic> *diags = nullptr) {
  tx_options o;
  o.origin = origin_of(editor());
  auto t = ws.manager().begin(std::move(o));
  if (!t) {
    return std::unexpected(t.error());
  }
  for (const patch_op &op : ops) {
    if (auto ok = t->stage(op); !ok) {
      return std::unexpected(ok.error());
    }
  }
  auto r = t->commit();
  if (diags != nullptr) {
    diags->assign(t->diagnostics().begin(), t->diagnostics().end());
  }
  return r;
}

bool refused_as_newer(const std::vector<diagnostic> &diags, const record_id &id) {
  return std::ranges::any_of(diags, [&](const diagnostic &d) {
    return d.rule == "R-SCH-NEWER" && d.record == id && d.severity == severity::error &&
           d.message.starts_with("read_only_newer: ");
  });
}

} // namespace

TEST_CASE("FAB-INV-SCH-07: a record from a newer minor is preserved byte-identically and refused "
          "for write") {
  auto store = std::make_shared<memory_backend>();
  const record_id lamp = record_named("item/lamp");
  const record_id rope = record_named("item/rope");
  const record_id sky = record_named("note/sky");
  snapshot_entry lamp_v11;
  snapshot_entry rope_v11;
  {
    auto newer = must(workspace::create_with(store, options(false)));
    const type_id item = type_id_of("content.demo.item");
    const type_id note = type_id_of("content.demo.note");
    (void)must(commit_ops(
        *newer,
        {op_create(
             lamp, item,
             must(from_text(R"({"label":"lamp","count":3,"weight":1.5})", item, newer->schemas()))),
         op_create(rope, item,
                   must(from_text(R"({"label":"rope","count":12})", item, newer->schemas()))),
         op_create(sky, note, must(from_text(R"({"text":"stars"})", note, newer->schemas())))}));
    const snapshot head = must(newer->manager().head());
    lamp_v11 = must(head.get(lamp));
    rope_v11 = must(head.get(rope));
    CHECK(lamp_v11.version == type_version{.major = 1, .minor = 1});
  }

  auto older = must(workspace::create_with(store, options(true)));
  const auto unchanged = [&](const snapshot &s) {
    for (const snapshot_entry *e : {&lamp_v11, &rope_v11}) {
      const snapshot_entry now = must(s.get(e->id));
      CHECK(now.object == e->object);
      CHECK(now.version == e->version);
      CHECK(must(encode(now.body)) == must(encode(e->body)));
    }
  };

  // Read: the unknown field 3 is kept byte-exact.
  const snapshot head = must(older->manager().head());
  unchanged(head);
  const std::string text =
      must(to_text(must(head.get(lamp)).body, lamp_v11.type, older->schemas()));
  CAPTURE(text);
  CHECK(text.contains("\"?3\":\"slb:"));
  auto verdict = check_writable(must(head.get(lamp)), older->schemas());
  REQUIRE_FALSE(verdict.has_value());
  CHECK(verdict.error().code == errc::read_only_newer);
  must_ok(check_writable(must(head.get(sky)), older->schemas()));

  // Writes to a newer-minor record are refused: set, retype and delete; nothing is written.
  const std::uint64_t commits = older->manager().commits();
  std::vector<diagnostic> diags;
  auto set = commit_ops(
      *older, {op_set(lamp, {path_segment::field(1)}, must(value::string("brass lamp")))}, &diags);
  REQUIRE_FALSE(set.has_value());
  CHECK(set.error().code == errc::validation_failed);
  CHECK(refused_as_newer(diags, lamp));
  auto del = commit_ops(*older, {op_delete(rope)}, &diags);
  REQUIRE_FALSE(del.has_value());
  CHECK(refused_as_newer(diags, rope));
  auto retype = commit_ops(*older, {op_retype(rope, rope_v11.type, rope_v11.body)}, &diags);
  REQUIRE_FALSE(retype.has_value());
  CHECK(refused_as_newer(diags, rope));
  CHECK(older->manager().commits() == commits);
  CHECK(must(older->manager().head()).revision_digest() == head.revision_digest());

  // A record at this reader's version stays writable, and the newer ones stay byte-identical.
  (void)must(commit_ops(
      *older, {op_set(sky, {path_segment::field(1)}, must(value::string("moon")))}, &diags));
  CHECK_FALSE(refused_as_newer(diags, sky));
  unchanged(must(older->manager().head()));

  // A migration preserves them: listed, never written.
  const migration_registry none;
  const migration_plan plan =
      must(plan_migration(must(older->manager().head()), older->schemas(), none));
  CHECK(plan.records.empty());
  CHECK(plan.preserved == std::vector<record_id>{std::min(lamp, rope), std::max(lamp, rope)});
  (void)must(run_migration(older->manager(), plan, origin_of(editor())));
  unchanged(must(older->manager().head()));
}
