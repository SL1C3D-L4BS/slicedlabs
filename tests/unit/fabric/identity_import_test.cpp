// FAB-INV-LC-04 (SL-ARCH-001 ARCH1-R07, ARCH1-R44, ARCH1-R65; ADR-0011): imported ids are a pure
// function of (class, system, external id), so re-importing a changed file lands on the record it
// produced before (its body is updated, its id and its import record's record are not); derived
// ids are a pure function of the derivation key, so re-cooking unchanged inputs (in any input
// order, in another workspace) lands on the same derived record and record object, and any change
// to an input, the toolchain, the parameters or the environment gives a new id. Import records
// are checked: the record must be the imported id of their (system, external id). A deleted import
// stays retired: the next import of the same external object is id_reuse. Seeded: random external
// ids and provenance records give stable ids, and distinct inputs never collide.

#include "sl/fabric/clock.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/identity.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/provenance.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/tx_fixture.hpp"
#include "../../../engine/fabric/testing/undo_fixture.hpp"
#include "../../../engine/fabric/testing/value_gen.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
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

void must_ok(const sl::result<void> &r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
}

digest digest_of(std::string_view text) {
  digest d;
  d.bytes = sha256::hash(text);
  return d;
}

std::unique_ptr<workspace> scene_ws(std::uint64_t seed = 1) {
  return must(
      workspace::create_with(std::make_shared<memory_backend>(),
                             must(tg::scene_options(std::make_shared<manual_clock>(1), seed))));
}

// An import of mesh file `locator` with `vertices`: a Mesh created under its imported id, or its
// vertex count set when the id is live already. Returns the import record.
import_record import_mesh(transaction_manager &m, std::string_view locator,
                          std::uint32_t vertices) {
  const import_record rec =
      must(make_import_record(id_class::asset, "file", locator, locator,
                              digest_of(std::format("{}#{}", locator, vertices))));
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  if (must(must(m.head()).contains(rec.record))) {
    must(tg::commit_ops(m, editor,
                        {op_set(rec.record, {path_segment::field(2)}, value::uint(vertices))}));
  } else {
    patch_op create = must(tg::create_mesh("imported", vertices));
    create.target = rec.record;
    must(tg::commit_ops(m, editor, {create}));
  }
  return rec;
}

// A cook of `source` with provenance `p` under derived_id_for(p).
commit_receipt cook(transaction_manager &m, const record_id &source, const provenance &p) {
  tx_options o;
  o.origin = tg::tx_origin_of(tg::tx_editor(), "asset");
  o.origin.provenance = p;
  o.origin.epistemic = epistemic_class::calculated;
  auto t = must(m.begin(o));
  patch_op create = must(tg::create_cooked("cooked", source));
  create.target = must(derived_id_for(p)).id();
  must_ok(t.stage(create));
  return must(t.commit());
}

} // namespace

TEST_CASE("FAB-INV-LC-04: re-import keeps the imported id and its import record") {
  auto ws = scene_ws();
  transaction_manager &m = ws->manager();
  const import_record first = import_mesh(m, "meshes/rock.glb", 128);
  CHECK(first.record == imported(id_class::asset, "file", "meshes/rock.glb"));
  CHECK(first.record.scheme() == mint_scheme::imported);
  CHECK(first.record.cls() == id_class::asset);
  const snapshot_entry before = must(must(m.head()).get(first.record));

  // The file changed: same id, new body and content digest.
  const import_record second = import_mesh(m, "meshes/rock.glb", 256);
  CHECK(second.record == first.record);
  CHECK(second.content != first.content);
  const snapshot head = must(m.head());
  CHECK(must(head.entry_count()) == 1);
  const snapshot_entry after = must(head.get(first.record));
  CHECK(after.object != before.object);
  CHECK(after.body.find_field(2)->as_uint() == 256);
  // A second workspace importing the same file names the same record.
  auto other = scene_ws(9);
  CHECK(import_mesh(other->manager(), "meshes/rock.glb", 256).record == first.record);
  CHECK(must(must(other->manager().head()).get(first.record)).object == after.object);

  // A deleted import stays retired: importing the same external object again is id_reuse.
  must(tg::commit_ops(m, tg::tx_origin_of(tg::tx_editor()), {op_delete(first.record)}));
  patch_op again = must(tg::create_mesh("imported", 64));
  again.target = first.record;
  tx_options o;
  o.origin = tg::tx_origin_of(tg::tx_editor());
  auto t = must(m.begin(o));
  must_ok(t.stage(again));
  CHECK(t.commit().error().code == errc::validation_failed);
  CHECK(std::ranges::any_of(t.diagnostics(),
                            [](const diagnostic &d) { return d.rule == "R-INV-ID-REUSE"; }));
}

TEST_CASE("FAB-INV-LC-04: import records and imported ids are checked") {
  const digest content = digest_of("bytes");
  const import_record r =
      must(make_import_record(id_class::asset, "fbx", "Mesh:12", "a.fbx", content));
  must_ok(check_import_record(r));
  CHECK(must(imported_id_for(id_class::asset, "fbx", "Mesh:12")) == r.record);
  // Domain separated by class, system and external id.
  CHECK(must(imported_id_for(id_class::entity, "fbx", "Mesh:12")) != r.record);
  CHECK(must(imported_id_for(id_class::asset, "gltf", "Mesh:12")) != r.record);
  CHECK(must(imported_id_for(id_class::asset, "fbx", "Mesh:13")) != r.record);
  CHECK(must(imported_id_for(id_class::asset, "fb", "xMesh:12")) != r.record);

  CHECK(imported_id_for(id_class::tx, "fbx", "Mesh:12").error().code == errc::id_class_mismatch);
  CHECK(imported_id_for(id_class::element, "fbx", "Mesh:12").error().code ==
        errc::id_class_mismatch);
  CHECK(imported_id_for(id_class::none, "fbx", "Mesh:12").error().code == errc::id_class_mismatch);
  CHECK(imported_id_for(id_class::asset, "", "Mesh:12").error().code == errc::invalid_argument);
  CHECK(imported_id_for(id_class::asset, "fbx", "").error().code == errc::invalid_argument);
  CHECK(make_import_record(id_class::asset, "fbx", "Mesh:12", "", content).error().code ==
        errc::invalid_argument);
  CHECK(make_import_record(id_class::asset, "fbx", "Mesh:12", "a.fbx", digest{}).error().code ==
        errc::invalid_argument);

  // A record that is not the imported id of (system, external id) is refused.
  for (const record_id &wrong :
       {tg::asset_named("Mesh:12"), must(imported_id_for(id_class::asset, "fbx", "Mesh:13")),
        must(imported_id_for(id_class::entity, "gltf", "Mesh:12")), record_id{}}) {
    CAPTURE(wrong.to_string());
    import_record bad = r;
    bad.record = wrong;
    CHECK(check_import_record(bad).error().code == errc::invalid_argument);
  }
  // The class is the record's own: the same external object imported as an entity is fine.
  import_record as_entity = r;
  as_entity.record = must(imported_id_for(id_class::entity, "fbx", "Mesh:12"));
  must_ok(check_import_record(as_entity));
  // The generated type round-trips.
  CHECK(must(import_record::from_value(must(r.to_value()))) == r);
}

TEST_CASE("FAB-INV-LC-04: re-cook keeps the derived id; any input change makes a new one") {
  auto ws = scene_ws();
  transaction_manager &m = ws->manager();
  const record_id mesh = tg::asset_named("rock");
  must(tg::commit_ops(m, tg::tx_origin_of(tg::tx_editor()), {must(tg::create_mesh("rock", 128))}));
  const provenance p = tg::complete_provenance(mesh);
  const derived_id id = must(derived_id_for(p));
  CHECK(id.id().cls() == id_class::derived);
  CHECK(id.id() == derive(id_class::derived, well_known(id_class::namespace_, 1),
                          must(derivation_key(p)).to_hex()));
  CHECK(must(derived_id_for(must(derivation_key(p)))) == id);
  cook(m, mesh, p);
  const snapshot_entry cooked = must(must(m.head()).get(id.id()));

  // Re-cook in another workspace: same id, same record object.
  auto other = scene_ws(5);
  must(tg::commit_ops(other->manager(), tg::tx_origin_of(tg::tx_editor()),
                      {must(tg::create_mesh("rock", 128))}));
  cook(other->manager(), mesh, p);
  CHECK(must(must(other->manager().head()).get(id.id())).object == cooked.object);

  // Input order does not matter; every field does.
  provenance two = p;
  two.input_ids = std::vector<record_id>{mesh, tg::asset_named("rock2")};
  two.input_revisions = std::vector<digest>{digest_of("r1"), digest_of("r2")};
  provenance swapped = two;
  swapped.input_ids = std::vector<record_id>{tg::asset_named("rock2"), mesh};
  swapped.input_revisions = std::vector<digest>{digest_of("r2"), digest_of("r1")};
  CHECK(must(derived_id_for(two)) == must(derived_id_for(swapped)));
  std::set<record_id> ids = {id.id(), must(derived_id_for(two)).id()};
  provenance v = p;
  v.input_revisions = std::vector<digest>{digest_of("edited")};
  ids.insert(must(derived_id_for(v)).id());
  v = p;
  v.toolchain_id = "sl.cook/2";
  ids.insert(must(derived_id_for(v)).id());
  v = p;
  v.parameters_digest = digest_of("other parameters");
  ids.insert(must(derived_id_for(v)).id());
  v = p;
  v.environment_hash = digest_of("other environment");
  ids.insert(must(derived_id_for(v)).id());
  CHECK(ids.size() == 6);

  // No key, no id.
  provenance incomplete = p;
  incomplete.toolchain_id.reset();
  CHECK(derived_id_for(incomplete).error().code == errc::provenance_incomplete);
  CHECK(derived_id_for(digest{}).error().code == errc::invalid_argument);
  digest blake = must(derivation_key(p));
  blake.algo = hash_algo::blake3;
  CHECK(derived_id_for(blake).error().code == errc::invalid_argument);
}

TEST_CASE("FAB-INV-LC-04: seeded external ids and provenance give stable, distinct ids") {
  const std::size_t seeds = tg::sweep_scaled(50);
  std::set<record_id> imports;
  std::set<record_id> derived;
  for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
    CAPTURE(seed);
    tg::value_gen values(seed);
    const std::string system = std::format("sys{}", values.random().below(4));
    const std::string external = std::format("obj/{}", values.random().next());
    const record_id a = must(imported_id_for(id_class::asset, system, external));
    CHECK(must(imported_id_for(id_class::asset, system, external)) == a); // pure
    CHECK(must(make_import_record(id_class::asset, system, external, "x", digest_of(external)))
              .record == a);
    imports.insert(a);

    provenance p = tg::complete_provenance(values.random_id(id_class::asset));
    p.parameters_digest = digest_of(std::format("params/{}", values.random().next()));
    const derived_id d = must(derived_id_for(p));
    provenance copy = p;
    CHECK(must(derived_id_for(copy)) == d);
    derived.insert(d.id());
  }
  CHECK(imports.size() == seeds);
  CHECK(derived.size() == seeds);
}
