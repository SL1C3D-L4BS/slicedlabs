// FAB-INV-TY-05 (SL-ARCH-001 ARCH1-R17): freeze rejects record types with no domain or a state
// class outside document, derived, observation; add after freeze is schema_frozen.

#include "sl/fabric/gen/system.gen.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/type.hpp"

#include "../../../engine/fabric/testing/test_schemas.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;

type_desc record_type(std::string_view name, state_class sc, std::string_view domain) {
  type_desc t;
  t.name = std::string(name);
  t.id = type_id_of(name);
  t.kind = type_kind::struct_;
  t.record_class = id_class::entity;
  t.state_class = sc;
  t.domain = std::string(domain);
  field_desc f;
  f.no = 1;
  f.name = "name";
  f.type = type_id_of("string");
  t.fields = {f};
  return t;
}

sl::result<void> freeze_one(type_desc t, schema_registry &reg) {
  REQUIRE(reg.add(std::move(t)).has_value());
  return reg.freeze();
}

bool mentions(const sl::result<void> &r, std::string_view text) {
  return !r && r.error().message.contains(text);
}

} // namespace

TEST_CASE("FAB-INV-TY-05: a record type needs a domain") {
  schema_registry reg;
  const auto r = freeze_one(record_type("t.Rec", state_class::document, ""), reg);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == sl::errc::schema_invalid);
  CHECK(mentions(r, "R-TY-DOMAIN: type 't.Rec'"));
  CHECK_FALSE(reg.frozen());
  // A non-record type needs no domain here (ownership is WO-048).
  schema_registry plain;
  type_desc s = record_type("t.Plain", state_class::none, "");
  s.record_class = id_class::none;
  CHECK(freeze_one(s, plain).has_value());
}

TEST_CASE("FAB-INV-TY-05: a record type's state class is document, derived or observation") {
  for (const state_class ok :
       {state_class::document, state_class::derived, state_class::observation}) {
    CAPTURE(to_string(ok));
    schema_registry reg;
    CHECK(freeze_one(record_type("t.Rec", ok, "content.t"), reg).has_value());
    CHECK(reg.frozen());
  }
  // No state class, or a value outside the three (no transient class exists: ADR-0013).
  for (const auto bad :
       {state_class::none, static_cast<state_class>(4), static_cast<state_class>(200)}) {
    schema_registry reg;
    const auto r = freeze_one(record_type("t.Rec", bad, "content.t"), reg);
    CHECK(mentions(r, "R-TY-STATE"));
    CHECK_FALSE(reg.frozen());
  }
  // A document root without a record class is a record type with no record class.
  schema_registry reg;
  type_desc root = record_type("t.Root", state_class::document, "content.t");
  root.record_class = id_class::none;
  root.document_root = true;
  CHECK(mentions(freeze_one(root, reg), "R-TY-RECORD"));
  type_desc reserved_class = record_type("t.Odd", state_class::document, "content.t");
  reserved_class.record_class = static_cast<id_class>(99);
  schema_registry reg2;
  CHECK(mentions(freeze_one(reserved_class, reg2), "R-TY-RECORD"));
}

TEST_CASE("FAB-INV-TY-05: freeze lists every violation, sorted, and changes nothing") {
  schema_registry reg;
  REQUIRE(reg.add(record_type("t.B", state_class::none, "")).has_value());
  REQUIRE(reg.add(record_type("t.A", state_class::document, "")).has_value());
  const auto r = reg.freeze();
  REQUIRE_FALSE(r.has_value());
  const std::string &m = r.error().message;
  CHECK(m.starts_with("freeze refused, 3 violation(s):"));
  const auto a_domain = m.find("R-TY-DOMAIN: type 't.A'");
  const auto b_domain = m.find("R-TY-DOMAIN: type 't.B'");
  const auto b_state = m.find("R-TY-STATE: type 't.B'");
  REQUIRE(a_domain != std::string::npos);
  REQUIRE(b_domain != std::string::npos);
  REQUIRE(b_state != std::string::npos);
  CHECK(a_domain < b_domain);
  CHECK(b_domain < b_state);
  CHECK_FALSE(reg.frozen());
  // Still open: a refused freeze leaves add() usable.
  CHECK(reg.add(record_type("t.C", state_class::derived, "asset.t")).has_value());
}

TEST_CASE("FAB-INV-TY-05: add after freeze is schema_frozen") {
  schema_registry reg;
  REQUIRE(testing::add_test_schemas(reg).has_value());
  const std::size_t n = reg.size();
  REQUIRE(reg.freeze().has_value());
  CHECK(reg.frozen());
  const auto late = reg.add(record_type("t.Late", state_class::document, "content.t"));
  REQUIRE_FALSE(late.has_value());
  CHECK(late.error().code == sl::errc::schema_frozen);
  CHECK(reg.size() == n);
  CHECK(reg.find("t.Late") == nullptr);
  // Freezing again is harmless.
  CHECK(reg.freeze().has_value());
  // A repeated id is refused before freeze.
  schema_registry dup;
  REQUIRE(dup.add(record_type("t.X", state_class::document, "content.t")).has_value());
  const auto again = dup.add(record_type("t.X", state_class::document, "content.t"));
  REQUIRE_FALSE(again.has_value());
  CHECK(again.error().message.starts_with("R-TY-DUP"));
  CHECK(dup.add(builtin_types().front()).error().message.starts_with("R-TY-DUP"));
}

TEST_CASE("FAB-INV-TY-05: the frozen registry finds types by id and name, in id order") {
  const auto reg = testing::make_test_registry();
  REQUIRE(reg.has_value());
  const type_desc *entity = reg->find(testing::test_types::entity);
  REQUIRE(entity != nullptr);
  CHECK(entity->is_record());
  CHECK(entity->domain == "content.scene");
  CHECK(entity->state_class == state_class::document);
  CHECK(reg->find(entity->id) == entity);
  CHECK(reg->find("content.scene.Nope") == nullptr);
  CHECK(reg->find("u64") != nullptr);
  CHECK(reg->size() == builtin_types().size() + 15);
  std::vector<type_id> ids;
  for (const auto &[id, t] : reg->types()) {
    CHECK(id == t.id);
    ids.push_back(id);
  }
  CHECK(std::ranges::is_sorted(ids));
  // Every record type of the test module passes ARCH1-R17.
  for (const auto &[id, t] : reg->types()) {
    if (t.is_record()) {
      CAPTURE(t.name);
      CHECK_FALSE(t.domain.empty());
      CHECK(is_valid(t.record_class));
      CHECK(t.state_class != state_class::none);
    }
  }
}

TEST_CASE("FAB-INV-TY-05: a state class alone makes a record type, so a declared value type "
          "registers with none and keeps its state class in its type entry") {
  // The rule documented on type_desc::is_record (WO-064).
  type_desc v;
  v.name = "content.demo.value";
  v.domain = "content.demo";
  CHECK_FALSE(v.is_record());
  v.state_class = state_class::document;
  CHECK(v.is_record());
  v.state_class = state_class::none;
  v.document_root = true;
  CHECK(v.is_record());

  const schema_registry reg = gen::system_registry().value_or(schema_registry{});
  REQUIRE(reg.frozen());
  std::size_t values = 0;
  std::size_t records = 0;
  for (const gen::type_entry &e : gen::system_types()) {
    CAPTURE(e.name);
    const type_desc *d = reg.find(e.name);
    REQUIRE(d != nullptr);
    CHECK(e.declared_state != state_class::none); // sl-type-def/1 declares one for every type
    if (d->record_class == id_class::none && !d->document_root) {
      CHECK(d->state_class == state_class::none);
      CHECK_FALSE(d->is_record());
      ++values;
    } else {
      CHECK(d->state_class == e.declared_state);
      ++records;
    }
  }
  CHECK(values > 0);
  CHECK(records > 0);
}
