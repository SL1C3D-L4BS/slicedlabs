// FAB-INV-OWN-07 (SL-ARCH-001 ARCH1-R40): agent and service actors are denied by default; an
// explicit grant allows.
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

TEST_CASE("FAB-INV-OWN-07: agent and service actors are denied by default, even via the owner") {
  CHECK(denied_by_default(actor_kind::agent));
  CHECK(denied_by_default(actor_kind::service));
  CHECK_FALSE(denied_by_default(actor_kind::human));
  CHECK_FALSE(denied_by_default(actor_kind::tool));
  CHECK_FALSE(denied_by_default(actor_kind::plugin));
  for (const actor_kind k : {actor_kind::human, actor_kind::tool, actor_kind::plugin,
                             actor_kind::agent, actor_kind::service}) {
    CHECK(actor_kind_from_string(to_string(k)) == k);
  }
  CHECK_FALSE(actor_kind_from_string("robot").has_value());

  const domain_registry domains = scene_domains();
  for (const actor_kind k : {actor_kind::agent, actor_kind::service}) {
    CAPTURE(to_string(k));
    const auto r = check_write(domains, make_actor("deny/x", k), "05", "content.scene");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == sl::errc::authority_denied);
    CHECK(r.error().message.contains("denied by default"));
    // An accept grant is not a write grant.
    CHECK(check_write(domains, make_actor("deny/y", k, {"accept:content.scene"}), "05",
                      "content.scene")
              .error()
              .code == sl::errc::authority_denied);
    // An explicit grant allows, through any subsystem.
    CHECK(check_write(domains, make_actor("deny/z", k, {"write:content.scene"}), "12",
                      "content.scene")
              .has_value());
  }
  for (const actor_kind k : {actor_kind::human, actor_kind::tool, actor_kind::plugin}) {
    CHECK(check_write(domains, make_actor("allow/x", k), "05", "content.scene").has_value());
  }
}

TEST_CASE("FAB-INV-OWN-07: the validator denies an ungranted agent and allows a granted one") {
  const tg::oracle_store base = fixture();
  const patch rename = single(op_set(hero, {path_segment::field(1)}, must(value::string("a"))));
  const actor agent = make_actor("deny/agent", actor_kind::agent);
  const auto ds = judge(base, rename, agent, "content");
  REQUIRE(ds.size() == 1);
  CHECK(ds[0].rule == "R-AUTH-DENIED");
  CHECK(ds[0].message.contains("(agent) is denied by default"));
  CHECK(judge(base, rename, make_actor("deny/svc", actor_kind::service), "gov").size() == 1);
  CHECK(
      judge(base, rename, make_actor("deny/ok", actor_kind::agent, {"write:content.scene"}), "gov")
          .empty());
  // A grant covers only its domain: a body inside still needs sim.
  const patch add_body =
      single(op_insert(hero, {path_segment::field(2)}, elem("b9"), {0xC0}, body_component(3.0)));
  CHECK(denied_domains(judge(base, add_body,
                             make_actor("deny/half", actor_kind::agent, {"write:content"}),
                             "content")) == std::vector<std::string>{"sim.physics"});
  CHECK(judge(base, add_body,
              make_actor("deny/full", actor_kind::agent, {"write:content", "write:sim"}), "content")
            .empty());
}
