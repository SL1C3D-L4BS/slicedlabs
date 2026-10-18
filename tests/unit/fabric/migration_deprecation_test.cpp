// FAB-INV-SCH-08 (SL-ARCH-001 ARCH1-R24): a deprecated field decodes and writing it yields a
// warning diagnostic. content.demo.item 1.1 deprecates field 4 (legacy) in minor 1. With
// deprecation_validator registered, creating an item that holds legacy, or changing legacy,
// commits with one warning R-SCH-DEPRECATED at path /4; writes that leave legacy alone (another
// field, an item without it) give none. The stored body keeps the field, decodes under the strict
// policy and conforms to the type.

#include "sl/core/error.hpp"
#include "sl/fabric/authority.hpp"
#include "sl/fabric/clock.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/migration.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/workspace.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
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

workspace_options options() {
  workspace_options o;
  for (type_desc &t : must(types_from_module(read_json("content.demo.v1.sltype.json")))) {
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
  o.ids = std::make_shared<deterministic_source>(57, "wo057");
  o.validators.push_back(std::make_shared<deprecation_validator>());
  return o;
}

// Commits `op` and returns the R-SCH-DEPRECATED diagnostics of the commit.
std::vector<diagnostic> deprecation_warnings(workspace &ws, const patch_op &op) {
  tx_options o;
  o.origin.actor = editor();
  o.origin.subsystem = "content";
  auto t = must(ws.manager().begin(std::move(o)));
  must_ok(t.stage(op));
  (void)must(t.commit());
  std::vector<diagnostic> out;
  for (const diagnostic &d : t.diagnostics()) {
    if (d.rule == "R-SCH-DEPRECATED") {
      out.push_back(d);
    }
  }
  return out;
}

} // namespace

TEST_CASE("FAB-INV-SCH-08: a deprecated field decodes and writing it yields a warning "
          "diagnostic") {
  auto ws = must(workspace::create_memory(options()));
  const type_id item = type_id_of("content.demo.item");
  const record_id lamp = record_named("item/lamp");
  const record_id rope = record_named("item/rope");
  const sl::fabric::path legacy{path_segment::field(4)};

  // Creating a record that holds the deprecated field: committed, with one warning at /4.
  const auto created = deprecation_warnings(
      *ws, op_create(lamp, item,
                     must(from_text(R"({"label":"lamp","count":3,"legacy":"brass"})", item,
                                    ws->schemas()))));
  REQUIRE(created.size() == 1);
  CHECK(created[0].severity == severity::warning);
  CHECK(created[0].record == lamp);
  CHECK(created[0].path == legacy);
  CHECK(created[0].stage == validator_stage::schema);
  CHECK(created[0].message.starts_with("deprecated: field 4 (legacy)"));

  // The deprecated field still decodes, strictly, and conforms.
  const snapshot_entry stored = must(must(ws->manager().head()).get(lamp));
  REQUIRE(stored.body.find_field(4) != nullptr);
  CHECK(stored.body.find_field(4)->as_string() == "brass");
  const auto bytes = must(encode(stored.body));
  CHECK(must(decode(bytes, unknown_policy::strict, item, ws->schemas())) == stored.body);
  std::vector<diagnostic_lite> conformance;
  must_ok(ws->schemas().check(stored.body, item, conformance));
  CHECK(conformance.empty());

  // Writes that leave it alone give no warning.
  CHECK(deprecation_warnings(
            *ws, op_set(lamp, {path_segment::field(1)}, must(value::string("brass lamp"))))
            .empty());
  CHECK(deprecation_warnings(
            *ws, op_create(rope, item,
                           must(from_text(R"({"label":"rope","count":12})", item, ws->schemas()))))
            .empty());

  // Changing it, or adding it to a record without it, warns again.
  const auto changed =
      deprecation_warnings(*ws, op_set(lamp, legacy, must(value::string("copper"))));
  REQUIRE(changed.size() == 1);
  CHECK(changed[0].path == legacy);
  const auto added = deprecation_warnings(*ws, op_set(rope, legacy, must(value::string("hemp"))));
  REQUIRE(added.size() == 1);
  CHECK(added[0].record == rope);
  CHECK(ws->manager().commits() == 5);
}
