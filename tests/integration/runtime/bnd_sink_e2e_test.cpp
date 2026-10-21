// RT-INV-BND-07 (SL-ARCH-003 ARCH3-R161, ADR-0027): end to end through testing::fabric_host. A
// runtime write is observed, proposed on the link's (stale) projection and committed by the host
// under rebase_if_commutes with origin epistemic class observed and the runtime actor; the F5
// ledger's bypassing count does not move (every write went through a transaction). An observation
// the runtime actor may not write (content.prop) is refused by the host, counted as rejected, and
// the frame and the link carry on.

#include "sl/core/error.hpp"
#include "sl/fabric/f5.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/value.hpp"
#include "sl/runtime/bridge.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"

#include "../../../engine/runtime/testing/fabric_host.hpp"

#include <doctest/doctest.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace link = sl::runtime::link;
namespace rtt = sl::runtime::testing;
namespace ecs = sl::runtime::ecs;
namespace fab = sl::fabric;
using rtt::fabric_host;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

void must_ok(const sl::result<void> &r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
}

} // namespace

TEST_CASE("RT-INV-BND-07: an observation commits as observed under rebase_if_commutes") {
  rtt::rig_options o;
  o.host.levels = 2;
  o.host.bodies = 6;
  const auto rig = must(rtt::link_rig::make(o));
  static_cast<void>(must(rig->ingest()));
  must_ok(rig->link->observe_component(rig->body_component));
  must_ok(rig->build());
  static_cast<void>(must(rig->step()));
  const fab::f5::ledger before = fab::f5::read();

  // The editor moves main on without telling the link: the link's projection is now stale.
  const std::vector<fab::patch_op> edit{
      fabric_host::set_body_field(4, 1, must(fab::value::f32(7.25F)))};
  static_cast<void>(must(rig->host->edit(edit, false)));
  CHECK(rig->link->current_generation() < rig->host->projection().generation());

  // The runtime writes another record; its proposal is based on the older revision.
  ecs::world &w = *rig->stack->world;
  const ecs::entity e = must(rig->entity_of(fabric_host::body_id(1)));
  rtt::bnd_body body = *must(w.get<rtt::bnd_body>(e));
  body.count = 1234;
  must_ok(w.set(e, body));
  static_cast<void>(must(rig->step()));

  const link::link_stats st = rig->link->stats();
  CHECK(st.proposed_patches == 1);
  CHECK(st.proposed_changes == 1);
  CHECK(st.rejected == 0);
  REQUIRE(rig->host->commits() == 1);
  const std::optional<fab::commit_receipt> &last = rig->host->last_receipt();
  REQUIRE(last.has_value());
  const fab::commit_receipt &receipt = *last; // NOLINT(bugprone-unchecked-optional-access): REQUIRE

  const auto revision =
      must(fab::decode_revision(must(rig->host->workspace().store().get(receipt.revision))));
  const auto tx =
      must(fab::decode_tx_record(must(rig->host->workspace().store().get(revision.tx))));
  CHECK(tx.origin.epistemic == fab::epistemic_class::observed);
  CHECK(tx.actor == fabric_host::runtime_actor().id());
  CHECK(tx.origin.subsystem == "03"); // link_config::subsystem "runtime", stored as its owner code

  // The commit's head (the editor's edit and the observation) was posted and rebased.
  CHECK(rig->link->current_generation() == rig->host->projection().generation());
  CHECK(must(w.get<rtt::bnd_body>(e))->count == 1234U);
  const ecs::entity four = must(rig->entity_of(fabric_host::body_id(4)));
  CHECK(must(w.get<rtt::bnd_body>(four))->mass == 7.25F);

  const fab::f5::ledger after = fab::f5::read();
  CHECK(after.bypassing == before.bypassing);
}

TEST_CASE("RT-INV-BND-07: an observation outside the actor's authority is rejected, the frame "
          "continues") {
  rtt::rig_options o;
  o.host.levels = 1;
  o.host.bodies = 2;
  o.host.crates = 2;
  o.host.domains = {"content"};
  o.bind_crate = true;
  const auto rig = must(rtt::link_rig::make(o));
  CHECK(must(rig->ingest()).ingested == 4);
  must_ok(rig->link->observe_component(rig->body_component));
  must_ok(rig->link->observe_component(rig->crate_component));
  must_ok(rig->build());
  static_cast<void>(must(rig->step()));
  const fab::f5::ledger before = fab::f5::read();

  // content.prop: the runtime actor holds write:content.scene only.
  ecs::world &w = *rig->stack->world;
  const ecs::entity crate = must(rig->entity_of(fabric_host::crate_id(0)));
  must_ok(w.set(crate, rtt::bnd_crate{.weight = 50.0F}));
  const sl::result<sl::runtime::frame_stats> frame = rig->step();
  REQUIRE_MESSAGE(frame.has_value(), (frame ? std::string() : frame.error().message));
  CHECK(rig->link->stats().rejected == 1);
  CHECK(rig->link->stats().proposed_patches == 1);
  CHECK(rig->host->commits() == 0);

  // The link keeps going: a write the actor may make commits on the next frame.
  const ecs::entity e = must(rig->entity_of(fabric_host::body_id(0)));
  rtt::bnd_body body = *must(w.get<rtt::bnd_body>(e));
  body.heat = 11;
  must_ok(w.set(e, body));
  static_cast<void>(must(rig->step()));
  CHECK(rig->host->commits() == 1);
  CHECK(rig->link->stats().rejected == 1);
  CHECK(fab::f5::read().bypassing == before.bypassing);
}
