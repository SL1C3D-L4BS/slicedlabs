// FAB-INV-OWN-06 (SL-ARCH-001 ARCH1-R39): a nested value of another domain needs that domain's
// grant.
#include "sl/core/error.hpp"
#include "sl/fabric/authority.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/value.hpp"

#include "../../../engine/fabric/testing/model_store.hpp"
#include "../../../engine/fabric/testing/test_schemas.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;
namespace tg = sl::fabric::testing;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

void must_ok(const sl::result<void> &r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
}

field_desc fld(std::uint32_t no, std::string_view name, std::string_view type,
               presence p = presence::required) {
  field_desc f;
  f.no = no;
  f.name = std::string(name);
  f.type = type_id_of(type);
  f.presence = p;
  return f;
}

type_desc named(std::string_view name, type_kind kind, std::string_view domain) {
  type_desc t;
  t.name = std::string(name);
  t.id = type_id_of(name);
  t.kind = kind;
  t.domain = std::string(domain);
  return t;
}

// A scene entity (content.scene, owner 05) holding an oset of components; a component is a tag
// (content.scene) or a physics body (sim.physics, owner 06): the nested value of another domain
// of ADR-0017.
schema_registry scene_schemas() {
  schema_registry reg;
  type_desc body = named("sim.physics.Body", type_kind::struct_, "sim.physics");
  body.fields = {fld(1, "mass", "f64")};
  type_desc tag = named("content.scene.Tag", type_kind::struct_, "content.scene");
  tag.fields = {fld(1, "label", "string")};
  type_desc component = named("content.scene.Component", type_kind::variant, "content.scene");
  component.cases = {case_desc{.no = 1, .name = "tag", .type = tag.id, .doc = {}},
                     case_desc{.no = 2, .name = "body", .type = body.id, .doc = {}}};
  type_desc components = named("content.scene.Components", type_kind::oset, "");
  components.element = component.id;
  type_desc entity = named("content.scene.Entity", type_kind::struct_, "content.scene");
  entity.record_class = id_class::entity;
  entity.state_class = state_class::document;
  entity.fields = {fld(1, "name", "string"),
                   fld(2, "components", "content.scene.Components", presence::optional)};
  for (type_desc t : {body, tag, component, components, entity}) {
    must_ok(reg.add(std::move(t)));
  }
  must_ok(reg.freeze());
  return reg;
}

domain_registry scene_domains() {
  domain_registry reg;
  must_ok(reg.claim(domain_spec{.name = "content.scene",
                                .owner = subsystem::content,
                                .state_classes = {state_class::document},
                                .types = {}}));
  must_ok(reg.claim(domain_spec{.name = "sim.physics",
                                .owner = subsystem::sim,
                                .state_classes = {state_class::document},
                                .types = {}}));
  must_ok(reg.freeze());
  return reg;
}

const record_id hero = tg::test_id(id_class::entity, "authority/hero");
const type_id entity_type = type_id_of("content.scene.Entity");

id_bytes elem(std::string_view name) { return tg::test_id(id_class::element, name).bytes(); }

value tag_component(std::string_view label) {
  return must(
      value::variant(1, must(value::struct_({field{.no = 1, .v = must(value::string(label))}}))));
}

value body_component(double mass) {
  return must(
      value::variant(2, must(value::struct_({field{.no = 1, .v = must(value::f64(mass))}}))));
}

value entity(std::string_view name, std::vector<oset_entry> components) {
  return must(value::struct_({field{.no = 1, .v = must(value::string(name))},
                              field{.no = 2, .v = must(value::oset(std::move(components)))}}));
}

/// The hero entity with a tag component "t" and a physics body component "b".
tg::oracle_store fixture() {
  tg::oracle_store store;
  store.put(
      hero,
      record_state{
          .type = entity_type,
          .body = entity(
              "hero",
              {oset_entry{.element = elem("t"), .order_key = {0x40}, .v = tag_component("hero")},
               oset_entry{.element = elem("b"), .order_key = {0x80}, .v = body_component(1.0)}})});
  return store;
}

actor make_actor(std::string_view key, actor_kind kind, std::vector<std::string> grants = {}) {
  return actor{.id = must(actor_id::from(tg::test_id(id_class::actor, key))),
               .kind = kind,
               .grants = std::move(grants)};
}

/// Runs the authority validator over `p` (normalised on `base`) for `who` editing through
/// `subsystem`.
std::vector<diagnostic> judge(const tg::oracle_store &base, const patch &p, const actor &who,
                              std::string_view subsystem) {
  static const schema_registry schemas = scene_schemas();
  static const domain_registry domains = scene_domains();
  actor_directory directory;
  must_ok(directory.add(who));
  const patch n = must(normalize(p, base));
  staged_state staged(base);
  must_ok(staged.apply(n));
  const validation_context ctx(n, staged, schemas,
                               edit_origin{.actor = who.id, .subsystem = std::string(subsystem)});
  const authority_validator v(domains, directory);
  std::vector<diagnostic> out;
  must_ok(v.run(ctx, out));
  return out;
}

patch single(patch_op op) {
  patch p;
  p.ops.push_back(std::move(op));
  return p;
}

/// The domains named by R-AUTH-DENIED diagnostics, in order.
std::vector<std::string> denied_domains(const std::vector<diagnostic> &ds) {
  std::vector<std::string> out;
  for (const diagnostic &d : ds) {
    CHECK(d.rule == "R-AUTH-DENIED");
    CHECK(d.severity == severity::error);
    for (const std::string_view domain : {"content.scene", "sim.physics"}) {
      if (d.message.contains(std::string(" ") + std::string(domain) + " ")) {
        out.emplace_back(domain);
      }
    }
  }
  return out;
}

} // namespace

TEST_CASE("FAB-INV-OWN-06: a physics body inside a scene entity needs sim as well as content") {
  const tg::oracle_store base = fixture();
  const actor tool = make_actor("inner/tool", actor_kind::tool);
  const path components{path_segment::field(2)};
  const patch add_body =
      single(op_insert(hero, components, elem("b2"), {0xC0}, body_component(2.0)));
  // Through content: the record is content's, the inserted body is sim's.
  const auto ds = judge(base, add_body, tool, "content");
  REQUIRE(ds.size() == 1);
  CHECK(denied_domains(ds) == std::vector<std::string>{"sim.physics"});
  // The boundary is the innermost typed value whose domain differs: the variant case payload.
  CHECK(to_string(ds[0].path) ==
        to_string(path{path_segment::field(2), path_segment::element(elem("b2")),
                       path_segment::field(2)}));
  // Through sim: the body is sim's, but the entity is content's.
  CHECK(denied_domains(judge(base, add_body, tool, "sim")) ==
        std::vector<std::string>{"content.scene"});
  // A grant for sim lets the content-side tool add the body.
  CHECK(judge(base, add_body, make_actor("inner/g", actor_kind::tool, {"write:sim.physics"}),
              "content")
            .empty());
  // A tag component stays inside content.
  CHECK(judge(base, single(op_insert(hero, components, elem("t2"), {0xC0}, tag_component("x"))),
              tool, "content")
            .empty());
}

TEST_CASE("FAB-INV-OWN-06: editing and removing a nested body need sim; moving it does not") {
  const tg::oracle_store base = fixture();
  const actor tool = make_actor("inner/tool", actor_kind::tool);
  const path body_path{path_segment::field(2), path_segment::element(elem("b")),
                       path_segment::field(2)};
  path mass = body_path;
  mass.push_back(path_segment::field(1));
  const patch set_mass = single(op_set(hero, mass, must(value::f64(5.0))));
  CHECK(denied_domains(judge(base, set_mass, tool, "content")) ==
        std::vector<std::string>{"sim.physics"});
  CHECK(judge(base, set_mass, make_actor("inner/both", actor_kind::tool, {"write:sim"}), "content")
            .empty());
  const path components{path_segment::field(2)};
  CHECK(denied_domains(judge(base, single(op_remove(hero, components, elem("b"))), tool,
                             "content")) == std::vector<std::string>{"sim.physics"});
  // Moving changes only the oset's order key, which is content's: the body itself is untouched.
  CHECK(judge(base, single(op_move(hero, components, elem("b"), {0x20})), tool, "content").empty());
  // Replacing the whole component set removes the body: sim is needed for what is replaced.
  const patch replace = single(op_set(hero, components, must(value::oset({}))));
  CHECK(denied_domains(judge(base, replace, tool, "content")) ==
        std::vector<std::string>{"sim.physics"});
  // Renaming the entity touches no body.
  CHECK(judge(base, single(op_set(hero, {path_segment::field(1)}, must(value::string("z")))), tool,
              "content")
            .empty());
}

TEST_CASE("FAB-INV-OWN-06: creating an entity with a body needs both grants, once each") {
  const tg::oracle_store base = fixture();
  const record_id other = tg::test_id(id_class::entity, "inner/other");
  const patch create = single(op_create(
      other, entity_type,
      entity("other",
             {oset_entry{.element = elem("b1"), .order_key = {0x40}, .v = body_component(1.0)},
              oset_entry{.element = elem("b2"), .order_key = {0x80}, .v = body_component(2.0)}})));
  const actor tool = make_actor("inner/tool", actor_kind::tool);
  // Two bodies, one diagnostic: one per (record, domain).
  CHECK(denied_domains(judge(base, create, tool, "content")) ==
        std::vector<std::string>{"sim.physics"});
  CHECK(denied_domains(judge(base, create, tool, "gov")) ==
        (std::vector<std::string>{"content.scene", "sim.physics"}));
  CHECK(judge(base, create, make_actor("inner/all", actor_kind::tool, {"write:sim"}), "content")
            .empty());
}
