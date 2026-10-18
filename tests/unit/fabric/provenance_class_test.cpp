// FAB-INV-PRV-03 (SL-ARCH-001 ARCH1-R42): the epistemic transition matrix is exact over all 36
// pairs; promotion needs accept:<domain>; observed is immutable. The matrix is data
// (legal_transitions) and is tested here against an independent literal.

#include "sl/core/error.hpp"
#include "sl/fabric/authority.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/provenance.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/value.hpp"

#include "../../../engine/fabric/testing/model_store.hpp"
#include "../../../engine/fabric/testing/test_schemas.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;
namespace tg = sl::fabric::testing;
using ec = epistemic_class;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

void must_ok(const sl::result<void> &r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
}

// Rows are `from`, columns `to`, both in class number order: observed, imported, asserted,
// calculated, inferred, accepted. 1 = legal. Written out by hand from SL-ARCH-001.
std::vector<std::vector<int>> expected_matrix() {
  return {
      {0, 0, 0, 0, 0, 0}, // observed: immutable
      {0, 0, 0, 0, 0, 1}, // imported
      {0, 0, 0, 0, 0, 1}, // asserted
      {0, 0, 0, 0, 0, 1}, // calculated
      {0, 0, 0, 0, 0, 1}, // inferred
      {0, 0, 0, 0, 0, 0}, // accepted: final
  };
}

std::vector<ec> classes() { return {epistemic_classes.begin(), epistemic_classes.end()}; }

actor make_actor(std::string_view key, std::vector<std::string> grants,
                 actor_kind kind = actor_kind::human) {
  return actor{.id = must(actor_id::from(tg::test_id(id_class::actor, key))),
               .kind = kind,
               .grants = std::move(grants)};
}

} // namespace

TEST_CASE("FAB-INV-PRV-03: the matrix data equals the SL-ARCH-001 table") {
  CHECK(legal_transitions.size() == 4);
  for (const class_transition &t : legal_transitions) {
    CHECK(t.to == ec::accepted);
    CHECK(t.from != ec::observed);
    CHECK(t.from != ec::accepted);
  }
  const std::vector<ec> all = classes();
  REQUIRE(all.size() == 6);
  for (std::size_t i = 0; i < 6; ++i) {
    CHECK(static_cast<std::size_t>(all[i]) == i + 1);
  }
  CHECK(to_string(ec::observed) == "observed");
  CHECK(to_string(ec::imported) == "imported");
  CHECK(to_string(ec::asserted) == "asserted");
  CHECK(to_string(ec::calculated) == "calculated");
  CHECK(to_string(ec::inferred) == "inferred");
  CHECK(to_string(ec::accepted) == "accepted");
  CHECK(to_string(static_cast<ec>(0)) == "unknown");
  CHECK(to_string(static_cast<ec>(7)) == "unknown");
}

TEST_CASE("FAB-INV-PRV-03: all 36 pairs, with and without the accept grant") {
  const actor granted = make_actor("cls/granted", {"accept:asset.cooked"});
  const actor ungranted = make_actor("cls/ungranted", {"write:asset.cooked"});
  const actor elsewhere = make_actor("cls/elsewhere", {"accept:content.mesh"});
  const std::vector<ec> all = classes();
  const std::vector<std::vector<int>> expected = expected_matrix();
  std::size_t legal = 0;
  std::size_t pairs = 0;
  for (std::size_t f = 0; f < 6; ++f) {
    for (std::size_t t = 0; t < 6; ++t) {
      const ec from = all[f];
      const ec to = all[t];
      CAPTURE(to_string(from));
      CAPTURE(to_string(to));
      ++pairs;
      CHECK(is_legal_transition(from, to) == (expected[f][t] == 1));
      const auto ok = check_transition(from, to, granted, "asset.cooked");
      const auto denied = check_transition(from, to, ungranted, "asset.cooked");
      const auto wrong = check_transition(from, to, elsewhere, "asset.cooked");
      if (expected[f][t] == 1) {
        ++legal;
        CHECK(ok.has_value());
        REQUIRE_FALSE(denied.has_value());
        CHECK(denied.error().code == sl::errc::promotion_denied);
        CHECK(denied.error().message.contains("accept:asset.cooked"));
        REQUIRE_FALSE(wrong.has_value());
        CHECK(wrong.error().code == sl::errc::promotion_denied);
      } else {
        // Outside the matrix: refused whoever asks, grant or not.
        for (const auto &r : {ok, denied, wrong}) {
          REQUIRE_FALSE(r.has_value());
          CHECK(r.error().code == sl::errc::invariant_violated);
        }
      }
    }
  }
  CHECK(pairs == 36);
  CHECK(legal == 4);
}

TEST_CASE("FAB-INV-PRV-03: a grant covers descendants, never siblings or other verbs") {
  const actor parent = make_actor("cls/parent", {"accept:asset"});
  CHECK(check_transition(ec::inferred, ec::accepted, parent, "asset.cooked").has_value());
  CHECK(check_transition(ec::inferred, ec::accepted, parent, "asset").has_value());
  const actor child = make_actor("cls/child", {"accept:asset.cooked"});
  CHECK(check_transition(ec::inferred, ec::accepted, child, "asset").error().code ==
        sl::errc::promotion_denied);
  CHECK(check_transition(ec::inferred, ec::accepted, child, "asset.cooked.lods").has_value());
  // A write grant is not an accept grant, and neither is an empty domain.
  const actor writer = make_actor("cls/writer", {"write:asset"});
  CHECK(check_transition(ec::asserted, ec::accepted, writer, "asset.cooked").error().code ==
        sl::errc::promotion_denied);
  CHECK(check_transition(ec::asserted, ec::accepted, parent, "").error().code ==
        sl::errc::promotion_denied);
}

TEST_CASE("FAB-INV-PRV-03: agent and service actors need the explicit grant too") {
  for (const actor_kind k : {actor_kind::agent, actor_kind::service, actor_kind::tool}) {
    CAPTURE(to_string(k));
    CHECK(check_transition(ec::calculated, ec::accepted, make_actor("cls/k", {}, k), "asset.cooked")
              .error()
              .code == sl::errc::promotion_denied);
    CHECK(check_transition(ec::calculated, ec::accepted, make_actor("cls/k2", {"accept:asset"}, k),
                           "asset.cooked")
              .has_value());
  }
}

TEST_CASE("FAB-INV-PRV-03: observed is immutable and accepted is final, with the reason") {
  const actor granted = make_actor("cls/g", {"accept:asset"});
  const auto obs = check_transition(ec::observed, ec::accepted, granted, "asset.cooked");
  REQUIRE_FALSE(obs.has_value());
  CHECK(obs.error().message.contains("immutable"));
  const auto demote = check_transition(ec::accepted, ec::asserted, granted, "asset.cooked");
  REQUIRE_FALSE(demote.has_value());
  CHECK(demote.error().message.contains("final"));
  const auto same = check_transition(ec::asserted, ec::asserted, granted, "asset.cooked");
  REQUIRE_FALSE(same.has_value());
  CHECK(same.error().message.contains("not a transition"));
}

// ---- The validator -----------------------------------------------------------------------

namespace {

domain_registry make_domains() {
  domain_registry reg;
  must_ok(reg.claim(domain_spec{.name = "asset.cooked",
                                .owner = subsystem::asset,
                                .state_classes = {state_class::derived},
                                .types = {}}));
  must_ok(reg.freeze());
  return reg;
}

const record_id cooked_id = tg::test_id(id_class::derived, "cls/cooked");

class_change change(ec from, ec to) {
  return class_change{.record = cooked_id, .from = from, .to = to};
}

std::vector<diagnostic> judge(const actor &who, const std::vector<class_change> &changes,
                              bool register_actor = true) {
  static const schema_registry schemas = must(tg::make_test_registry());
  static const domain_registry domains = make_domains();
  tg::oracle_store base;
  base.put(cooked_id, record_state{.type = type_id_of(tg::test_types::cooked_mesh),
                                   .body = must(value::struct_({}))});
  actor_directory directory;
  if (register_actor) {
    must_ok(directory.add(who));
  }
  fixed_provenance_source source;
  for (const class_change &c : changes) {
    source.add_change(c);
  }
  patch p;
  p.ops.push_back(op_annotate(must(value::string("promote"))));
  const patch n = must(normalize(p, base));
  staged_state staged(base);
  must_ok(staged.apply(n));
  const validation_context ctx(n, staged, schemas,
                               edit_origin{.actor = who.id, .subsystem = "asset"});
  const provenance_validator v(source, directory, domains);
  std::vector<diagnostic> out;
  must_ok(v.run(ctx, out));
  return out;
}

} // namespace

TEST_CASE(
    "FAB-INV-PRV-03: the validator refuses an illegal transition and an ungranted promotion") {
  const actor granted = make_actor("cls/v1", {"accept:asset.cooked"});
  const actor ungranted = make_actor("cls/v2", {});

  CHECK(judge(granted, {change(ec::calculated, ec::accepted)}).empty());

  const auto denied = judge(ungranted, {change(ec::calculated, ec::accepted)});
  REQUIRE(denied.size() == 1);
  CHECK(denied[0].rule == "R-PRV-PROMOTION");
  CHECK(denied[0].record == cooked_id);
  CHECK(denied[0].stage == validator_stage::invariants);
  CHECK(denied[0].message.contains("promotion_denied"));
  CHECK(denied[0].message.contains("accept:asset.cooked"));

  for (const class_change &bad :
       {change(ec::observed, ec::accepted), change(ec::accepted, ec::asserted),
        change(ec::asserted, ec::calculated), change(ec::inferred, ec::inferred)}) {
    CAPTURE(to_string(bad.from));
    CAPTURE(to_string(bad.to));
    const auto ds = judge(granted, {bad});
    REQUIRE(ds.size() == 1);
    CHECK(ds[0].rule == "R-PRV-TRANSITION");
    CHECK(ds[0].message.contains("invariant_violated"));
  }

  // Every change is judged; one diagnostic each.
  const auto many =
      judge(ungranted, {change(ec::asserted, ec::accepted), change(ec::observed, ec::accepted)});
  CHECK(many.size() == 2);

  // An actor outside the directory is denied.
  const auto stranger = judge(granted, {change(ec::asserted, ec::accepted)}, false);
  REQUIRE(stranger.size() == 1);
  CHECK(stranger[0].rule == "R-PRV-ACTOR");

  // No changes, no findings.
  CHECK(judge(ungranted, {}).empty());
}
