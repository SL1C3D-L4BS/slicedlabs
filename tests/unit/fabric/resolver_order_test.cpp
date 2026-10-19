// FAB-INV-REF-01 (SL-ARCH-001 ARCH1-R63, ARCH1-R66): resolution follows the same document (the
// membership map), the project, then the manifest's package pins in pin order (oset order: order
// key, then element id, whatever the vector order), else missing; the first scope holding a leaf
// decides (a project record shadows a package one, a project tombstone too, the first pin shadows
// later ones, a wrong type stops the search). A package-qualified reference looks only in that
// package's pinned revision. An unavailable earlier pin stops the search with package_missing.
// References carry ids only: an id derived from a record's name finds nothing.

#include "sl/fabric/digest.hpp"
#include "sl/fabric/gen/fabric.identity.gen.hpp"
#include "sl/fabric/id.hpp"
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
#include <set>
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
record_id named(id_class c, std::string_view name) {
  return derive(c, well_known(id_class::namespace_, 1), name);
}
record_id ent(std::string_view name) { return named(id_class::entity, name); }
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

void put(revision_builder &b, const record_id &id, const type_id &t, std::uint64_t n,
         const record_id &doc = {}) {
  REQUIRE(
      b.put(id, record_write{.type = t, .document = doc, .body = body(n), .version = std::nullopt})
          .has_value());
}

class map_provider final : public package_provider {
public:
  std::map<record_id, const store_backend *> stores;
  std::set<record_id> unavailable;

  sl::result<snapshot> open(const record_id &package, const digest &revision) override {
    const auto it = stores.find(package);
    if (it == stores.end() || unavailable.contains(package)) {
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

reference ref_to(const record_id &target, std::vector<type_id> expected = {},
                 std::optional<record_id> package = std::nullopt) {
  return reference{
      .package = package, .target = target, .expected = std::move(expected), .external = {}};
}

std::uint64_t field1(const resolution &r) {
  REQUIRE(r.record.has_value());
  return r.record ? r.record->body.find_field(1)->as_uint() : 0;
}

} // namespace

TEST_CASE("FAB-INV-REF-01: document, project, pinned packages in pin order, else missing") {
  world w;
  const record_id doc_d = well_known(id_class::entity, 100);
  const record_id doc_e = well_known(id_class::entity, 101);
  const record_id a = ent("a");
  const record_id b = ent("b");
  const record_id c = ent("c");
  const record_id shadow = ent("shadow");
  const record_id both = ent("both");
  const record_id only2 = ent("only2");
  const record_id gone = ent("gone");
  const record_id nowhere = ent("nowhere");
  const record_id p1 = pkg("pkg/one");
  const record_id p2 = pkg("pkg/two");

  snapshot project = w.build(w.empty(), [&](revision_builder &rb) {
    put(rb, doc_d, rec(0), 1, doc_d);
    put(rb, a, rec(0), 10, doc_d);
    put(rb, b, rec(0), 11, doc_e);
    put(rb, c, rec(0), 12);
    put(rb, shadow, rec(1), 13);
    put(rb, gone, rec(0), 14);
  });
  project = w.build(project, [&](revision_builder &rb) { REQUIRE(rb.retire(gone).has_value()); });
  const snapshot pkg1 = w.build(w.empty(), [&](revision_builder &rb) {
    put(rb, shadow, rec(0), 20);
    put(rb, both, rec(0), 21);
    put(rb, gone, rec(0), 22);
    put(rb, a, rec(0), 23);
  });
  const snapshot pkg2 = w.build(w.empty(), [&](revision_builder &rb) {
    put(rb, both, rec(0), 30);
    put(rb, only2, rec(0), 31);
  });
  map_provider provider;
  provider.stores = {{p1, &w.store}, {p2, &w.store}};
  project_manifest m;
  // Vector order is not pin order: p1 has the smaller order key.
  m.package_pins = {pin_item(p2, pkg2, 0x60), pin_item(p1, pkg1, 0x40)};
  const resolver r(project, m, provider);
  REQUIRE(r.pins().size() == 2);
  CHECK(r.pins()[0].package == p1);
  CHECK(r.pins()[1].package == p2);

  // 1. Same document first, then the project.
  resolution got = must(r.resolve(ref_to(a), doc_d));
  CHECK(got.outcome == resolution_outcome::resolved);
  CHECK(got.scope == resolution_scope::document);
  CHECK(field1(got) == 10);
  CHECK(!got.package.has_value());
  got = must(r.resolve(ref_to(b), doc_d));
  CHECK(got.scope == resolution_scope::project);
  CHECK(field1(got) == 11);
  got = must(r.resolve(ref_to(b), doc_e));
  CHECK(got.scope == resolution_scope::document);
  got = must(r.resolve(ref_to(c), doc_d));
  CHECK(got.scope == resolution_scope::project);
  got = must(r.resolve(ref_to(a), record_id{}));
  CHECK(got.scope == resolution_scope::project);
  CHECK(field1(got) == 10); // the project's a, not pkg1's
  got = must(r.resolve(ref_to(a), well_known(id_class::entity, 999))); // an unknown document
  CHECK(got.scope == resolution_scope::project);

  // 2. The project shadows the packages: a record, a wrong type and a tombstone all decide.
  got = must(r.resolve(ref_to(shadow), record_id{}));
  CHECK(got.scope == resolution_scope::project);
  CHECK(field1(got) == 13);
  got = must(r.resolve(ref_to(shadow, {rec(0)}), record_id{}));
  CHECK(got.outcome == resolution_outcome::wrong_type);
  CHECK(got.scope == resolution_scope::project);
  got = must(r.resolve(ref_to(gone), record_id{}));
  CHECK(got.outcome == resolution_outcome::retired);
  CHECK(got.scope == resolution_scope::project);

  // 3. Pins in pin order.
  got = must(r.resolve(ref_to(both), doc_d));
  CHECK(got.outcome == resolution_outcome::resolved);
  CHECK(got.scope == resolution_scope::package);
  CHECK(got.package == p1);
  CHECK(field1(got) == 21);
  got = must(r.resolve(ref_to(only2), doc_d));
  CHECK(got.package == p2);
  CHECK(field1(got) == 31);

  // 4. Missing.
  got = must(r.resolve(ref_to(nowhere), doc_d));
  CHECK(got.outcome == resolution_outcome::missing);
  CHECK(got.scope == resolution_scope::none);
  CHECK(!got.record.has_value());
  CHECK(!got.package.has_value());
  CHECK(got.target == nowhere);

  // Names never resolve: a reference by an id made from a record's name finds nothing.
  CHECK(must(r.resolve(ref_to(ent("10")), doc_d)).outcome == resolution_outcome::missing);

  // Swapping the order keys swaps the winner.
  project_manifest swapped;
  swapped.package_pins = {pin_item(p2, pkg2, 0x20), pin_item(p1, pkg1, 0x40)};
  const resolver rs(project, swapped, provider);
  got = must(rs.resolve(ref_to(both), doc_d));
  CHECK(got.package == p2);
  CHECK(field1(got) == 30);
  // Equal order keys fall back to the element id.
  project_manifest tied;
  tied.package_pins = {pin_item(p2, pkg2, 0x40), pin_item(p1, pkg1, 0x40)};
  const resolver rt(project, tied, provider);
  const bool p1_first =
      named(id_class::element, p1.to_string()) < named(id_class::element, p2.to_string());
  CHECK(rt.pins()[0].package == (p1_first ? p1 : p2));
  CHECK(must(rt.resolve(ref_to(both), record_id{})).package == (p1_first ? p1 : p2));
}

TEST_CASE("FAB-INV-REF-01: a package-qualified reference looks only in its pinned revision") {
  world w;
  const record_id a = ent("a");
  const record_id both = ent("both");
  const record_id only2 = ent("only2");
  const record_id p1 = pkg("pkg/one");
  const record_id p2 = pkg("pkg/two");
  const snapshot project = w.build(w.empty(), [&](revision_builder &rb) { put(rb, a, rec(0), 1); });
  const snapshot pkg1 =
      w.build(w.empty(), [&](revision_builder &rb) { put(rb, both, rec(0), 21); });
  snapshot pkg2_old = w.build(w.empty(), [&](revision_builder &rb) { put(rb, both, rec(0), 29); });
  const snapshot pkg2 = w.build(pkg2_old, [&](revision_builder &rb) {
    put(rb, both, rec(0), 30);
    put(rb, only2, rec(0), 31);
  });
  map_provider provider;
  provider.stores = {{p1, &w.store}, {p2, &w.store}};
  project_manifest m;
  m.package_pins = {pin_item(p1, pkg1, 0x40), pin_item(p2, pkg2, 0x60)};
  const resolver r(project, m, provider);

  resolution got = must(r.resolve(ref_to(both, {}, p2), record_id{}));
  CHECK(got.outcome == resolution_outcome::resolved);
  CHECK(got.package == p2);
  CHECK(field1(got) == 30);
  // Not in that package: missing, never another package or the project.
  got = must(r.resolve(ref_to(only2, {}, p1), record_id{}));
  CHECK(got.outcome == resolution_outcome::missing);
  CHECK(got.package == p1);
  CHECK(must(r.resolve(ref_to(a, {}, p1), record_id{})).outcome == resolution_outcome::missing);

  // The pinned revision, not the package's newest: pinning the old revision gives the old body.
  project_manifest old;
  old.package_pins = {pin_item(p2, pkg2_old, 0x40)};
  const resolver ro(project, old, provider);
  CHECK(field1(must(ro.resolve(ref_to(both, {}, p2), record_id{}))) == 29);
  CHECK(must(ro.resolve(ref_to(only2, {}, p2), record_id{})).outcome ==
        resolution_outcome::missing);

  // An unpinned package is package_missing, even when the provider could mount it.
  project_manifest none;
  const resolver rn(project, none, provider);
  got = must(rn.resolve(ref_to(both, {}, p2), record_id{}));
  CHECK(got.outcome == resolution_outcome::package_missing);
  CHECK(got.package == p2);
  CHECK(must(rn.resolve(ref_to(both), record_id{})).outcome == resolution_outcome::missing);
}

TEST_CASE("FAB-INV-REF-01: an unavailable pin stops the search at its place in the order") {
  world w;
  const record_id both = ent("both");
  const record_id only2 = ent("only2");
  const record_id local = ent("local");
  const record_id p1 = pkg("pkg/one");
  const record_id p2 = pkg("pkg/two");
  const snapshot project =
      w.build(w.empty(), [&](revision_builder &rb) { put(rb, local, rec(0), 1); });
  const snapshot pkg1 =
      w.build(w.empty(), [&](revision_builder &rb) { put(rb, both, rec(0), 21); });
  const snapshot pkg2 = w.build(w.empty(), [&](revision_builder &rb) {
    put(rb, both, rec(0), 30);
    put(rb, only2, rec(0), 31);
  });
  map_provider provider;
  provider.stores = {{p1, &w.store}, {p2, &w.store}};
  project_manifest m;
  m.package_pins = {pin_item(p1, pkg1, 0x40), pin_item(p2, pkg2, 0x60)};
  const resolver r(project, m, provider);

  provider.unavailable = {p1};
  resolution got = must(r.resolve(ref_to(only2), record_id{}));
  CHECK(got.outcome == resolution_outcome::package_missing);
  CHECK(got.package == p1);
  // The project is consulted before any package.
  CHECK(must(r.resolve(ref_to(local), record_id{})).outcome == resolution_outcome::resolved);
  // A later unavailable pin does not matter once an earlier scope decides.
  provider.unavailable = {p2};
  got = must(r.resolve(ref_to(both), record_id{}));
  CHECK(got.package == p1);
  got = must(r.resolve(ref_to(only2), record_id{}));
  CHECK(got.outcome == resolution_outcome::package_missing);
  CHECK(got.package == p2);
}
