// FAB-INV-IMP-04 (SL-ARCH-001 ARCH1-R58; master plan section 2.4): the impact summary on the
// receipt and event groups affected ids by class. A project of asset, entity, instance and
// derived records linked by fabric.edge records, with tests (validates edges) and build targets
// (builds edges): editing the asset puts a report on the receipt (attachment "fabric.impact")
// that lists every affected id under its id class in (distance, id) order with exact counts, and
// the tests and build targets by type; the manager's own distance-0 summary is untouched. A
// handler of "fabric.tx.committed" gets the same report for the event's tx. The value form
// round-trips, malformed values are refused, the bytes are the same in two identical projects,
// and the summary is capped at 64 ids per class and per role, and truncated by max_nodes.

#include "sl/fabric/codec.hpp"
#include "sl/fabric/deps.hpp"
#include "sl/fabric/events.hpp"
#include "sl/fabric/gen/fabric.edge.gen.hpp"
#include "sl/fabric/gen/fabric.history.gen.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/impact.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/test_schemas.hpp"
#include "../../../engine/fabric/testing/tx_fixture.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
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

constexpr std::array<id_class, 4> classes{id_class::asset, id_class::entity, id_class::instance,
                                          id_class::derived};

type_id class_type(id_class c) { return type_id_of("imp.rec." + std::string(to_string(c))); }
type_id test_type() { return type_id_of("imp.test_case"); }
type_id target_type() { return type_id_of("imp.build_target"); }

sl::result<void> add_type(schema_registry &reg, std::string name, id_class c) {
  type_desc t;
  t.name = std::move(name);
  t.id = type_id_of(t.name);
  t.kind = type_kind::struct_;
  t.domain = "gen";
  t.record_class = c;
  t.state_class = state_class::document;
  t.fields.push_back(field_desc{.no = 1,
                                .name = "revision",
                                .type = type_id_of("u64"),
                                .presence = presence::optional,
                                .default_value = std::nullopt,
                                .since_minor = 0,
                                .deprecated_in = std::nullopt,
                                .ref = std::nullopt,
                                .doc = {}});
  return reg.add(std::move(t));
}

workspace_options project_options() {
  workspace_options o = must(tg::gen_options(std::make_shared<manual_clock>(11), 4));
  REQUIRE(gen::fabric_edge::register_types(o.schemas).has_value());
  for (const id_class c : classes) {
    REQUIRE(add_type(o.schemas, "imp.rec." + std::string(to_string(c)), c).has_value());
  }
  REQUIRE(add_type(o.schemas, "imp.test_case", id_class::entity).has_value());
  REQUIRE(add_type(o.schemas, "imp.build_target", id_class::entity).has_value());
  return o;
}

value empty_body() { return must(value::struct_({})); }

patch_op link(const record_id &from, const record_id &to, gen::fabric_edge::edge_kind kind) {
  const record_id id =
      tg::test_id(id_class::entity, "edge/" + from.to_string() + "/" + to.to_string());
  const gen::fabric_edge::edge e{.from = from, .to = to, .kind = kind};
  return op_create(id, type_id_of("fabric.edge.edge"), must(e.to_value()));
}

struct project {
  record_id a = tg::test_id(id_class::asset, "A");
  record_id e = tg::test_id(id_class::entity, "E");
  record_id i = tg::test_id(id_class::instance, "I");
  record_id d = tg::test_id(id_class::derived, "D");
  record_id t1 = tg::test_id(id_class::entity, "T1");
  record_id t2 = tg::test_id(id_class::entity, "T2");
  record_id b = tg::test_id(id_class::entity, "B");
};

std::vector<patch_op> project_ops(const project &p) {
  using k = gen::fabric_edge::edge_kind;
  return {
      op_create(p.a, class_type(id_class::asset), empty_body()),
      op_create(p.e, class_type(id_class::entity), empty_body()),
      op_create(p.i, class_type(id_class::instance), empty_body()),
      op_create(p.d, class_type(id_class::derived), empty_body()),
      op_create(p.t1, test_type(), empty_body()),
      op_create(p.t2, test_type(), empty_body()),
      op_create(p.b, target_type(), empty_body()),
      link(p.e, p.a, k::reference),
      link(p.i, p.e, k::reference),
      link(p.d, p.a, k::derives_from),
      link(p.t1, p.e, k::validates),
      link(p.t2, p.i, k::validates),
      link(p.b, p.d, k::builds),
  };
}

patch_op touch(const record_id &id, std::uint64_t n) {
  return op_set(id, {path_segment::field(1)}, value::uint(n));
}

impact_report attached(const commit_receipt &r) {
  REQUIRE(r.attachments.contains(std::string(impact_attachment)));
  return must(impact_report_from_value(r.attachments.find(impact_attachment)->second));
}

} // namespace

TEST_CASE("FAB-INV-IMP-04: the impact summary on the receipt and event groups affected ids by "
          "class") {
  auto ws = must(workspace::create_memory(project_options()));
  transaction_manager &m = ws->manager();
  const tx_origin origin = tg::tx_origin_of(tg::tx_editor());
  auto engine = must(impact_engine::attach(m, must(m.head())));
  std::vector<std::optional<impact_report>> from_events;
  std::vector<record_id> event_txs;
  auto sub = m.events().subscribe("fabric.tx.committed", [&](const event_envelope &ev) noexcept {
    from_events.push_back(engine->committed(ev));
    event_txs.push_back(ev.causality.tx.value_or(record_id{}));
  });

  const project p;
  const commit_receipt created = must(tg::commit_ops(m, origin, project_ops(p)));
  const commit_receipt edited = must(tg::commit_ops(m, origin, {touch(p.a, 1)}));
  const impact_report r = attached(edited);

  // Every affected record, grouped by class, in (distance, id) order inside each class.
  CHECK(r.summary.total == 7);
  CHECK_FALSE(r.summary.truncated);
  CHECK(r.at_revision == edited.revision);
  REQUIRE(r.summary.classes.size() == 4);
  CHECK(r.summary.classes[0] == class_impact{.cls = id_class::asset, .count = 1, .first = {p.a}});
  const class_impact &entities = r.summary.classes[1];
  CHECK(entities.cls == id_class::entity);
  CHECK(entities.count == 4);
  REQUIRE(entities.first.size() == 4);
  CHECK(entities.first[0] == p.e); // distance 1
  // distance 2: B (via D) and T1 (via E), by id; T2 at distance 3 (via I).
  CHECK(entities.first[1] == std::min(p.b, p.t1));
  CHECK(entities.first[2] == std::max(p.b, p.t1));
  CHECK(entities.first[3] == p.t2);
  CHECK(r.summary.classes[2] ==
        class_impact{.cls = id_class::instance, .count = 1, .first = {p.i}});
  CHECK(r.summary.classes[3] == class_impact{.cls = id_class::derived, .count = 1, .first = {p.d}});
  for (const class_impact &c : r.summary.classes) {
    for (const record_id &id : c.first) {
      CHECK(id.cls() == c.cls);
    }
  }
  // Tests and build targets by type.
  REQUIRE(r.roles.size() == 2);
  CHECK(r.roles[0] ==
        role_impact{
            .role = impact_role::test, .type = test_type(), .count = 2, .first = {p.t1, p.t2}});
  CHECK(r.roles[1] ==
        role_impact{
            .role = impact_role::build_target, .type = target_type(), .count = 1, .first = {p.b}});
  CHECK(r == must(engine->report(std::vector<record_id>{p.a})));

  // The manager's own summary stays the changed records only (distance 0).
  CHECK(edited.impact.total == 1);
  REQUIRE(edited.impact.classes.size() == 1);
  CHECK(edited.impact.classes[0].first == std::vector<record_id>{p.a});

  // The committed events: the same reports, looked up by the event's tx.
  REQUIRE(from_events.size() == 2);
  CHECK(event_txs == std::vector<record_id>{created.tx.id(), edited.tx.id()});
  CHECK(from_events[0] == attached(created));
  CHECK(from_events[1] == r);

  // A test edited alone affects only itself; its report still names its role and type.
  const impact_report lone = attached(must(tg::commit_ops(m, origin, {touch(p.t2, 2)})));
  CHECK(lone.summary.total == 1);
  REQUIRE(lone.roles.size() == 1);
  CHECK(lone.roles[0].role == impact_role::test);
}

TEST_CASE("FAB-INV-IMP-04: the report value round-trips, refuses malformed values and is "
          "deterministic") {
  const project p;
  std::vector<std::vector<std::uint8_t>> bytes;
  for (int run = 0; run < 2; ++run) {
    auto ws = must(workspace::create_memory(project_options()));
    transaction_manager &m = ws->manager();
    auto engine = must(impact_engine::attach(m, must(m.head())));
    const tx_origin origin = tg::tx_origin_of(tg::tx_editor());
    must(tg::commit_ops(m, origin, project_ops(p)));
    const commit_receipt edited = must(tg::commit_ops(m, origin, {touch(p.a, 1)}));
    const value &v = edited.attachments.find(impact_attachment)->second;
    CHECK(must(to_value(must(impact_report_from_value(v)))) == v);
    bytes.push_back(must(encode(v)));
  }
  CHECK(bytes[0] == bytes[1]);

  impact_report r;
  r.summary.total = 1;
  r.summary.classes.push_back(class_impact{.cls = id_class::asset, .count = 1, .first = {p.a}});
  r.roles.push_back(role_impact{
      .role = impact_role::build_target, .type = target_type(), .count = 1, .first = {p.b}});
  CHECK(must(impact_report_from_value(must(to_value(r)))) == r); // no revision: field 7 absent

  CHECK(impact_report_from_value(value::uint(1)).error().code == errc::type_mismatch);
  CHECK(impact_report_from_value(empty_body()).error().code == errc::type_mismatch);
  const value good = must(to_value(r));
  CHECK(impact_report_from_value(must(good.with({path_segment::field(1)}, value::boolean(true))))
            .error()
            .code == errc::type_mismatch);
  CHECK(impact_report_from_value(must(good.with({path_segment::field(8)}, value::uint(0))))
            .error()
            .code == errc::type_mismatch);
  role_impact bad_role = r.roles[0];
  bad_role.role = static_cast<impact_role>(9);
  impact_report bad = r;
  bad.roles = {bad_role};
  CHECK(impact_report_from_value(must(to_value(bad))).error().code == errc::type_mismatch);
  CHECK(to_string(impact_role::test) == "test");
  CHECK(to_string(impact_role::build_target) == "build_target");
}

TEST_CASE("FAB-INV-IMP-04: the summary lists at most 64 ids per class and role and is cut by "
          "max_nodes") {
  auto ws = must(workspace::create_memory(project_options()));
  transaction_manager &m = ws->manager();
  const tx_origin origin = tg::tx_origin_of(tg::tx_editor());
  impact_options capped;
  capped.max_nodes = 3;
  capped.attachment = "test.impact.capped";
  auto engine = must(impact_engine::attach(m, must(m.head())));
  auto small = must(impact_engine::attach(m, must(m.head()), capped));
  const project p;
  std::vector<patch_op> ops = project_ops(p);
  for (int n = 0; n < 70; ++n) {
    const record_id t = tg::test_id(id_class::entity, "test" + std::to_string(n));
    ops.push_back(op_create(t, test_type(), empty_body()));
    ops.push_back(link(t, p.a, gen::fabric_edge::edge_kind::validates));
  }
  must(tg::commit_ops(m, origin, ops));
  const commit_receipt edited = must(tg::commit_ops(m, origin, {touch(p.a, 1)}));
  const impact_report r = attached(edited);
  CHECK(r.summary.total == 77);
  CHECK(r.summary.classes[1].count == 74);
  CHECK(r.summary.classes[1].first.size() == default_summary_ids);
  REQUIRE(r.roles.size() == 2);
  CHECK(r.roles[0].count == 72);
  CHECK(r.roles[0].first.size() == default_summary_ids);

  // The capped engine: the same commit, cut to the first three nodes in (distance, id) order.
  const std::optional<impact_report> cut_report = small->committed(edited.tx.id());
  REQUIRE(cut_report.has_value());
  const impact_report cut = cut_report.value_or(impact_report{});
  CHECK(cut.summary.truncated);
  CHECK(cut.summary.total == 3);
  const impact_set s = must(small->impact(std::vector<record_id>{p.a}));
  CHECK(s.truncated);
  REQUIRE(s.nodes.size() == 3);
  CHECK(s.nodes[0].id == p.a);
  CHECK(s.nodes[1].distance == 1);
}

TEST_CASE("FAB-INV-IMP-04: the committed event payload stays the tx record; its report is "
          "committed(event), on one seq stream") {
  // WO-064 decision: observers cannot extend the payload without a second seq stream, so the
  // manager publishes the tx record and the impact report is reached through the engine.
  auto ws = must(workspace::create_memory(project_options()));
  transaction_manager &m = ws->manager();
  const tx_origin origin = tg::tx_origin_of(tg::tx_editor());
  auto engine = must(impact_engine::attach(m, must(m.head())));
  std::vector<event_envelope> committed;
  std::vector<std::uint64_t> seqs;
  auto all = m.events().subscribe("", [&](const event_envelope &ev) noexcept {
    seqs.push_back(ev.seq);
    if (ev.type == "fabric.tx.committed") {
      committed.push_back(ev);
    }
  });
  const project p;
  static_cast<void>(must(tg::commit_ops(m, origin, project_ops(p))));
  const commit_receipt edited = must(tg::commit_ops(m, origin, {touch(p.a, 1)}));
  REQUIRE(committed.size() == 2);
  const event_envelope &ev = committed.back();
  REQUIRE(ev.payload.has_value());
  const value payload =
      must(decode(ev.payload.value_or(std::vector<std::uint8_t>{}), unknown_policy::preserve));
  static_cast<void>(must(gen::fabric_history::tx_record::from_value(payload)));
  CHECK(ev.schema.type == type_id_of("fabric.history.tx_record").id());
  CHECK(ev.causality.tx == std::optional<record_id>(edited.tx.id()));
  CHECK(engine->committed(ev) == std::optional<impact_report>(attached(edited)));
  for (std::size_t i = 1; i < seqs.size(); ++i) {
    CHECK(seqs[i] > seqs[i - 1]); // one ordered stream: nothing published beside it
  }
}
