// FAB-INV-TX-13 (SL-ARCH-002 ARCH2-R14; master plan section 3, F3): a derived record whose
// provenance lacks any F3 field is refused at the invariants stage (R-PRV-INCOMPLETE,
// provenance_incomplete) on every commit path: a plain commit, the dry run, and a commit rebased
// under rebase_if_commutes. With complete provenance on the origin the commit goes through and the
// tx record carries it; an epistemic class change outside the matrix is refused the same way.

#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/provenance.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/tx_fixture.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>

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

struct fixture {
  std::shared_ptr<memory_backend> store = std::make_shared<memory_backend>();
  std::unique_ptr<workspace> ws;
  record_id mesh = tg::asset_named("rock");

  fixture() {
    ws = must(
        workspace::create_with(store, must(tg::scene_options(std::make_shared<manual_clock>(6)))));
    must(tg::commit_ops(ws->manager(), tg::tx_origin_of(tg::tx_editor(), "content"),
                        {must(tg::create_mesh("rock", 128))}));
  }

  // A cook transaction through subsystem asset, with `p` on its origin.
  [[nodiscard]] transaction cook(std::optional<provenance> p,
                                 commit_policy policy = commit_policy::reject_stale) const {
    tx_options o;
    o.origin = tg::tx_origin_of(tg::tx_editor(), "asset");
    o.origin.provenance = std::move(p);
    o.origin.epistemic = epistemic_class::calculated;
    o.policy = policy;
    auto t = must(ws->manager().begin(o));
    must_ok(t.stage(must(tg::create_cooked("rock", mesh))));
    return t;
  }
};

bool refused_incomplete(const transaction &t) {
  return std::ranges::any_of(t.diagnostics(), [](const diagnostic &d) {
    return d.rule == "R-PRV-INCOMPLETE" && d.stage == validator_stage::invariants &&
           d.message.starts_with("provenance_incomplete") && d.record == tg::derived_named("rock");
  });
}

} // namespace

TEST_CASE("FAB-INV-TX-13: a derived record without complete provenance is refused at the "
          "invariants stage") {
  fixture f;
  const std::optional<digest> head = must(f.ws->manager().head()).revision_digest();
  provenance gap = tg::complete_provenance(f.mesh);
  gap.toolchain_id.reset();
  provenance zero = tg::complete_provenance(f.mesh);
  zero.environment_hash = digest{}; // the all-zero digest counts as missing
  for (const std::optional<provenance> &p :
       {std::optional<provenance>{}, std::optional<provenance>{gap},
        std::optional<provenance>{zero}}) {
    auto t = f.cook(p);
    const auto dry = must(t.validate()); // the dry run reports it too
    CHECK(has_error(dry));
    const auto r = t.commit();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == errc::validation_failed);
    CHECK(refused_incomplete(t));
    CHECK(must(f.ws->manager().head()).revision_digest() == head);
  }
}

TEST_CASE("FAB-INV-TX-13: the refusal holds on the rebase path, and complete provenance rides on "
          "the tx record") {
  fixture f;
  auto rebased = f.cook(std::nullopt, commit_policy::rebase_if_commutes);
  must(tg::commit_ops(f.ws->manager(), tg::tx_origin_of(tg::tx_editor()),
                      {must(tg::create_entity("bystander"))}));
  const auto r = rebased.commit();
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == errc::validation_failed);
  CHECK(refused_incomplete(rebased));

  const provenance full = tg::complete_provenance(f.mesh);
  auto ok = f.cook(full);
  const commit_receipt receipt = must(ok.commit());
  const auto rev = must(decode_revision(must(f.store->get(receipt.revision))));
  const auto tx = must(decode_tx_record(must(f.store->get(rev.tx))));
  REQUIRE(tx.provenance.has_value());
  CHECK(tx.provenance == std::optional<provenance>(full));
  CHECK(tx.origin.subsystem == "08");
  CHECK(tx.origin.epistemic == epistemic_class::calculated);
}

TEST_CASE("FAB-INV-TX-13: a class change outside the matrix is refused at the invariants stage") {
  fixture f;
  tx_options o;
  o.origin = tg::tx_origin_of(tg::tx_editor());
  o.origin.class_changes.push_back(class_change{
      .record = f.mesh, .from = epistemic_class::accepted, .to = epistemic_class::inferred});
  auto t = must(f.ws->manager().begin(o));
  must_ok(t.stage(op_set(f.mesh, {path_segment::field(2)}, value::uint(256))));
  const auto r = t.commit();
  REQUIRE_FALSE(r.has_value());
  CHECK(std::ranges::any_of(t.diagnostics(), [](const diagnostic &d) {
    return d.rule == "R-PRV-TRANSITION" && d.stage == validator_stage::invariants;
  }));
}
