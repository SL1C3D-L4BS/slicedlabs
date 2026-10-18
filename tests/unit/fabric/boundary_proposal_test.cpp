// FAB-INV-BND-01 (SL-ARCH-001 ARCH1-R60; master plan section 2.1): runtime changes enter the
// document only as proposed patches with epistemic class observed, through the transaction
// manager. propose_from_observation() builds a normalised patch on the projection's revision and
// commits nothing; the manager then runs every validation stage over it; a record outside the
// projection's domains is refused (authority_denied) before any transaction exists; an origin that
// is not observed is refused; the committed tx record says observed.

#include "sl/fabric/boundary.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/provenance.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/test_schemas.hpp"
#include "../../../engine/fabric/testing/tx_fixture.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
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

actor_id sensor() { return must(actor_id::from(tg::test_id(id_class::actor, "bnd/sensor"))); }

// A workspace with a runtime sensor (a service holding write:content.scene only) and a small
// project: two entities, a scene owning them, and a mesh.
struct fixture {
  std::shared_ptr<memory_backend> store = std::make_shared<memory_backend>();
  std::unique_ptr<workspace> ws;
  record_id hero = tg::entity_named("hero");
  record_id villain = tg::entity_named("villain");
  record_id level = tg::entity_named("level");
  record_id mesh = tg::asset_named("rock");

  fixture() {
    workspace_options o = must(tg::scene_options(std::make_shared<manual_clock>(21), 3));
    must_ok(o.actors.add(
        actor{.id = sensor(), .kind = actor_kind::service, .grants = {"write:content.scene"}}));
    ws = must(workspace::create_with(store, std::move(o)));
    must(tg::commit_ops(ws->manager(), tg::tx_origin_of(tg::tx_editor(), "content"),
                        {must(tg::create_entity("hero")), must(tg::create_entity("villain")),
                         must(tg::create_scene(level, "level", {hero, villain})),
                         must(tg::create_mesh("rock", 128))}));
  }

  [[nodiscard]] transaction_manager &manager() const { return ws->manager(); }

  [[nodiscard]] document_projection project(const std::vector<std::string> &domains) const {
    return must(document_projection::make(must(manager().head()), manager().schemas(), domains));
  }

  static tx_origin runtime_origin(epistemic_class c = epistemic_class::observed) {
    tx_origin o = tg::tx_origin_of(sensor(), "runtime");
    o.epistemic = c;
    return o;
  }

  // A transaction that stages `p` on its base, ready to commit.
  [[nodiscard]] transaction submit(const patch &p, const tx_origin &origin,
                                   commit_policy policy = commit_policy::reject_stale) const {
    tx_options o;
    o.origin = origin;
    o.base = p.base;
    o.policy = policy;
    transaction t = must(manager().begin(o));
    must_ok(t.stage(p));
    return t;
  }

  [[nodiscard]] std::optional<std::string> name_of(const record_id &id) const {
    const auto e = must(manager().head()).get(id);
    if (!e) {
      return std::nullopt;
    }
    return std::string(must(e->body.get({path_segment::field(1)})).as_string());
  }
};

observed_change rename(const record_id &id, const std::string &name) {
  return observed_change{
      .record = id, .path = {path_segment::field(1)}, .value = must(value::string(name))};
}

} // namespace

TEST_CASE("FAB-INV-BND-01: an observation becomes a normalised patch on the projection's "
          "revision and commits nothing") {
  fixture f;
  const document_projection proj = f.project({"content.scene"});
  const std::optional<digest> head = must(f.manager().head()).revision_digest();
  const std::uint64_t commits = f.manager().commits();

  const std::array changes{rename(f.hero, "hero (seen)")};
  const patch p = must(propose_from_observation(proj, changes, fixture::runtime_origin()));
  CHECK(p.base == proj.revision_digest());
  CHECK(p.base == head);
  CHECK(p.meta.label == observation_label);
  REQUIRE(p.ops.size() == 1);
  CHECK(p.ops[0].kind == op_kind::set);
  CHECK(p.ops[0].target == f.hero);
  CHECK(is_normalized(p));
  // The expectation is the projected value: the observation was made against this state.
  CHECK(p.ops[0].expect.is_present());

  // Proposing is pure: the document, the head and the commit count are untouched.
  CHECK(must(f.manager().head()).revision_digest() == head);
  CHECK(f.manager().commits() == commits);
  CHECK(f.name_of(f.hero) == "hero");

  // The same proposal twice is the same patch (deterministic).
  CHECK(p == must(propose_from_observation(proj, changes, fixture::runtime_origin())));
}

TEST_CASE("FAB-INV-BND-01: a proposal is applied only by committing it through the manager, as "
          "class observed") {
  fixture f;
  const document_projection before = f.project({"content.scene"});
  const std::array changes{rename(f.hero, "hero (seen)"), rename(f.villain, "villain (seen)")};
  const patch p = must(propose_from_observation(before, changes, fixture::runtime_origin()));

  transaction t = f.submit(p, fixture::runtime_origin());
  const commit_receipt receipt = must(t.commit());
  CHECK(f.name_of(f.hero) == "hero (seen)");
  CHECK(f.name_of(f.villain) == "villain (seen)");

  // The tx record carries the class and the runtime subsystem.
  const auto rev = must(decode_revision(must(f.store->get(receipt.revision))));
  const auto tx = must(decode_tx_record(must(f.store->get(rev.tx))));
  CHECK(tx.origin.epistemic == epistemic_class::observed);
  CHECK(tx.origin.subsystem == "03");

  // A projection is immutable: the one taken before still shows the old document, a new one the
  // new document, and the new one is keyed by another root digest.
  CHECK(must(before.get(f.hero)).body->get({path_segment::field(1)})->as_string() == "hero");
  const document_projection after = f.project({"content.scene"});
  CHECK(must(after.get(f.hero)).body->get({path_segment::field(1)})->as_string() == "hero (seen)");
  CHECK(after.root_digest() != before.root_digest());
  CHECK(after.revision_digest() == receipt.revision);
}

TEST_CASE("FAB-INV-BND-01: only class observed is accepted for a proposal") {
  fixture f;
  const document_projection proj = f.project({"content.scene"});
  const std::array changes{rename(f.hero, "hero (seen)")};
  for (const epistemic_class c :
       {epistemic_class::imported, epistemic_class::asserted, epistemic_class::calculated,
        epistemic_class::inferred, epistemic_class::accepted}) {
    const auto r = propose_from_observation(proj, changes, fixture::runtime_origin(c));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == errc::invalid_argument);
  }
  // No changes, and a null record id, are refused too.
  const auto none =
      propose_from_observation(proj, std::span<const observed_change>{}, fixture::runtime_origin());
  REQUIRE_FALSE(none.has_value());
  CHECK(none.error().code == errc::invalid_argument);
  const std::array null_change{rename(record_id{}, "nobody")};
  const auto nobody = propose_from_observation(proj, null_change, fixture::runtime_origin());
  REQUIRE_FALSE(nobody.has_value());
  CHECK(nobody.error().code == errc::invalid_argument);
}

TEST_CASE("FAB-INV-BND-01: a record outside the projection's domains is refused before it "
          "reaches the manager") {
  fixture f;
  const document_projection proj = f.project({"content.scene"});
  const std::optional<digest> head = must(f.manager().head()).revision_digest();
  const std::uint64_t commits = f.manager().commits();

  // The mesh lives in content.mesh: live, but not in this projection. One bad change spoils the
  // whole proposal, whatever its place in the list.
  const observed_change mesh_change{
      .record = f.mesh, .path = {path_segment::field(2)}, .value = value::uint(256)};
  const std::array changes{rename(f.hero, "hero (seen)"), mesh_change};
  const auto r = propose_from_observation(proj, changes, fixture::runtime_origin());
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == errc::authority_denied);
  CHECK(r.error().message.find(f.mesh.to_string()) != std::string::npos);

  // A record that does not exist is not_found, not a denial.
  const std::array ghost{rename(tg::entity_named("ghost"), "boo")};
  const auto g = propose_from_observation(proj, ghost, fixture::runtime_origin());
  REQUIRE_FALSE(g.has_value());
  CHECK(g.error().code == errc::not_found);

  // Nothing was committed, begun or changed.
  CHECK(must(f.manager().head()).revision_digest() == head);
  CHECK(f.manager().commits() == commits);
  CHECK(f.name_of(f.hero) == "hero");
}

TEST_CASE("FAB-INV-BND-01: the manager still runs every validation stage over a proposal") {
  fixture f;
  const std::optional<digest> head = must(f.manager().head()).revision_digest();

  // In scope (the projection covers content), but the sensor may write content.scene only: the
  // authority stage refuses the mesh edit.
  const document_projection wide = f.project({"content"});
  const observed_change mesh_change{
      .record = f.mesh, .path = {path_segment::field(2)}, .value = value::uint(256)};
  const std::array denied_changes{mesh_change};
  const patch denied =
      must(propose_from_observation(wide, denied_changes, fixture::runtime_origin()));
  transaction t1 = f.submit(denied, fixture::runtime_origin());
  const auto r1 = t1.commit();
  REQUIRE_FALSE(r1.has_value());
  CHECK(r1.error().code == errc::validation_failed);
  CHECK(std::ranges::any_of(t1.diagnostics(), [](const diagnostic &d) {
    return d.rule == "R-AUTH-DENIED" && d.stage == validator_stage::authority;
  }));

  // A value of the wrong type passes the projection and fails the schema stage.
  const std::array wrong{
      observed_change{.record = f.hero, .path = {path_segment::field(1)}, .value = value::uint(7)}};
  const patch bad = must(propose_from_observation(wide, wrong, fixture::runtime_origin()));
  transaction t2 = f.submit(bad, fixture::runtime_origin());
  const auto r2 = t2.commit();
  REQUIRE_FALSE(r2.has_value());
  CHECK(r2.error().code == errc::validation_failed);
  CHECK(std::ranges::any_of(
      t2.diagnostics(), [](const diagnostic &d) { return d.stage == validator_stage::schema; }));

  CHECK(must(f.manager().head()).revision_digest() == head);
  CHECK(f.name_of(f.hero) == "hero");
}

TEST_CASE("FAB-INV-BND-01: an observation made on an older revision rebases over disjoint edits "
          "and conflicts with overlapping ones") {
  fixture f;
  const document_projection proj = f.project({"content.scene"});
  const std::array changes{rename(f.hero, "hero (seen)")};
  const patch p = must(propose_from_observation(proj, changes, fixture::runtime_origin()));

  // The document moves on: the villain is edited elsewhere.
  must(tg::commit_ops(f.manager(), tg::tx_origin_of(tg::tx_editor(), "content"),
                      {must(tg::set_name(f.villain, "villain (edited)"))}));
  {
    // Staged on its own base with the default policy, the stale proposal is refused.
    transaction stale = f.submit(p, fixture::runtime_origin());
    const auto r = stale.commit();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == errc::stale_base);
  }
  // With rebase_if_commutes the disjoint observation lands on top.
  transaction t = f.submit(p, fixture::runtime_origin(), commit_policy::rebase_if_commutes);
  must(t.commit());
  CHECK(f.name_of(f.hero) == "hero (seen)");
  CHECK(f.name_of(f.villain) == "villain (edited)");

  // An overlapping edit makes the next proposal conflict: the editor changed the hero first.
  const document_projection fresh = f.project({"content.scene"});
  const std::array again{rename(f.hero, "hero (again)")};
  const patch q = must(propose_from_observation(fresh, again, fixture::runtime_origin()));
  must(tg::commit_ops(f.manager(), tg::tx_origin_of(tg::tx_editor(), "content"),
                      {must(tg::set_name(f.hero, "hero (edited)"))}));
  transaction clash = f.submit(q, fixture::runtime_origin(), commit_policy::rebase_if_commutes);
  const auto r = clash.commit();
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == errc::conflict);
  CHECK(f.name_of(f.hero) == "hero (edited)");
}
