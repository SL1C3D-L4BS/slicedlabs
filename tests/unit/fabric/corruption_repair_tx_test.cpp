// FAB-INV-COR-04 (SL-ARCH-002 ARCH2-R42): repairs are transactions with epistemic class inferred
// and never invent data. A schema-invalid body planted on e1 gets one proposal restoring the body
// of the newest ancestor revision where e1 verifies; applying it commits one transaction whose tx
// record has origin epistemic inferred and a repair label, after which e1 has its old body and fsck
// is clean. A dangling strong reference to a record that an ancestor still holds gets a create of
// that record with its old body. A record with no verifiable earlier version gets no proposal; no
// proposals, and a proposal whose op does not carry its source body, are refused and commit
// nothing.

#include "sl/core/error.hpp"
#include "sl/fabric/corruption.hpp"
#include "sl/fabric/fsck.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/provenance.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/corrupt_store.hpp"
#include "../../../engine/fabric/testing/tx_fixture.hpp"

#include <doctest/doctest.h>

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

std::vector<finding> findings_of(const tg::sample_store &s, const tg::sample_config &cfg) {
  const workspace_options o = must(tg::sample_options(s.fs, cfg));
  return must(fsck(*s.fs, s.root, fsck_options{.schemas = &o.schemas, .probe_lock = false}))
      .findings;
}

repair_origin repairer() { return repair_origin{.actor = tg::tx_editor(), .subsystem = "content"}; }

void inferred_tx(const store_backend &store, const digest &revision) {
  const auto rev = must(decode_revision(must(store.get(revision))));
  const auto tx = must(decode_tx_record(must(store.get(rev.tx))));
  CHECK(tx.origin.epistemic == epistemic_class::inferred);
  CHECK(tx.label.starts_with("repair"));
}

} // namespace

TEST_CASE("FAB-INV-COR-04: a schema-invalid body is restored from the newest verifiable "
          "ancestor by an inferred transaction") {
  const tg::sample_config cfg;
  const tg::sample_store s = must(tg::make_sample_store(cfg));
  const record_id e1 = tg::entity_named("e1");
  record_object good;
  {
    auto ws = must(tg::open_sample(s, cfg));
    good = must(tg::record_at(ws->store(), s.heads.back(), e1));
  }
  record_object bad = good;
  bad.body = value::uint(7);
  const digest planted = must(tg::plant_record(s, e1, must(encode_record_object(bad)), cfg));
  const std::vector<finding> found = findings_of(s, cfg);
  REQUIRE(found.size() == 1);
  REQUIRE(found[0].cls == corruption_class::c7);

  auto ws = must(tg::open_sample(s, cfg));
  const auto proposals = must(propose_repairs(ws->store(), planted, found, &ws->schemas()));
  REQUIRE(proposals.size() == 1);
  const repair_proposal &p = proposals[0];
  CHECK(p.cls == corruption_class::c7);
  CHECK(p.record == e1);
  CHECK(p.source == repair_source::ancestor_revision);
  CHECK(p.source_revision == s.heads.back());
  CHECK(p.source_object == must(must(snapshot::open(ws->store(), s.heads.back())).get(e1)).object);
  CHECK(p.op.kind == op_kind::set);
  CHECK(p.op.target == e1);
  CHECK(p.op.path.empty());
  REQUIRE(p.op.new_value.has_value());
  CHECK(p.op.new_value.value_or(value{}) == good.body); // nothing invented: the exact body

  const commit_receipt r = must(apply_repairs(ws->manager(), proposals, repairer()));
  inferred_tx(ws->store(), r.revision);
  CHECK(must(tg::record_at(ws->store(), r.revision, e1)).body == good.body);
  ws.reset();
  CHECK(findings_of(s, cfg).empty());
}

TEST_CASE("FAB-INV-COR-04: a dangling strong reference is repaired by recreating the target "
          "from an ancestor") {
  const tg::sample_config cfg;
  const tg::sample_store s = must(tg::make_sample_store(cfg));
  const record_id m0 = tg::asset_named("m0");
  const patch_op cook = must(tg::create_cooked("c4", m0));
  record_object mesh;
  {
    auto ws = must(tg::open_sample(s, cfg));
    mesh = must(tg::record_at(ws->store(), s.heads.back(), m0));
    const type_desc *t = ws->schemas().find(cook.type);
    REQUIRE(t != nullptr);
    const auto version = t->version; // t lives in ws, which closes next
    ws.reset();
    const value body = cook.new_value.value_or(value{});
    (void)must(tg::plant_record(s, cook.target,
                                must(encode_record_object(record_object{.id = cook.target,
                                                                        .type = cook.type,
                                                                        .document = cook.target,
                                                                        .version = version,
                                                                        .body = body})),
                                cfg));
  }
  std::vector<tree_change> drop;
  drop.push_back(tree_change{.id = m0, .entry = std::nullopt});
  const digest head = must(tg::rewrite_sample(s, drop, {}, cfg));
  const std::vector<finding> found = findings_of(s, cfg);
  REQUIRE(found.size() == 1);
  REQUIRE(found[0].cls == corruption_class::c9);
  CHECK(found[0].target == m0);

  auto ws = must(tg::open_sample(s, cfg));
  const auto proposals = must(propose_repairs(ws->store(), head, found, &ws->schemas()));
  REQUIRE(proposals.size() == 1);
  CHECK(proposals[0].record == m0);
  CHECK(proposals[0].op.kind == op_kind::create_record);
  CHECK(proposals[0].op.type == mesh.type);
  CHECK(proposals[0].op.new_value == mesh.body);
  const commit_receipt r = must(apply_repairs(ws->manager(), proposals, repairer()));
  inferred_tx(ws->store(), r.revision);
  CHECK(must(tg::record_at(ws->store(), r.revision, m0)).body == mesh.body);
  ws.reset();
  CHECK(findings_of(s, cfg).empty());
}

TEST_CASE("FAB-INV-COR-04: no verifiable earlier version means no proposal; refused repairs "
          "commit nothing") {
  const tg::sample_config cfg;
  const tg::sample_store s = must(tg::make_sample_store(cfg));
  const record_id e1 = tg::entity_named("e1");
  record_object fresh;
  {
    auto ws = must(tg::open_sample(s, cfg));
    fresh = must(tg::record_at(ws->store(), s.heads.back(), e1));
  }
  fresh.id = tg::entity_named("born-broken");
  fresh.body = value::uint(1);
  const digest head = must(tg::plant_record(s, fresh.id, must(encode_record_object(fresh)), cfg));
  const std::vector<finding> found = findings_of(s, cfg);
  REQUIRE(found.size() == 1);
  CHECK(found[0].where.record == fresh.id);

  auto ws = must(tg::open_sample(s, cfg));
  CHECK(must(propose_repairs(ws->store(), head, found, &ws->schemas())).empty());

  const auto none = apply_repairs(ws->manager(), {}, repairer());
  REQUIRE_FALSE(none.has_value());
  CHECK(none.error().code == errc::invalid_argument);

  // A proposal for e1 (a valid record) from its parent, then tampered: refused, nothing written.
  finding f;
  f.cls = corruption_class::c7;
  f.where.record = e1;
  std::vector<repair_proposal> proposals =
      must(propose_repairs(ws->store(), head, std::vector<finding>{f}, &ws->schemas()));
  REQUIRE(proposals.size() == 1);
  proposals[0].op.new_value = value::uint(99);
  const auto tampered = apply_repairs(ws->manager(), proposals, repairer());
  REQUIRE_FALSE(tampered.has_value());
  CHECK(tampered.error().code == errc::object_corrupt);
  CHECK(must(ws->store().read_ref("main")) == head);
}
