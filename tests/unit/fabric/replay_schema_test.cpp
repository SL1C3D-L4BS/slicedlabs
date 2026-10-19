// FAB-INV-RPL-03 (SL-ARCH-002 ARCH2-R52; SL-ARCH-001 ARCH1-R22 and ARCH1-R50; master plan, Volume
// 04, replay seams): a revision pins the schema set it was written with. The WO-057 fixture project
// is written under content.demo v1; a workspace over the same store with the v2 module (the schemas
// evolved) still opens the old revision read-only, its records keep their v1 type versions and
// bodies, and its revision object names the v1 schema set digest, not the v2 one. A commit made
// under v2 names the v2 digest, while the old revision is unchanged (negative control). The schema
// set digest is SHA-256 over the sorted schema ids (codec.hpp schema_set_digest); the schema
// objects themselves are not stored per revision (SL-ARCH-001 open issue).

#include "sl/core/error.hpp"
#include "sl/fabric/authority.hpp"
#include "sl/fabric/clock.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/migration.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/workspace.hpp"

#include <doctest/doctest.h>

#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;

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
  return derive(id_class::entity, well_known(id_class::namespace_, 1), "wo062/" + std::string(key));
}

actor_id editor() noexcept {
  return actor_id::from(
             derive(id_class::actor, well_known(id_class::namespace_, 1), "wo062/editor"))
      .value_or(actor_id{});
}

tx_options editor_tx() {
  tx_options o;
  o.origin.actor = editor();
  o.origin.subsystem = "content";
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
  o.clock = std::make_shared<manual_clock>(1'000'000);
  o.ids = std::make_shared<deterministic_source>(62, "wo062");
  return o;
}

std::string module_of(std::string_view section) {
  return store_fixture().find(section)->find("module")->text;
}

struct written {
  std::shared_ptr<memory_backend> store = std::make_shared<memory_backend>();
  digest revision;
  digest v1_set;
  std::vector<snapshot_entry> entries;
};

written write_v1() {
  written w;
  auto ws = must(workspace::create_with(w.store, options_for(module_of("v1"))));
  w.v1_set = must(schema_set_digest(ws->schemas()));
  auto t = must(ws->manager().begin(editor_tx()));
  for (const json_value &r : store_fixture().find("v1")->find("records")->items) {
    const type_id type = type_id_of(r.find("type")->text);
    must_ok(t.stage(op_create(record_named(r.find("key")->text), type,
                              must(from_text(r.find("body")->text, type, ws->schemas())))));
  }
  w.revision = must(t.commit()).revision;
  const snapshot head = must(ws->manager().head());
  REQUIRE(head.for_each_entry([&](const snapshot_entry &e) -> sl::result<void> {
                w.entries.push_back(e);
                return {};
              })
              .has_value());
  return w;
}

} // namespace

TEST_CASE("FAB-INV-RPL-03: an old revision opens with the schema set it was written with") {
  const written w = write_v1();
  REQUIRE_FALSE(w.entries.empty());
  auto ws = must(workspace::create_with(w.store, options_for(module_of("v2"))));
  const digest v2_set = must(schema_set_digest(ws->schemas()));
  REQUIRE(v2_set != w.v1_set);

  const auto rev = must(decode_revision(must(ws->store().get(w.revision))));
  CHECK(rev.schema_set == w.v1_set);

  const snapshot old = must(ws->manager().at(w.revision));
  std::size_t older = 0;
  for (const snapshot_entry &e : w.entries) {
    const snapshot_entry now = must(old.get(e.id));
    CHECK(now == e); // same v1 version, body and record object
    const type_desc *t = ws->schemas().find(e.type);
    REQUIRE(t != nullptr);
    older += e.version != t->version ? 1 : 0;
  }
  CHECK(older > 0); // some types evolved between v1 and v2
}

TEST_CASE("FAB-INV-RPL-03: a commit under the evolved schemas names the new set, the old stays") {
  const written w = write_v1();
  auto ws = must(workspace::create_with(w.store, options_for(module_of("v2"))));
  const digest v2_set = must(schema_set_digest(ws->schemas()));
  const commit_receipt c =
      must(ws->manager().create_ref("replay/pin", w.revision, editor_tx().origin));
  const auto pinned = must(decode_revision(must(ws->store().get(c.revision))));
  CHECK(pinned.schema_set == v2_set);
  CHECK(pinned.schema_set != w.v1_set);
  CHECK(must(decode_revision(must(ws->store().get(w.revision)))).schema_set == w.v1_set);
}
