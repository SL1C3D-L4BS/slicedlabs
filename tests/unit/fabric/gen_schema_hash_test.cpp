// FAB-INV-GEN-03 (SL-ARCH-001 ARCH1-R20, ADR-0013): every generated descriptor hashes, as a schema
// object (codec.hpp schema_object_id), to the lock hash tools/fabric/schemalib.py computes for its
// source definition (tests/fixtures/fabric/types/lock_hashes.json, written by tools/fabric/slgen.py
// and checked by `just fabric-gen-check`), and the inspector metadata carries the same hash.

#include "sl/fabric/codec.hpp"
#include "sl/fabric/gen/system.gen.hpp"

#include <doctest/doctest.h>

#include <format>
#include <fstream>
#include <map>
#include <set>
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

json_value load(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE_MESSAGE(in.good(), path);
  std::ostringstream s;
  s << in.rdbuf();
  return must(parse_json(s.str()));
}

std::string text(const json_value *v) { return v == nullptr ? std::string() : v->text; }

std::map<std::string, std::string> python_lock_hashes() {
  const json_value doc = load(std::string(SL_FABRIC_FIXTURES_DIR) + "/types/lock_hashes.json");
  REQUIRE(text(doc.find("format")) == "sl-gen-lock-hashes/1");
  std::map<std::string, std::string> out;
  for (const json_member &m : doc.find("hashes")->members) {
    out[m.key] = m.v.text;
  }
  return out;
}

/// type name -> lock_hash of engine/fabric/gen/meta/*.meta.json, for the modules of `names`.
std::map<std::string, std::string> meta_lock_hashes(const std::set<std::string> &modules) {
  std::map<std::string, std::string> out;
  for (const std::string &module : modules) {
    const json_value doc = load(std::format("{}/../../../engine/fabric/gen/meta/{}.meta.json",
                                            SL_FABRIC_FIXTURES_DIR, module));
    for (const json_value &t : doc.find("types")->items) {
      out[text(t.find("name"))] = text(t.find("lock_hash"));
    }
  }
  return out;
}

} // namespace

TEST_CASE("FAB-INV-GEN-03: generated descriptors hash to the Python lock hashes") {
  const schema_registry reg = must(gen::system_registry());
  const std::vector<gen::type_entry> types = gen::system_types();
  const auto lock = python_lock_hashes();
  REQUIRE(types.size() == lock.size());
  std::set<std::string> modules;
  for (const gen::type_entry &t : types) {
    CAPTURE(t.name);
    const auto row = lock.find(std::string(t.name));
    REQUIRE(row != lock.end());
    CHECK("sha256:" + must(gen::lock_hash(t, reg)).to_hex() == row->second);
    const std::string name(t.name);
    modules.insert(name.substr(0, name.find('.', name.find('.') + 1)));
  }
  const auto meta = meta_lock_hashes(modules);
  CHECK(meta == lock);
}

TEST_CASE("FAB-INV-GEN-03: descriptors keep the state class on record types only") {
  const schema_registry reg = must(gen::system_registry());
  for (const gen::type_entry &t : gen::system_types()) {
    CAPTURE(t.name);
    const type_desc *d = reg.find(t.name);
    REQUIRE(d != nullptr);
    CHECK(t.declared_state != state_class::none);
    if (d->record_class == id_class::none) {
      CHECK(d->state_class == state_class::none);
      // Without the declared state class the object id differs from the lock hash.
      CHECK(must(schema_object_id(*d, reg)) != must(gen::lock_hash(t, reg)));
    } else {
      CHECK(d->state_class == t.declared_state);
      CHECK(must(schema_object_id(*d, reg)) == must(gen::lock_hash(t, reg)));
    }
  }
}

TEST_CASE("FAB-INV-GEN-03: any edit changes the hash; an unregistered type is schema_unknown") {
  const schema_registry reg = must(gen::system_registry());
  const auto lock = python_lock_hashes();
  const gen::type_entry manifest = [] {
    for (const gen::type_entry &t : gen::system_types()) {
      if (t.name == "fabric.manifest.manifest") {
        return t;
      }
    }
    FAIL("fabric.manifest.manifest is not generated");
    return gen::type_entry{};
  }();
  type_desc edited = *reg.find(manifest.name);
  edited.fields.front().doc = "Edited doc.";
  CHECK("sha256:" + must(schema_object_id(edited, reg)).to_hex() !=
        lock.at("fabric.manifest.manifest"));

  const auto unknown = gen::lock_hash(gen::type_entry{.name = "fabric.none.missing"}, reg);
  REQUIRE_FALSE(unknown.has_value());
  CHECK(unknown.error().code == errc::schema_unknown);
}
