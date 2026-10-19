// The domain fixture registry of the conformance suite (WO-062): see conformance_registry.hpp.

#include "conformance_registry.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/gen/fabric.identity.gen.hpp"
#include "sl/fabric/gen/system.gen.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/migration.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/provenance.hpp"
#include "sl/fabric/resolver.hpp"
#include "sl/fabric/schema.hpp"

#include "test_schemas.hpp"

namespace sl::fabric::testing {

namespace {

void write_string(std::string &out, std::string_view s) {
  out += '"';
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (c < 0x20) {
        out += std::format("\\u{:04x}", c);
      } else {
        out += ch;
      }
    }
  }
  out += '"';
}

void write_compact(std::string &out, const json_value &j) {
  switch (j.kind) {
  case json_value::kind_type::null:
    out += "null";
    break;
  case json_value::kind_type::boolean:
    out += j.boolean ? "true" : "false";
    break;
  case json_value::kind_type::number:
    out += j.text;
    break;
  case json_value::kind_type::string:
    write_string(out, j.text);
    break;
  case json_value::kind_type::array:
    out += '[';
    for (std::size_t i = 0; i < j.items.size(); ++i) {
      out += i == 0 ? "" : ",";
      write_compact(out, j.items[i]);
    }
    out += ']';
    break;
  case json_value::kind_type::object:
    out += '{';
    for (std::size_t i = 0; i < j.members.size(); ++i) {
      out += i == 0 ? "" : ",";
      write_string(out, j.members[i].key);
      out += ':';
      write_compact(out, j.members[i].v);
    }
    out += '}';
    break;
  }
}

result<json_value> read_json(const std::string &path) {
  auto text = read_text_file(path);
  if (!text) {
    return std::unexpected(text.error());
  }
  auto doc = parse_json(*text);
  if (!doc) {
    return sl::make_error(doc.error().code, path + ": " + doc.error().message);
  }
  return doc;
}

digest digest_of(std::string_view text) noexcept {
  digest d;
  d.bytes = sha256::hash(text);
  return d;
}

std::string digest_text_of(std::string_view text) { return "sha256:" + digest_of(text).to_hex(); }

// ---- fabric: the generated system modules -------------------------------------------------

result<domain_fixtures> system_domain(std::string_view fixtures_dir) {
  domain_fixtures d;
  d.name = "fabric";
  d.add_types = [](schema_registry &reg) { return gen::register_system_types(reg); };
  d.claim = [](domain_registry &reg) { return claim_fabric_domains(reg); };
  d.prerequisites_subsystem = "02";
  for (const gen::type_entry &entry : gen::system_types()) {
    const std::string_view name = entry.name;
    std::optional<json_value> doc;
    std::string tried;
    for (std::size_t dot = name.rfind('.'); dot != std::string_view::npos && dot > 0;
         dot = name.rfind('.', dot - 1)) {
      const std::string path = std::format("{}/types/{}/{}.json", fixtures_dir, name.substr(0, dot),
                                           name.substr(dot + 1));
      tried = path;
      if (auto j = read_json(path); j) {
        doc = std::move(*j);
        break;
      }
    }
    if (!doc) {
      return sl::make_error(errc::not_found,
                            std::format("no fixture file for {} (last tried {})", name, tried));
    }
    const json_value *values = doc->find("values");
    if (values == nullptr || values->kind != json_value::kind_type::array) {
      return sl::make_error(errc::invalid_argument,
                            std::format("{}: fixture file has no values array", name));
    }
    type_fixtures t{.type = std::string(name), .values = {}};
    for (const json_value &v : values->items) {
      t.values.push_back(compact_json(v));
    }
    d.types.push_back(std::move(t));
  }
  // An import record lives at import_record_id_for(system, locator) (WO-060, ARCH1-R65).
  d.record_id_for = [](const type_desc &type, std::size_t index,
                       const value &body) -> result<record_id> {
    if (type.name == "fabric.identity.import_record") {
      auto rec = gen::fabric_identity::import_record::from_value(body);
      if (!rec) {
        return std::unexpected(rec.error());
      }
      return import_record_id_for(rec->system, rec->locator);
    }
    return derive(type.record_class, well_known(id_class::namespace_, 1),
                  std::format("conformance/{}/{}", type.name, index));
  };
  return d;
}

// ---- content.scene: the test schemas, the Volume 05+ onboarding example -------------------

record_id conformance_mesh() noexcept { return test_id(id_class::asset, "conformance/mesh"); }

domain_fixtures scene_domain() {
  namespace tt = test_types;
  const std::string ast = conformance_mesh().to_string();
  const std::string ent = test_id(id_class::entity, "conformance/ref/e1").to_string();
  const std::string elm1 = test_id(id_class::element, "conformance/slot/1").to_string();
  const std::string elm2 = test_id(id_class::element, "conformance/slot/2").to_string();
  const std::string elm3 = test_id(id_class::element, "conformance/slot/3").to_string();
  const std::string vec_a = R"({"x":1.0,"y":2.0,"z":3.0})";
  const std::string vec_b = R"({"x":0.0,"y":-1.5,"z":4.25})";

  domain_fixtures d;
  d.name = "content.scene";
  d.add_types = [](schema_registry &reg) { return add_test_schemas(reg); };
  d.claim = [](domain_registry &reg) -> result<void> {
    for (domain_spec spec : {domain_spec{.name = "content.scene",
                                         .owner = subsystem::content,
                                         .state_classes = {state_class::document},
                                         .types = {}},
                             domain_spec{.name = "content.mesh",
                                         .owner = subsystem::content,
                                         .state_classes = {state_class::document},
                                         .types = {}},
                             domain_spec{.name = "asset.cooked",
                                         .owner = subsystem::asset,
                                         .state_classes = {state_class::derived},
                                         .types = {}}}) {
      if (auto ok = reg.claim(std::move(spec)); !ok) {
        return ok;
      }
    }
    return {};
  };
  d.types = {
      {.type = std::string(tt::meters), .values = {"2.5", "100.0"}},
      {.type = std::string(tt::vec3), .values = {vec_a, vec_b}},
      {.type = std::string(tt::layer), .values = {R"("world")", R"("debug")"}},
      {.type = std::string(tt::transform),
       .values = {std::format(R"({{"position":{}}})", vec_a),
                  std::format(R"({{"position":{},"scale":2.0,"radius":0.5}})", vec_b)}},
      {.type = std::string(tt::mesh_renderer),
       .values = {std::format(R"({{"mesh":"{}"}})", ast),
                  std::format(R"({{"mesh":"{}","cast_shadows":false}})", ast)}},
      {.type = std::string(tt::tag), .values = {R"({"label":"hero"})", R"({"label":""})"}},
      {.type = std::string(tt::component),
       .values = {std::format(R"({{"transform":{{"position":{}}}}})", vec_a),
                  R"({"tag":{"label":"hero"}})"}},
      {.type = std::string(tt::components),
       .values = {"[]", std::format(
                            R"([{{"id":"{}","order":"40","value":{{"tag":{{"label":"hero"}}}}}}])",
                            elm1)}},
      {.type = std::string(tt::entity_list),
       .values = {"[]", std::format(R"([{{"id":"{}","order":"40","value":"{}"}}])", elm2, ent)}},
      {.type = std::string(tt::properties), .values = {"[]", R"([["owner","tests"]])"}},
      {.type = std::string(tt::lods), .values = {"[]", "[1.0,0.5]"}},
      {.type = std::string(tt::entity),
       .values = {R"({"name":"hero","components":[]})",
                  std::format(
                      R"({{"name":"foe","layer":"ui","components":[{{"id":"{}","order":"80",)"
                      R"("value":{{"tag":{{"label":"foe"}}}}}}],"properties":[["owner","tests"]],)"
                      R"("lods":[1.0]}})",
                      elm3)}},
      {.type = std::string(tt::scene),
       .values = {R"({"name":"level","entities":[]})",
                  R"({"name":"arena","entities":[],"gravity":{"x":0.0,"y":-9.8,"z":0.0}})"}},
      {.type = std::string(tt::mesh),
       .values = {R"({"source":{"system":"gltf","locator":"meshes/rock.gltf"},"vertex_count":24})",
                  std::format(R"({{"source":{{"system":"blender","locator":"art/tree.blend",)"
                              R"("content":"{}"}},"vertex_count":0}})",
                              digest_text_of("tree.blend"))}},
      {.type = std::string(tt::cooked_mesh),
       .values = {std::format(R"({{"source":"{}","payload":"{}"}})", ast,
                              digest_text_of("cooked/rock")),
                  std::format(R"({{"source":"{}","payload":"{}"}})", ast,
                              digest_text_of("cooked/rock/lod1"))}},
  };
  d.prerequisites_subsystem = "content";
  d.prerequisites = [](const schema_registry &reg) -> result<std::vector<patch_op>> {
    auto body = from_text(R"({"source":{"system":"gltf","locator":"meshes/conformance.gltf"},)"
                          R"("vertex_count":8})",
                          type_id_of(test_types::mesh), reg);
    if (!body) {
      return std::unexpected(body.error());
    }
    return std::vector<patch_op>{
        op_create(conformance_mesh(), type_id_of(test_types::mesh), std::move(*body))};
  };
  d.provenance_for = [](const type_desc &type) -> std::optional<provenance> {
    if (type.state_class != state_class::derived) {
      return std::nullopt;
    }
    provenance p;
    p.input_ids = std::vector<record_id>{conformance_mesh()};
    p.input_revisions = std::vector<digest>{digest_of("conformance/mesh/revision")};
    p.toolchain_id = "sl.cook/1";
    p.parameters_digest = digest_of("conformance/parameters");
    p.environment_hash = digest_of("conformance/environment");
    return p;
  };
  return d;
}

// ---- content.demo: the WO-057 migration fixture module -------------------------------------

result<std::vector<type_desc>> module_types(const std::string &path) {
  auto doc = read_json(path);
  if (!doc) {
    return std::unexpected(doc.error());
  }
  return types_from_module(*doc);
}

result<domain_fixtures> demo_domain(std::string_view fixtures_dir) {
  const std::string dir = std::format("{}/migration", fixtures_dir);
  auto store = read_json(dir + "/store.json");
  if (!store) {
    return std::unexpected(store.error());
  }
  const json_value *v2 = store->find("v2");
  const json_value *records = v2 != nullptr ? v2->find("records") : nullptr;
  if (records == nullptr) {
    return sl::make_error(errc::invalid_argument, dir + "/store.json: no v2 records");
  }
  domain_fixtures d;
  d.name = "content.demo";
  const std::string v2_path = dir + "/content.demo.v2.sltype.json";
  const std::string v1_path = dir + "/content.demo.v1.sltype.json";
  d.add_types = [v2_path](schema_registry &reg) -> result<void> {
    auto types = module_types(v2_path);
    if (!types) {
      return std::unexpected(types.error());
    }
    for (type_desc &t : *types) {
      if (auto ok = reg.add(std::move(t)); !ok) {
        return ok;
      }
    }
    return {};
  };
  d.claim = [](domain_registry &reg) {
    return reg.claim(domain_spec{.name = "content.demo",
                                 .owner = subsystem::content,
                                 .state_classes = {state_class::document},
                                 .types = {}});
  };
  for (const json_value &r : records->items) {
    const json_value *type = r.find("type");
    const json_value *body = r.find("body");
    if (type == nullptr || body == nullptr) {
      return sl::make_error(errc::invalid_argument,
                            dir + "/store.json: a v2 record lacks type or body");
    }
    auto it =
        std::ranges::find_if(d.types, [&](const type_fixtures &t) { return t.type == type->text; });
    if (it == d.types.end()) {
      d.types.push_back(type_fixtures{.type = type->text, .values = {}});
      it = d.types.end() - 1;
    }
    it->values.push_back(body->text);
  }
  d.previous_types = [v1_path] { return module_types(v1_path); };
  return d;
}

} // namespace

std::string compact_json(const json_value &j) {
  std::string out;
  write_compact(out, j);
  return out;
}

result<std::string> read_text_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.good()) {
    return sl::make_error(errc::not_found, path + ": cannot be read");
  }
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

result<void> domain_fixture_registry::add(domain_fixtures d) {
  if (d.name.empty() || !d.add_types || !d.claim) {
    return sl::make_error(errc::invalid_argument,
                          "a domain needs a name, an add_types hook and a claim hook");
  }
  std::set<std::string> seen;
  for (const domain_fixtures &other : domains_) {
    if (other.name == d.name) {
      return sl::make_error(errc::already_exists, "domain " + d.name + " is registered already");
    }
    for (const type_fixtures &t : other.types) {
      seen.insert(t.type);
    }
  }
  for (const type_fixtures &t : d.types) {
    if (t.values.size() < 2) {
      return sl::make_error(errc::invalid_argument,
                            std::format("{}: type {} has {} fixture(s), needs two or more", d.name,
                                        t.type, t.values.size()));
    }
    if (!seen.insert(t.type).second) {
      return sl::make_error(errc::already_exists,
                            "fixtures of " + t.type + " are registered twice");
    }
  }
  domains_.push_back(std::move(d));
  return {};
}

std::size_t domain_fixture_registry::type_count() const noexcept {
  std::size_t n = 0;
  for (const domain_fixtures &d : domains_) {
    n += d.types.size();
  }
  return n;
}

result<domain_fixture_registry> builtin_fixture_registry(std::string_view fixtures_dir) {
  domain_fixture_registry reg;
  auto system = system_domain(fixtures_dir);
  if (!system) {
    return std::unexpected(system.error());
  }
  auto demo = demo_domain(fixtures_dir);
  if (!demo) {
    return std::unexpected(demo.error());
  }
  for (domain_fixtures *d : {&*system, &*demo}) {
    if (auto ok = reg.add(std::move(*d)); !ok) {
      return std::unexpected(ok.error());
    }
  }
  if (auto ok = reg.add(scene_domain()); !ok) {
    return std::unexpected(ok.error());
  }
  return reg;
}

} // namespace sl::fabric::testing
