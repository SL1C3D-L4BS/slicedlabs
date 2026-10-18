// FAB-INV-OWN-05 (SL-ARCH-001 ARCH1-R36): the authority validator denies a non-owner write
// with authority_denied.
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

TEST_CASE("FAB-INV-OWN-05: a write through a subsystem that does not own the domain is denied") {
  const domain_registry domains = scene_domains();
  const actor tool = make_actor("grant/tool", actor_kind::tool);
  CHECK(check_write(domains, tool, "05", "content.scene").has_value());
  CHECK(check_write(domains, tool, "content", "content.scene").has_value());
  const auto r = check_write(domains, tool, "06", "content.scene");
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == sl::errc::authority_denied);
  CHECK(r.error().message.contains("owner 05 content"));
  CHECK(r.error().message.contains("write:content.scene"));
  CHECK(check_write(domains, tool, "", "content.scene").error().code == sl::errc::authority_denied);
  // A grant for the domain or an ancestor allows; a sibling's does not.
  CHECK(check_write(domains, make_actor("g/a", actor_kind::tool, {"write:content.scene"}), "06",
                    "content.scene")
            .has_value());
  CHECK(check_write(domains, make_actor("g/b", actor_kind::tool, {"write:content"}), "06",
                    "content.scene.lights")
            .has_value());
  CHECK(check_write(domains, make_actor("g/c", actor_kind::tool, {"write:content.mesh"}), "06",
                    "content.scene")
            .error()
            .code == sl::errc::authority_denied);
  CHECK(check_write(domains, make_actor("g/d", actor_kind::tool, {"accept:content.scene"}), "06",
                    "content.scene")
            .error()
            .code == sl::errc::authority_denied);
  // No owner: orphan, whoever writes.
  CHECK(check_write(domains, tool, "05", "gov.audit").error().code == sl::errc::ownership_orphan);
}

TEST_CASE("FAB-INV-OWN-05: the validator reports authority_denied for a non-owner record write") {
  const tg::oracle_store base = fixture();
  const actor tool = make_actor("grant/tool", actor_kind::tool);
  const patch rename = single(op_set(hero, {path_segment::field(1)}, must(value::string("x"))));
  CHECK(judge(base, rename, tool, "content").empty());
  const auto ds = judge(base, rename, tool, "sim");
  REQUIRE(ds.size() == 1);
  CHECK(ds[0].rule == "R-AUTH-DENIED");
  CHECK(ds[0].record == hero);
  CHECK(ds[0].path.empty());
  CHECK(ds[0].message.contains("may not write content.scene"));
  // create and delete need the record's domain (and the body's nested domains) as well.
  const record_id other = tg::test_id(id_class::entity, "grant/other");
  const patch create = single(op_create(other, entity_type, entity("other", {})));
  CHECK(denied_domains(judge(base, create, tool, "sim")) ==
        std::vector<std::string>{"content.scene"});
  CHECK(judge(base, create, tool, "content").empty());
  const patch del = single(op_delete(hero));
  CHECK(denied_domains(judge(base, del, tool, "content")) ==
        std::vector<std::string>{"sim.physics"});
  // Annotations write nothing.
  CHECK(judge(base, single(op_annotate(must(value::string("note")))), tool, "sim").empty());
}

TEST_CASE("FAB-INV-OWN-05: actors, grants and the validator registry") {
  actor_directory dir;
  CHECK(dir.add(make_actor("dir/a", actor_kind::human, {"write:content.scene", "accept:sim"}))
            .has_value());
  CHECK(dir.add(make_actor("dir/a", actor_kind::tool)).error().code == sl::errc::already_exists);
  for (const std::string_view bad :
       {"write:", "write:ui.hud", "read:content", "write:content.", "accept", "writecontent"}) {
    CAPTURE(bad);
    CHECK_FALSE(is_valid_grant(bad));
    CHECK(dir.add(make_actor("dir/bad", actor_kind::human, {std::string(bad)})).error().code ==
          sl::errc::invalid_argument);
  }
  CHECK(dir.add(actor{}).error().code == sl::errc::invalid_argument);
  CHECK(dir.add(make_actor("dir/k", static_cast<actor_kind>(9))).error().code ==
        sl::errc::invalid_argument);
  CHECK(dir.size() == 1);
  CHECK(dir.find(make_actor("dir/a", actor_kind::human).id) != nullptr);
  // Accepting always needs an explicit accept grant (ARCH1-R42), even for a human owner.
  const domain_registry domains = scene_domains();
  const actor human = make_actor("acc/h", actor_kind::human, {"write:content.scene"});
  CHECK(check_accept(domains, human, "content.scene").error().code == sl::errc::authority_denied);
  CHECK(check_accept(domains, make_actor("acc/g", actor_kind::human, {"accept:content"}),
                     "content.scene")
            .has_value());
  CHECK(check_accept(domains, human, "gov.audit").error().code == sl::errc::ownership_orphan);

  // Through the validator registry: stage authority, unknown actors denied.
  const tg::oracle_store base = fixture();
  const schema_registry schemas = scene_schemas();
  validator_registry validators;
  must_ok(validators.add(std::make_shared<authority_validator>(domains, dir)));
  const patch p = must(
      normalize(single(op_set(hero, {path_segment::field(1)}, must(value::string("y")))), base));
  staged_state staged(base);
  must_ok(staged.apply(p));
  const actor stranger = make_actor("dir/stranger", actor_kind::human);
  const auto unknown = must(validators.run(validation_context(
      p, staged, schemas, edit_origin{.actor = stranger.id, .subsystem = "05"})));
  REQUIRE(unknown.size() == 1);
  CHECK(unknown[0].rule == "R-AUTH-ACTOR");
  CHECK(unknown[0].stage == validator_stage::authority);
  const auto known = must(validators.run(validation_context(
      p, staged, schemas,
      edit_origin{.actor = make_actor("dir/a", actor_kind::human).id, .subsystem = "06"})));
  CHECK(known.empty()); // write:content.scene
}
