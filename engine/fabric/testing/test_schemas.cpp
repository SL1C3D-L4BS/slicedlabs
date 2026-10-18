// The scene/entity/component test module set (WO-039). See test_schemas.hpp.

#include "test_schemas.hpp"

#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "sl/fabric/type.hpp"

namespace sl::fabric::testing {

namespace {

type_desc make_type(std::string_view name, type_kind kind) {
  type_desc t;
  t.name = std::string(name);
  t.id = type_id_of(name);
  t.kind = kind;
  t.domain = std::string(name.substr(0, name.rfind('.')));
  return t;
}

field_desc make_field(std::uint32_t no, std::string_view name, std::string_view type,
                      presence p = presence::required) {
  field_desc f;
  f.no = no;
  f.name = std::string(name);
  f.type = type_id_of(type);
  f.presence = p;
  return f;
}

field_desc ref_field(std::uint32_t no, std::string_view name, std::string_view type,
                     ref_strength strength, std::string_view target,
                     presence p = presence::required) {
  field_desc f = make_field(no, name, type, p);
  f.ref = ref_spec{.strength = strength, .targets = {type_id_of(target)}};
  return f;
}

type_desc record(std::string_view name, id_class cls, state_class sc, bool root) {
  type_desc t = make_type(name, type_kind::struct_);
  t.record_class = cls;
  t.state_class = sc;
  t.document_root = root;
  return t;
}

type_desc of_element(std::string_view name, type_kind kind, std::string_view element) {
  type_desc t = make_type(name, kind);
  t.element = type_id_of(element);
  return t;
}

result<std::vector<type_desc>> test_module() {
  namespace tt = test_types;
  std::vector<type_desc> out;

  out.push_back(of_element(tt::meters, type_kind::tagged, "f64"));

  type_desc vec3 = make_type(tt::vec3, type_kind::struct_);
  vec3.fields = {make_field(1, "x", "f64"), make_field(2, "y", "f64"), make_field(3, "z", "f64")};
  out.push_back(std::move(vec3));

  type_desc layer = make_type(tt::layer, type_kind::enum_);
  layer.enumerators = {enumerator_desc{.no = 0, .name = "world", .doc = ""},
                       enumerator_desc{.no = 1, .name = "ui", .doc = ""},
                       enumerator_desc{.no = 2, .name = "debug", .doc = ""}};
  out.push_back(std::move(layer));

  auto one = value::f64(1.0);
  if (!one) {
    return std::unexpected(std::move(one.error()));
  }
  type_desc transform = make_type(tt::transform, type_kind::struct_);
  transform.fields = {make_field(1, "position", tt::vec3),
                      make_field(2, "scale", "f64", presence::optional),
                      make_field(3, "radius", tt::meters, presence::optional)};
  transform.fields[1].default_value = *one;
  out.push_back(std::move(transform));

  type_desc renderer = make_type(tt::mesh_renderer, type_kind::struct_);
  renderer.fields = {ref_field(1, "mesh", "ref", ref_strength::strong, tt::mesh),
                     make_field(2, "cast_shadows", "bool", presence::optional)};
  renderer.fields[1].default_value = value::boolean(true);
  out.push_back(std::move(renderer));

  type_desc tag = make_type(tt::tag, type_kind::struct_);
  tag.fields = {make_field(1, "label", "string")};
  out.push_back(std::move(tag));

  type_desc component = make_type(tt::component, type_kind::variant);
  component.cases = {
      case_desc{.no = 1, .name = "transform", .type = type_id_of(tt::transform), .doc = ""},
      case_desc{.no = 2, .name = "mesh_renderer", .type = type_id_of(tt::mesh_renderer), .doc = ""},
      case_desc{.no = 3, .name = "tag", .type = type_id_of(tt::tag), .doc = ""}};
  out.push_back(std::move(component));

  out.push_back(of_element(tt::components, type_kind::oset, tt::component));
  out.push_back(of_element(tt::entity_list, type_kind::oset, "ref"));
  type_desc props = of_element(tt::properties, type_kind::map, "string");
  props.key = type_id_of("string");
  out.push_back(std::move(props));
  out.push_back(of_element(tt::lods, type_kind::list, "f32"));

  type_desc entity = record(tt::entity, id_class::entity, state_class::document, false);
  entity.fields = {
      make_field(1, "name", "string"),
      make_field(2, "layer", tt::layer, presence::optional),
      ref_field(3, "parent", "ref", ref_strength::weak, tt::entity, presence::optional),
      make_field(4, "components", tt::components),
      make_field(5, "properties", tt::properties, presence::optional),
      make_field(6, "lods", tt::lods, presence::optional)};
  entity.fields[1].default_value = value::uint(0);
  entity.reserved = {7};
  out.push_back(std::move(entity));

  type_desc scene = record(tt::scene, id_class::entity, state_class::document, true);
  scene.fields = {make_field(1, "name", "string"),
                  ref_field(2, "entities", tt::entity_list, ref_strength::owned, tt::entity),
                  make_field(3, "gravity", tt::vec3, presence::optional)};
  out.push_back(std::move(scene));

  type_desc mesh = record(tt::mesh, id_class::asset, state_class::document, true);
  mesh.fields = {make_field(1, "source", "external_ref"), make_field(2, "vertex_count", "u32"),
                 make_field(3, "content", "blob_ref", presence::optional)};
  out.push_back(std::move(mesh));

  type_desc cooked = record(tt::cooked_mesh, id_class::derived, state_class::derived, false);
  cooked.fields = {ref_field(1, "source", "ref", ref_strength::strong, tt::mesh),
                   make_field(2, "payload", "blob_ref")};
  out.push_back(std::move(cooked));
  return out;
}

/// Unwraps a value or forwards the error.
#define SL_TS_TRY(var, expr)                                                                       \
  auto var = (expr); /* NOLINT(bugprone-macro-parentheses): var is a declarator */                 \
  if (!(var)) {                                                                                    \
    return std::unexpected(std::move((var).error()));                                              \
  }

} // namespace

result<void> add_test_schemas(schema_registry &reg) {
  SL_TS_TRY(types, test_module());
  for (type_desc &t : *types) {
    if (auto ok = reg.add(std::move(t)); !ok) {
      return ok;
    }
  }
  return {};
}

result<schema_registry> make_test_registry() {
  schema_registry reg;
  if (auto ok = add_test_schemas(reg); !ok) {
    return std::unexpected(std::move(ok.error()));
  }
  if (auto ok = reg.freeze(); !ok) {
    return std::unexpected(std::move(ok.error()));
  }
  return reg;
}

record_id test_id(id_class c, std::string_view key) noexcept {
  return derive(c, well_known(id_class::namespace_, 1), key);
}

result<value> make_vec3(double x, double y, double z) {
  SL_TS_TRY(vx, value::f64(x));
  SL_TS_TRY(vy, value::f64(y));
  SL_TS_TRY(vz, value::f64(z));
  return value::struct_(
      {field{.no = 1, .v = *vx}, field{.no = 2, .v = *vy}, field{.no = 3, .v = *vz}});
}

result<value> sample_entity(std::string_view name) {
  SL_TS_TRY(position, make_vec3(1.0, 2.0, 3.0));
  SL_TS_TRY(transform, value::struct_({field{.no = 1, .v = *position}}));
  SL_TS_TRY(transform_case, value::variant(1, *transform));
  SL_TS_TRY(label, value::string(name));
  SL_TS_TRY(tag, value::struct_({field{.no = 1, .v = *label}}));
  SL_TS_TRY(tag_case, value::variant(3, *tag));
  SL_TS_TRY(
      components,
      value::oset(
          {oset_entry{.element =
                          test_id(id_class::element, std::format("{}/transform", name)).bytes(),
                      .order_key = {0x40},
                      .v = *transform_case},
           oset_entry{.element = test_id(id_class::element, std::format("{}/tag", name)).bytes(),
                      .order_key = {0x80},
                      .v = *tag_case}}));
  SL_TS_TRY(key, value::string("owner"));
  SL_TS_TRY(owner, value::string("tests"));
  SL_TS_TRY(props, value::map({map_entry{.key = *key, .v = *owner}}));
  return value::struct_(
      {field{.no = 1, .v = *label}, field{.no = 4, .v = *components}, field{.no = 5, .v = *props}});
}

result<value> sample_scene(std::string_view name, unsigned count) {
  std::vector<oset_entry> entities;
  std::vector<std::uint8_t> key;
  for (unsigned i = 0; i < count; ++i) {
    SL_TS_TRY(next, order_key_between(key, {}));
    key = *next;
    const record_id element = test_id(id_class::element, std::format("{}/slot{}", name, i));
    entities.push_back(
        oset_entry{.element = element.bytes(),
                   .order_key = key,
                   .v = value::id(test_id(id_class::entity, std::format("{}/e{}", name, i)))});
  }
  SL_TS_TRY(label, value::string(name));
  SL_TS_TRY(list, value::oset(std::move(entities)));
  return value::struct_({field{.no = 1, .v = *label}, field{.no = 2, .v = *list}});
}

#undef SL_TS_TRY

} // namespace sl::fabric::testing
