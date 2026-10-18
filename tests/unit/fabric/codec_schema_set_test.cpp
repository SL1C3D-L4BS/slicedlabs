// FAB-INV-TY-04 (SL-ARCH-001 ARCH1-R22): the schema object id of every fixture type equals the lock
// hash tools/fabric/schemalib.py computes (tests/fixtures/fabric/schema/lock_vectors.json, from
// tests/fixtures/fabric/schema/modules), and the schema_set digest does not depend on
// registration order and equals SHA-256 over the sorted Python lock hashes.

#include "sl/fabric/codec.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <map>
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

struct lock_row {
  std::string hex;
  std::string hash;
};

std::map<std::string, lock_row> lock_vectors() {
  const std::string path = std::string(SL_FABRIC_FIXTURES_DIR) + "/schema/lock_vectors.json";
  std::ifstream in(path, std::ios::binary);
  std::ostringstream s;
  s << in.rdbuf();
  const json_value doc = must(parse_json(s.str()));
  std::map<std::string, lock_row> out;
  for (const json_value &v : doc.find("vectors")->items) {
    out[v.find("type")->text] = lock_row{.hex = v.find("hex")->text, .hash = v.find("hash")->text};
  }
  return out;
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

type_desc base(std::string_view name, type_kind kind, std::string_view domain) {
  type_desc t;
  t.name = std::string(name);
  t.id = type_id_of(name);
  t.kind = kind;
  t.domain = std::string(domain);
  t.state_class = state_class::document; // the fixture modules give every type a state class
  return t;
}

type_desc with_element(type_desc t, std::string_view element) {
  t.element = type_id_of(element);
  return t;
}

ref_spec refs(ref_strength s, std::string_view target) {
  return ref_spec{.strength = s, .targets = {type_id_of(target)}};
}

/// The twelve types of tests/fixtures/fabric/schema/modules, field for field.
std::vector<type_desc> fixture_types() {
  constexpr std::string_view shapes = "content.shapes";
  std::vector<type_desc> out;

  type_desc color = base("content.shapes.color", type_kind::enum_, shapes);
  color.enumerators = {{.no = 0, .name = "red", .doc = {}},
                       {.no = 1, .name = "green", .doc = {}},
                       {.no = 2, .name = "blue", .doc = {}}};
  color.doc = "Palette.";
  out.push_back(color);

  type_desc point = base("content.shapes.point", type_kind::struct_, shapes);
  point.fields = {fld(1, "x", "f64"), fld(2, "y", "f64")};
  out.push_back(point);

  out.push_back(
      with_element(base("content.shapes.points", type_kind::list, shapes), "content.shapes.point"));
  type_desc tags = with_element(base("content.shapes.tags", type_kind::map, shapes), "string");
  tags.key = type_id_of("string");
  out.push_back(tags);
  out.push_back(with_element(base("content.shapes.meters", type_kind::tagged, shapes), "f64"));

  type_desc shape = base("content.shapes.shape", type_kind::variant, shapes);
  shape.cases = {{.no = 1, .name = "circle", .type = type_id_of("f64"), .doc = {}},
                 {.no = 2, .name = "poly", .type = type_id_of("content.shapes.points"), .doc = {}}};
  out.push_back(shape);
  out.push_back(
      with_element(base("content.shapes.layers", type_kind::oset, shapes), "content.shapes.shape"));

  type_desc scene = base("content.shapes.scene", type_kind::struct_, shapes);
  scene.version = {.major = 1, .minor = 1};
  scene.record_class = id_class::entity;
  scene.document_root = true;
  scene.reserved = {4};
  scene.fields = {fld(1, "name", "string"), fld(2, "origin", "content.shapes.point")};
  scene.fields[0].doc = "Display label.";
  scene.fields.push_back(fld(3, "color", "content.shapes.color", presence::optional));
  scene.fields.back().default_value = value::uint(1);
  scene.fields.push_back(fld(5, "parent", "ref", presence::optional));
  scene.fields.back().ref = refs(ref_strength::weak, "content.shapes.scene");
  scene.fields.push_back(fld(6, "layers", "content.shapes.layers", presence::optional));
  scene.fields.push_back(fld(7, "tags", "content.shapes.tags", presence::optional));
  scene.fields.push_back(fld(8, "body", "sim.physics.body", presence::optional));
  scene.fields.push_back(fld(9, "radius", "content.shapes.meters", presence::optional));
  scene.fields.back().since_minor = 1;
  scene.fields.push_back(fld(10, "retries", "u32", presence::optional));
  scene.fields.back().default_value = value::uint(3);
  scene.fields.push_back(fld(11, "scale", "f32", presence::optional));
  scene.fields.back().default_value = must(value::f32(1.5F));
  scene.fields.push_back(fld(12, "visible", "bool", presence::optional));
  scene.fields.back().default_value = value::boolean(true);
  scene.doc = "A scene document root.";
  out.push_back(scene);

  constexpr std::string_view physics = "sim.physics";
  type_desc kind = base("sim.physics.body_kind", type_kind::enum_, physics);
  kind.enumerators = {{.no = 0, .name = "static", .doc = {}},
                      {.no = 1, .name = "dynamic", .doc = {}},
                      {.no = 2, .name = "kinematic", .doc = {}}};
  out.push_back(kind);

  type_desc body = base("sim.physics.body", type_kind::struct_, physics);
  body.fields = {fld(1, "mass", "f32"), fld(2, "kind", "sim.physics.body_kind", presence::optional),
                 fld(3, "drag", "f64", presence::optional)};
  body.fields[1].default_value = value::uint(0);
  body.fields[2].deprecated_in = 0;
  body.fields[2].doc = "Deprecated field still decodes.";
  out.push_back(body);

  type_desc cache = base("sim.physics.contact_cache", type_kind::struct_, physics);
  cache.record_class = id_class::derived;
  cache.state_class = state_class::derived;
  cache.fields = {fld(1, "scene", "ref"), fld(2, "contacts", "u32"),
                  fld(3, "delta", "i16", presence::optional)};
  cache.fields[0].ref = refs(ref_strength::strong, "content.shapes.scene");
  cache.fields[2].default_value = value::integer(-5);
  out.push_back(cache);

  type_desc capture = base("runtime.capture.frame_capture", type_kind::struct_, "runtime.capture");
  capture.record_class = id_class::event;
  capture.state_class = state_class::observation;
  capture.fields = {fld(1, "tick", "u64"),
                    fld(2, "frame_ms", "f32"),
                    fld(3, "digest", "digest", presence::optional),
                    fld(4, "blob", "blob_ref", presence::optional),
                    fld(5, "source", "external_ref", presence::optional),
                    fld(6, "scene", "ref", presence::optional)};
  capture.fields[5].ref = refs(ref_strength::owned, "content.shapes.scene");
  out.push_back(capture);
  return out;
}

schema_registry registry_of(const std::vector<type_desc> &types) {
  schema_registry reg;
  for (const type_desc &t : types) {
    REQUIRE_MESSAGE(reg.add(t).has_value(), t.name);
  }
  return reg;
}

} // namespace

TEST_CASE("FAB-INV-TY-04: schema object ids equal the Python lock hashes") {
  const auto lock = lock_vectors();
  const auto types = fixture_types();
  REQUIRE(lock.size() == types.size());
  const schema_registry reg = registry_of(types);
  for (const type_desc &t : types) {
    CAPTURE(t.name);
    const auto row = lock.find(t.name);
    REQUIRE(row != lock.end());
    CHECK(to_hex(must(encode_type_def(t, reg))) == row->second.hex);
    CHECK("sha256:" + must(schema_object_id(t, reg)).to_hex() == row->second.hash);
  }
  // Without a registry only the type itself and the built-ins resolve.
  const type_desc &point = types[1];
  CHECK(must(encode_type_def(point)) == must(encode_type_def(point, reg)));
  const auto scene = schema_object_id(types[7]);
  REQUIRE_FALSE(scene.has_value());
  CHECK(scene.error().code == errc::schema_unknown);
}

TEST_CASE("FAB-INV-TY-04: the schema_set digest is independent of registration order") {
  const auto lock = lock_vectors();
  auto types = fixture_types();
  const digest forward = must(schema_set_digest(registry_of(types)));
  std::ranges::reverse(types);
  CHECK(must(schema_set_digest(registry_of(types))) == forward);
  std::ranges::rotate(types, types.begin() + 5);
  CHECK(must(schema_set_digest(registry_of(types))) == forward);

  // SHA-256 over the sorted 33-byte object ids of the Python lock.
  std::vector<std::vector<std::uint8_t>> ids;
  for (const auto &[name, row] : lock) {
    std::vector<std::uint8_t> id = {0x01};
    const auto raw = must(from_hex(std::string_view(row.hash).substr(7)));
    id.insert(id.end(), raw.begin(), raw.end());
    ids.push_back(std::move(id));
  }
  std::ranges::sort(ids);
  sha256 h;
  for (const auto &id : ids) {
    h.update(id);
  }
  CHECK(forward.algo == hash_algo::sha256);
  CHECK(forward.bytes == h.finish());

  // Built-in types are not part of the set; any change to a user type changes the digest.
  CHECK(must(schema_set_digest(schema_registry{})).bytes == sha256::hash(byte_span{}));
  auto edited = fixture_types();
  edited[1].fields[0].doc = "x coordinate";
  CHECK(must(schema_set_digest(registry_of(edited))) != forward);
  edited = fixture_types();
  edited.pop_back();
  CHECK(must(schema_set_digest(registry_of(edited))) != forward);
}
