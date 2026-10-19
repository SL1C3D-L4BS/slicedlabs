// FAB-INV-TX-05 (SL-ARCH-002 ARCH2-R05): validation collects every diagnostic of every stage and
// returns them sorted by (stage, record id, path, rule): one patch that breaks a rule at each of
// structural, schema, authority, references and invariants gets all of them, in diagnostic_less
// order, the same from validate() and commit(), and the same on every run.

#include "sl/fabric/clock.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/provenance.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/test_schemas.hpp"
#include "../../../engine/fabric/testing/tx_fixture.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
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

using key = std::tuple<validator_stage, record_id, std::string>;

struct run_result {
  std::vector<diagnostic> dry;
  std::vector<diagnostic> committed;
};

run_result run_once() {
  auto ws =
      must(workspace::create_memory(must(tg::scene_options(std::make_shared<manual_clock>(3)))));
  transaction_manager &m = ws->manager();
  must(tg::commit_ops(m, tg::tx_origin_of(tg::tx_editor()), {must(tg::create_entity("hero"))}));
  tx_options o;
  o.origin = tg::tx_origin_of(tg::tx_agent()); // every write is denied by authority
  auto t = must(m.begin(o));
  // structural (tx-class target) and invariants (id class)
  must_ok(t.stage(
      op_create(well_known(id_class::tx, 3), tg::entity_type(), must(tg::sample_entity("tx")))));
  // schema: no name, no components
  must_ok(t.stage(
      op_create(tg::entity_named("nameless"), tg::entity_type(), must(value::struct_({})))));
  // references: an owned ref to a record that does not exist
  must_ok(t.stage(
      must(tg::create_scene(tg::entity_named("scene"), "scene", {tg::entity_named("missing")}))));
  // invariants: an entity under an asset id
  must_ok(t.stage(
      op_create(tg::asset_named("wrong"), tg::entity_type(), must(tg::sample_entity("wrong")))));
  run_result out;
  out.dry = must(t.validate());
  CHECK(t.state() == tx_state::open);
  const auto r = t.commit();
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == errc::validation_failed);
  out.committed.assign(t.diagnostics().begin(), t.diagnostics().end());
  return out;
}

} // namespace

TEST_CASE("FAB-INV-TX-05: diagnostics are complete and sorted by stage, record, path, rule") {
  const run_result first = run_once();
  const std::vector<diagnostic> &d = first.committed;
  CHECK(std::ranges::is_sorted(d, diagnostic_less));
  CHECK(has_error(d));

  std::set<key> got;
  for (const diagnostic &x : d) {
    got.emplace(x.stage, x.record, x.rule);
  }
  const record_id tx_target = well_known(id_class::tx, 3);
  const std::vector<key> want = {
      {validator_stage::structural, tx_target, "R-STR-TARGET"},
      {validator_stage::schema, tg::entity_named("nameless"), "R-TY-REQUIRED"},
      {validator_stage::authority, tg::entity_named("nameless"), "R-AUTH-DENIED"},
      {validator_stage::authority, tg::entity_named("scene"), "R-AUTH-DENIED"},
      {validator_stage::authority, tg::asset_named("wrong"), "R-AUTH-DENIED"},
      {validator_stage::references, tg::entity_named("scene"), "R-REF-DANGLING"},
      {validator_stage::invariants, tx_target, "R-INV-ID-CLASS"},
      {validator_stage::invariants, tg::asset_named("wrong"), "R-INV-ID-CLASS"},
  };
  for (const key &k : want) {
    CAPTURE(std::get<2>(k));
    CAPTURE(std::get<1>(k).to_string());
    CHECK(got.contains(k));
  }
  // Every stage that ran is represented, in stage order.
  std::vector<validator_stage> stages;
  for (const diagnostic &x : d) {
    if (stages.empty() || stages.back() != x.stage) {
      stages.push_back(x.stage);
    }
  }
  CHECK(stages == std::vector<validator_stage>{validator_stage::structural, validator_stage::schema,
                                               validator_stage::authority,
                                               validator_stage::references,
                                               validator_stage::invariants});
  // Each message starts with the error code it stands for.
  for (const diagnostic &x : d) {
    CHECK(x.message.find(": ") != std::string::npos);
  }
  // The dry run reports exactly what commit does, and a second run is identical.
  CHECK(first.dry == first.committed);
  CHECK(run_once().committed == first.committed);
}

TEST_CASE("FAB-INV-TX-05: the built-in chain documented in transaction.hpp is builtin_validators() "
          "in order") {
  // The names of the doc comment above builtin_validators(), read from the header itself.
  const std::string header = std::string(SL_FABRIC_FIXTURES_DIR) +
                             "/../../../engine/fabric/include/sl/fabric/transaction.hpp";
  std::ifstream in(header);
  REQUIRE(in.good());
  std::vector<std::string> documented;
  bool inside = false;
  for (std::string line; std::getline(in, line);) {
    if (line.starts_with("/// The built-in validator chain")) {
      inside = true;
      continue;
    }
    if (!inside) {
      continue;
    }
    if (!line.starts_with("///")) {
      break;
    }
    // An entry line: "///   fabric.<name>  (<stage>) ..."; continuation lines are indented more.
    if (line.starts_with("///   fabric.")) {
      std::istringstream words(line.substr(3));
      std::string name;
      words >> name;
      documented.push_back(name);
    }
  }
  auto ws =
      must(workspace::create_memory(must(tg::scene_options(std::make_shared<manual_clock>(1)))));
  const fixed_provenance_source provenance;
  const auto chain = must(builtin_validators(builtin_validator_env{
      .domains = &ws->domains(), .actors = &ws->actors(), .provenance = &provenance}));
  std::vector<std::string> built;
  built.reserve(chain.size());
  for (const auto &v : chain) {
    built.emplace_back(v->name());
  }
  CHECK(documented == built);
  CHECK(std::ranges::count(built, "fabric.identity") == 1); // WO-058 appended it
  CHECK(std::ranges::count(built, "fabric.schema.newer") == 1);
}
