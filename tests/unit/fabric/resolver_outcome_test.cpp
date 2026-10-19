// FAB-INV-REF-02 (SL-ARCH-001 ARCH1-R64; ADR-0009): resolved, retired, missing, wrong_type and
// package_missing are distinct outcomes, each with its own evidence (the record for resolved and
// wrong_type, the package for package scopes and package_missing, no record for retired and
// missing); package_missing covers an unpinned package, a pin the provider cannot mount
// (not_found) and a pinned revision absent from the store (object_missing). A read failure is an
// error, never an outcome: a corrupt record object in the project or in a package, a corrupt
// membership read, a provider failure other than unavailability and a provider snapshot at another
// revision than the pin; an ill-formed reference is invalid_argument.

#include "sl/fabric/digest.hpp"
#include "sl/fabric/gen/fabric.identity.gen.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/manifest.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/resolver.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/tree.hpp"
#include "sl/fabric/value.hpp"

#include "../../../engine/fabric/testing/store_faults.hpp"
#include "../../../engine/fabric/testing/tree_fixture.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <memory>
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
  std::shared_ptr<tg::store_faults> faults = std::make_shared<tg::store_faults>();
  memory_backend store{faults};
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
  std::optional<errc> fail;                // every open fails with this code
  std::optional<snapshot> ignore_revision; // returned whatever the pin says

  sl::result<snapshot> open(const record_id &package, const digest &revision) override {
    if (fail) {
      return sl::make_error(*fail, "injected provider failure");
    }
    if (ignore_revision) {
      return *ignore_revision;
    }
    const auto it = stores.find(package);
    if (it == stores.end() || unavailable.contains(package)) {
      return sl::make_error(errc::not_found, "package " + package.to_string() + " not mounted");
    }
    return snapshot::open(*it->second, revision);
  }
};

gen::oset_item<package_pin> pin_item(const record_id &package, const digest &revision,
                                     std::uint8_t key) {
  return {.element = named(id_class::element, package.to_string()),
          .order_key = {key},
          .v = package_pin{
              .package = package, .revision = revision, .mount = "lib/" + package.to_string()}};
}

reference ref_to(const record_id &target, std::vector<type_id> expected = {},
                 std::optional<record_id> package = std::nullopt) {
  return reference{
      .package = package, .target = target, .expected = std::move(expected), .external = {}};
}

snapshot_entry record_of(const resolution &r) { return r.record.value_or(snapshot_entry{}); }

digest digest_of(std::string_view text) {
  digest d;
  d.bytes = sha256::hash(text);
  return d;
}

} // namespace

TEST_CASE("FAB-INV-REF-02: the five outcomes are distinct, each with its own evidence") {
  world w;
  const record_id live = ent("live");
  const record_id gone = ent("gone");
  const record_id pkg_live = ent("pkg-live");
  const record_id pkg_gone = ent("pkg-gone");
  const record_id p1 = pkg("pkg/one");
  const record_id p_unpinned = pkg("pkg/unpinned");
  const record_id p_absent = pkg("pkg/absent");

  snapshot project = w.build(w.empty(), [&](revision_builder &b) {
    put(b, live, rec(1), 1);
    put(b, gone, rec(1), 2);
  });
  project = w.build(project, [&](revision_builder &b) { REQUIRE(b.retire(gone).has_value()); });
  snapshot pkg1 = w.build(w.empty(), [&](revision_builder &b) {
    put(b, pkg_live, rec(2), 3);
    put(b, pkg_gone, rec(2), 4);
  });
  pkg1 = w.build(pkg1, [&](revision_builder &b) { REQUIRE(b.retire(pkg_gone).has_value()); });
  map_provider provider;
  provider.stores = {{p1, &w.store}, {p_absent, &w.store}};
  project_manifest m;
  m.package_pins = {pin_item(p1, pkg1.revision_digest().value_or(digest{}), 0x40)};
  const resolver r(project, m, provider);

  // resolved: the record, its scope.
  resolution got = must(r.resolve(ref_to(live, {rec(1), rec(3)}), record_id{}));
  CHECK(got.outcome == resolution_outcome::resolved);
  REQUIRE(got.record.has_value());
  CHECK(record_of(got).id == live);
  CHECK(record_of(got).type == rec(1));
  CHECK(got.target == live);
  // wrong_type: the record is returned so the caller sees its actual type.
  got = must(r.resolve(ref_to(live, {rec(0)}), record_id{}));
  CHECK(got.outcome == resolution_outcome::wrong_type);
  REQUIRE(got.record.has_value());
  CHECK(record_of(got).type == rec(1));
  // retired: no record, whatever types are expected.
  for (const std::vector<type_id> &expected : {std::vector<type_id>{}, std::vector{rec(0)}}) {
    got = must(r.resolve(ref_to(gone, expected), record_id{}));
    CHECK(got.outcome == resolution_outcome::retired);
    CHECK(got.scope == resolution_scope::project);
    CHECK(!got.record.has_value());
  }
  // missing.
  got = must(r.resolve(ref_to(ent("never")), record_id{}));
  CHECK(got.outcome == resolution_outcome::missing);
  CHECK(!got.record.has_value());

  // The same outcomes inside a package carry the package.
  got = must(r.resolve(ref_to(pkg_live, {rec(2)}), record_id{}));
  CHECK(got.outcome == resolution_outcome::resolved);
  CHECK(got.package == p1);
  got = must(r.resolve(ref_to(pkg_live, {rec(0)}, p1), record_id{}));
  CHECK(got.outcome == resolution_outcome::wrong_type);
  CHECK(got.package == p1);
  got = must(r.resolve(ref_to(pkg_gone), record_id{}));
  CHECK(got.outcome == resolution_outcome::retired);
  CHECK(got.scope == resolution_scope::package);
  CHECK(got.package == p1);
  got = must(r.resolve(ref_to(pkg_gone, {}, p1), record_id{}));
  CHECK(got.outcome == resolution_outcome::retired);

  // package_missing: unpinned (though mountable), pinned but not mounted, pinned revision absent.
  got = must(r.resolve(ref_to(pkg_live, {}, p_unpinned), record_id{}));
  CHECK(got.outcome == resolution_outcome::package_missing);
  CHECK(got.package == p_unpinned);
  CHECK(!got.record.has_value());
  provider.stores.emplace(p_unpinned, &w.store);
  CHECK(must(r.resolve(ref_to(pkg_live, {}, p_unpinned), record_id{})).outcome ==
        resolution_outcome::package_missing);
  provider.unavailable = {p1};
  got = must(r.resolve(ref_to(pkg_live, {}, p1), record_id{}));
  CHECK(got.outcome == resolution_outcome::package_missing);
  CHECK(got.package == p1);
  provider.unavailable.clear();
  project_manifest absent = m;
  absent.package_pins.push_back(pin_item(p_absent, digest_of("no such revision"), 0x50));
  const resolver ra(project, absent, provider);
  got = must(ra.resolve(ref_to(pkg_live, {}, p_absent), record_id{}));
  CHECK(got.outcome == resolution_outcome::package_missing);
  CHECK(got.package == p_absent);
  got = must(ra.resolve(ref_to(ent("never")), record_id{})); // the search reaches the absent pin
  CHECK(got.outcome == resolution_outcome::package_missing);
  CHECK(got.package == p_absent);

  // The names are distinct.
  std::set<std::string_view> names;
  for (const resolution_outcome o :
       {resolution_outcome::resolved, resolution_outcome::retired, resolution_outcome::missing,
        resolution_outcome::wrong_type, resolution_outcome::package_missing}) {
    names.insert(to_string(o));
  }
  CHECK(names == std::set<std::string_view>{"resolved", "retired", "missing", "wrong_type",
                                            "package_missing"});
  CHECK(to_string(static_cast<resolution_outcome>(9)) == "unknown");
  CHECK(to_string(resolution_scope::document) == "document");
  CHECK(to_string(static_cast<resolution_scope>(9)) == "unknown");
}

TEST_CASE("FAB-INV-REF-02: a read failure is an error, never an outcome") {
  world w;
  const record_id doc = well_known(id_class::entity, 100);
  const record_id live = ent("live");
  const record_id member = ent("member");
  const record_id pkg_live = ent("pkg-live");
  const record_id p1 = pkg("pkg/one");
  const snapshot project = w.build(w.empty(), [&](revision_builder &b) {
    put(b, live, rec(1), 1);
    put(b, member, rec(1), 2, doc);
  });
  const snapshot pkg1 =
      w.build(w.empty(), [&](revision_builder &b) { put(b, pkg_live, rec(2), 3); });
  map_provider provider;
  provider.stores = {{p1, &w.store}};
  project_manifest m;
  m.package_pins = {pin_item(p1, pkg1.revision_digest().value_or(digest{}), 0x40)};
  const resolver r(project, m, provider);

  // A corrupt record object: object_corrupt from the store, in every scope that reads it.
  const digest live_obj = must(project.get(live)).object;
  const digest member_obj = must(project.get(member)).object;
  const digest pkg_obj = must(pkg1.get(pkg_live)).object;
  w.faults->flip_on_read(live_obj, 12, 0x5A);
  w.faults->flip_on_read(member_obj, 12, 0x5A);
  w.faults->flip_on_read(pkg_obj, 12, 0x5A);
  const std::vector<std::pair<reference, record_id>> reads = {
      {ref_to(live), record_id{}},
      {ref_to(member), doc},
      {ref_to(pkg_live), record_id{}},
      {ref_to(pkg_live, {}, p1), record_id{}},
      {ref_to(live, {rec(0)}), record_id{}}};
  for (const auto &[ref, from] : reads) {
    CAPTURE(ref.target.to_string());
    const auto got = r.resolve(ref, from);
    REQUIRE_FALSE(got.has_value());
    CHECK(got.error().code == errc::object_corrupt);
  }
  w.faults->clear_flips();
  CHECK(must(r.resolve(ref_to(live), record_id{})).outcome == resolution_outcome::resolved);

  // A corrupt membership map (the document's member trie) fails the document step.
  const auto doc_entry = must(tree_reader(w.store).find(project.membership_root(), doc));
  REQUIRE(doc_entry.has_value());
  w.faults->flip_on_read(doc_entry.value_or(tree_entry{}).target, 12, 0x5A);
  CHECK(r.resolve(ref_to(member), doc).error().code == errc::object_corrupt);
  CHECK(must(r.resolve(ref_to(member), record_id{})).outcome == resolution_outcome::resolved);
  w.faults->clear_flips();

  // A provider failure other than unavailability.
  for (const errc code : {errc::store_io, errc::object_corrupt, errc::checkpoint_invalid}) {
    provider.fail = code;
    CHECK(r.resolve(ref_to(pkg_live, {}, p1), record_id{}).error().code == code);
    CHECK(r.resolve(ref_to(ent("never")), record_id{}).error().code == code);
    // A reference the project decides never asks the provider.
    CHECK(must(r.resolve(ref_to(live), record_id{})).outcome == resolution_outcome::resolved);
  }
  provider.fail.reset();

  // A provider snapshot that is not the pinned revision.
  provider.ignore_revision = project;
  CHECK(r.resolve(ref_to(pkg_live, {}, p1), record_id{}).error().code == errc::invalid_argument);
  provider.ignore_revision = must(snapshot::empty(w.store));
  CHECK(r.resolve(ref_to(ent("never")), record_id{}).error().code == errc::invalid_argument);
  provider.ignore_revision.reset();

  // Ill-formed references.
  CHECK(r.resolve(ref_to(record_id{}), record_id{}).error().code == errc::invalid_argument);
  reference both = ref_to(live);
  both.external = external_ref{.system = "file", .locator = "a.glb", .content = std::nullopt};
  CHECK(r.resolve(both, record_id{}).error().code == errc::invalid_argument);
}
