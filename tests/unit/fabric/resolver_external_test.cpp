// FAB-INV-REF-04 (SL-ARCH-001 ARCH1-R65; ADR-0011): an external reference (system, locator)
// resolves only through an import record: the record at import_record_id_for(system, locator)
// whose record, an imported-scheme id, then resolves through the usual order (same document,
// project, pins) and the resolution carries the import record. With no import record the external
// reference is missing even when a record sits at the imported id the importer would have used
// (no shortcut); a retired import record is retired; the target's own outcomes still apply
// (missing, retired, wrong_type). A record at the import record id that is not an import record of
// that system and locator is object_corrupt. Inside a package the import record and the target
// come from the pinned revision; an unpinned package is package_missing. A plain id reference to
// an imported id still resolves directly.

#include "sl/fabric/digest.hpp"
#include "sl/fabric/gen/fabric.identity.gen.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/identity.hpp"
#include "sl/fabric/manifest.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/resolver.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/value.hpp"

#include "../../../engine/fabric/testing/tree_fixture.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;
namespace tg = sl::fabric::testing;
using sl::errc;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

value body(std::uint64_t n) { return must(value::struct_({field{.no = 1, .v = value::uint(n)}})); }
type_id rec(int i) { return type_id_of(std::format("gen.rec{}", i)); }
type_id import_type() { return type_id_of("fabric.identity.import_record"); }
record_id named(id_class c, std::string_view name) {
  return derive(c, well_known(id_class::namespace_, 1), name);
}
record_id pkg(std::string_view name) { return named(id_class::package, name); }

struct world {
  schema_registry reg;
  memory_backend store;
  std::uint32_t tx = 1;

  world() {
    REQUIRE(tg::add_tree_record_types(reg).has_value());
    REQUIRE(gen::fabric_identity::register_types(reg).has_value());
  }
  [[nodiscard]] snapshot empty() const { return must(snapshot::empty(store)); }
  snapshot build(const snapshot &base, const std::function<void(revision_builder &)> &edit) {
    revision_builder b(base, reg);
    edit(b);
    const built_revision built = must(b.build(tg::tree_inputs("resolver", tx++)));
    REQUIRE(tg::commit_built(store, built).has_value());
    return must(snapshot::open(store, built.revision));
  }
};

void put(revision_builder &b, const record_id &id, const type_id &t, value v,
         const record_id &doc = {}) {
  REQUIRE(
      b.put(id,
            record_write{.type = t, .document = doc, .body = std::move(v), .version = std::nullopt})
          .has_value());
}

digest digest_of(std::string_view text) {
  digest d;
  d.bytes = sha256::hash(text);
  return d;
}

// The import record of file `locator` as an entity, filed at import_record_id_for("file", locator).
import_record import_of(std::string_view locator) {
  return must(make_import_record(id_class::entity, "file", locator, locator, digest_of(locator)));
}
void put_import(revision_builder &b, const import_record &ir, std::string_view locator) {
  put(b, import_record_id_for("file", locator), import_type(), must(ir.to_value()));
}

class map_provider final : public package_provider {
public:
  std::map<record_id, const store_backend *> stores;

  sl::result<snapshot> open(const record_id &package, const digest &revision) override {
    const auto it = stores.find(package);
    if (it == stores.end()) {
      return sl::make_error(errc::not_found, "package " + package.to_string() + " not mounted");
    }
    return snapshot::open(*it->second, revision);
  }
};

gen::oset_item<package_pin> pin_item(const record_id &package, const snapshot &s,
                                     std::uint8_t key) {
  return {.element = named(id_class::element, package.to_string()),
          .order_key = {key},
          .v = package_pin{.package = package,
                           .revision = s.revision_digest().value_or(digest{}),
                           .mount = "lib/" + package.to_string()}};
}

reference external(std::string_view locator, std::vector<type_id> expected = {},
                   std::optional<record_id> package = std::nullopt) {
  return reference{.package = package,
                   .target = record_id{},
                   .expected = std::move(expected),
                   .external = external_ref{
                       .system = "file", .locator = std::string(locator), .content = std::nullopt}};
}

reference external_in(std::string_view system, std::string_view locator,
                      std::optional<digest> content = std::nullopt) {
  reference r = external(locator);
  r.external = external_ref{
      .system = std::string(system), .locator = std::string(locator), .content = content};
  return r;
}

snapshot_entry record_of(const resolution &r) { return r.record.value_or(snapshot_entry{}); }

} // namespace

TEST_CASE("FAB-INV-REF-04: an external reference resolves only through an import record") {
  world w;
  const record_id doc = well_known(id_class::entity, 100);
  const import_record rock = import_of("meshes/rock.glb");
  const import_record tree = import_of("meshes/tree.glb");
  const import_record gone = import_of("meshes/gone.glb");
  const import_record lost = import_of("meshes/lost.glb");
  const record_id shortcut = imported(id_class::entity, "file", "meshes/bare.glb");
  CHECK(import_record_id_for("file", "meshes/rock.glb") ==
        imported(id_class::import, "file", "meshes/rock.glb"));

  snapshot project = w.build(w.empty(), [&](revision_builder &b) {
    put(b, doc, rec(0), body(1), doc);
    put_import(b, rock, "meshes/rock.glb");
    put(b, rock.record, rec(0), body(10), doc);
    put_import(b, tree, "meshes/tree.glb"); // its target is never written
    put_import(b, gone, "meshes/gone.glb");
    put(b, gone.record, rec(0), body(12));
    put_import(b, lost, "meshes/lost.glb");
    put(b, lost.record, rec(0), body(13));
    put(b, shortcut, rec(0), body(14)); // an imported id with no import record
  });
  project = w.build(project, [&](revision_builder &b) {
    REQUIRE(b.retire(gone.record).has_value());
    REQUIRE(b.retire(import_record_id_for("file", "meshes/lost.glb")).has_value());
  });
  map_provider provider;
  const resolver r(project, project_manifest{}, provider);

  // Through the import record: the imported id, the usual order, the import record attached.
  resolution got = must(r.resolve(external("meshes/rock.glb"), record_id{}));
  CHECK(got.outcome == resolution_outcome::resolved);
  CHECK(got.scope == resolution_scope::project);
  CHECK(got.target == rock.record);
  CHECK(got.target.scheme() == mint_scheme::imported);
  REQUIRE(got.record.has_value());
  CHECK(record_of(got).id == rock.record);
  CHECK(got.import == rock);
  got = must(r.resolve(external("meshes/rock.glb"), doc));
  CHECK(got.scope == resolution_scope::document);
  CHECK(must(r.resolve(external("meshes/rock.glb", {rec(1)}), doc)).outcome ==
        resolution_outcome::wrong_type);
  // The content digest of the reference is not compared.
  const reference stale = external_in("file", "meshes/rock.glb", digest_of("other bytes"));
  CHECK(must(r.resolve(stale, record_id{})).outcome == resolution_outcome::resolved);

  // No import record: missing, even though a record sits at the imported id.
  got = must(r.resolve(external("meshes/bare.glb"), record_id{}));
  CHECK(got.outcome == resolution_outcome::missing);
  CHECK(got.target.is_null());
  CHECK(!got.import.has_value());
  CHECK(!got.record.has_value());
  // ... while a plain id reference to that imported id resolves directly.
  CHECK(must(r.resolve(reference{.package = std::nullopt,
                                 .target = shortcut,
                                 .expected = {},
                                 .external = std::nullopt},
                       record_id{}))
            .outcome == resolution_outcome::resolved);
  // Another system is another external object.
  const reference other_system = external_in("fbx", "meshes/rock.glb");
  CHECK(must(r.resolve(other_system, record_id{})).outcome == resolution_outcome::missing);

  // The target's outcomes: missing and retired, with the import record attached.
  got = must(r.resolve(external("meshes/tree.glb"), record_id{}));
  CHECK(got.outcome == resolution_outcome::missing);
  CHECK(got.target == tree.record);
  CHECK(got.import == tree);
  got = must(r.resolve(external("meshes/gone.glb"), record_id{}));
  CHECK(got.outcome == resolution_outcome::retired);
  CHECK(got.import == gone);
  // A retired import record: retired, without an import record.
  got = must(r.resolve(external("meshes/lost.glb"), record_id{}));
  CHECK(got.outcome == resolution_outcome::retired);
  CHECK(!got.import.has_value());

  // Ill-formed external references.
  CHECK(r.resolve(external(""), record_id{}).error().code == errc::invalid_argument);
  const reference no_system = external_in("", "meshes/rock.glb");
  CHECK(r.resolve(no_system, record_id{}).error().code == errc::invalid_argument);
}

TEST_CASE("FAB-INV-REF-04: a record at the import record id must be that import record") {
  world w;
  const import_record rock = import_of("meshes/rock.glb");
  import_record elsewhere = import_of("meshes/elsewhere.glb");
  import_record bad_record = rock;
  bad_record.record = named(id_class::entity, "not imported");
  import_record bad_system = import_of("meshes/sys.glb");
  bad_system.system = "fbx";
  bad_system.record = imported(id_class::entity, "fbx", "meshes/sys.glb");

  const snapshot project = w.build(w.empty(), [&](revision_builder &b) {
    put(b, rock.record, rec(0), body(10));
    put(b, elsewhere.record, rec(0), body(11));
    // The import record of elsewhere.glb filed under the id of mapped.glb.
    put(b, import_record_id_for("file", "meshes/mapped.glb"), import_type(),
        must(elsewhere.to_value()));
    // A record at an import record id that is not an import record.
    put(b, import_record_id_for("file", "meshes/plain.glb"), rec(0), body(12));
    // An import record whose record is not the imported id of its external object.
    put(b, import_record_id_for("file", "meshes/rock.glb"), import_type(),
        must(bad_record.to_value()));
    // An import record of another system filed under this system's id.
    put(b, import_record_id_for("file", "meshes/sys.glb"), import_type(),
        must(bad_system.to_value()));
    // A body that does not decode as an import record.
    put(b, import_record_id_for("file", "meshes/junk.glb"), import_type(), body(13));
  });
  map_provider provider;
  const resolver r(project, project_manifest{}, provider);
  for (const std::string_view locator : {"meshes/mapped.glb", "meshes/plain.glb", "meshes/rock.glb",
                                         "meshes/sys.glb", "meshes/junk.glb"}) {
    CAPTURE(locator);
    const auto got = r.resolve(external(locator), record_id{});
    REQUIRE_FALSE(got.has_value());
    CHECK(got.error().code == errc::object_corrupt);
  }
}

TEST_CASE("FAB-INV-REF-04: inside a package the import record comes from the pinned revision") {
  world w;
  const import_record rock = import_of("meshes/rock.glb");
  const import_record tree = import_of("meshes/tree.glb");
  const record_id p1 = pkg("pkg/one");
  const snapshot project = w.build(w.empty(), [&](revision_builder &b) {
    put_import(b, tree, "meshes/tree.glb");
    put(b, tree.record, rec(0), body(1));
  });
  const snapshot pkg1 = w.build(w.empty(), [&](revision_builder &b) {
    put_import(b, rock, "meshes/rock.glb");
    put(b, rock.record, rec(0), body(20));
    put(b, tree.record, rec(0), body(21)); // no import record for tree in the package
  });
  map_provider provider;
  provider.stores = {{p1, &w.store}};
  project_manifest m;
  m.package_pins = {pin_item(p1, pkg1, 0x40)};
  const resolver r(project, m, provider);

  resolution got = must(r.resolve(external("meshes/rock.glb", {}, p1), record_id{}));
  CHECK(got.outcome == resolution_outcome::resolved);
  CHECK(got.scope == resolution_scope::package);
  CHECK(got.package == p1);
  CHECK(got.import == rock);
  REQUIRE(got.record.has_value());
  CHECK(record_of(got).body.find_field(1)->as_uint() == 20);
  // The package's import records only: tree.glb is imported by the project, not by the package.
  got = must(r.resolve(external("meshes/tree.glb", {}, p1), record_id{}));
  CHECK(got.outcome == resolution_outcome::missing);
  CHECK(got.package == p1);
  // Unqualified: the project's import records; the target then follows the order.
  got = must(r.resolve(external("meshes/tree.glb"), record_id{}));
  CHECK(got.scope == resolution_scope::project);
  CHECK(record_of(got).body.find_field(1)->as_uint() == 1);
  CHECK(must(r.resolve(external("meshes/rock.glb"), record_id{})).outcome ==
        resolution_outcome::missing);
  // An unpinned package.
  got = must(r.resolve(external("meshes/rock.glb", {}, pkg("pkg/unpinned")), record_id{}));
  CHECK(got.outcome == resolution_outcome::package_missing);
  CHECK(got.package == pkg("pkg/unpinned"));
}
