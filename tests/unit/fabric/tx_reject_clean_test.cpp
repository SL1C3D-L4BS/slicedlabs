// FAB-INV-TX-04 (SL-ARCH-002 ARCH2-R05, master plan section 2.4 "Structured rejection, nothing
// written"): a transaction rejected at each validation stage (structural, schema, authority,
// references, invariants, policy) or refused by the lock authority (the locks stage) leaves the
// store byte-identical: on memory_backend the object count, sequence and refs are unchanged, on
// log_backend every file of the store has the same bytes.

#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/test_schemas.hpp"
#include "../../../engine/fabric/testing/tx_fixture.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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

// Volume 20 approval hooks stand-in: refuses any patch labelled "forbidden".
class label_policy final : public validator {
public:
  [[nodiscard]] std::string_view name() const noexcept override { return "test.policy"; }
  [[nodiscard]] validator_stage stage() const noexcept override { return validator_stage::policy; }
  [[nodiscard]] sl::result<void> run(const validation_context &ctx,
                                     std::vector<diagnostic> &out) const override {
    if (ctx.patch().meta.label == "forbidden") {
      out.push_back(diagnostic{.rule = "R-TEST-POLICY",
                               .severity = severity::error,
                               .record = record_id{},
                               .path = {},
                               .message = "approval_required: the label is forbidden",
                               .stage = validator_stage::policy});
    }
    return {};
  }
};

class deny_all final : public lock_authority {
public:
  [[nodiscard]] sl::result<void> check(const edit_origin & /*origin*/,
                                       const footprint & /*fp*/) const override {
    return sl::make_error(errc::locked, "everything is locked by a test");
  }
};

struct scenario {
  std::string name;
  std::optional<validator_stage> stage; // nullopt: refused by the lock authority
  actor_id actor;
  std::string label;
  std::function<std::vector<patch_op>()> ops;
};

std::vector<scenario> scenarios() {
  const record_id hero = tg::entity_named("hero");
  return {
      {.name = "structural",
       .stage = validator_stage::structural,
       .actor = tg::tx_editor(),
       .label = "",
       .ops =
           [] {
             return std::vector<patch_op>{op_create(well_known(id_class::tx, 9), tg::entity_type(),
                                                    must(tg::sample_entity("tx")))};
           }},
      {.name = "schema",
       .stage = validator_stage::schema,
       .actor = tg::tx_editor(),
       .label = "",
       .ops =
           [] {
             const value nameless = must(value::struct_({}));
             return std::vector<patch_op>{
                 op_create(tg::entity_named("nameless"), tg::entity_type(), nameless)};
           }},
      {.name = "authority",
       .stage = validator_stage::authority,
       .actor = tg::tx_agent(),
       .label = "",
       .ops = [hero] { return std::vector<patch_op>{must(tg::set_name(hero, "by agent"))}; }},
      {.name = "references",
       .stage = validator_stage::references,
       .actor = tg::tx_editor(),
       .label = "",
       .ops =
           [] {
             return std::vector<patch_op>{must(tg::create_scene(tg::entity_named("scene"), "scene",
                                                                {tg::entity_named("missing")}))};
           }},
      {.name = "invariants",
       .stage = validator_stage::invariants,
       .actor = tg::tx_editor(),
       .label = "",
       .ops =
           [] {
             return std::vector<patch_op>{op_create(tg::asset_named("wrong-class"),
                                                    tg::entity_type(),
                                                    must(tg::sample_entity("wrong-class")))};
           }},
      {.name = "policy",
       .stage = validator_stage::policy,
       .actor = tg::tx_editor(),
       .label = "forbidden",
       .ops = [hero] { return std::vector<patch_op>{must(tg::set_name(hero, "renamed"))}; }},
      {.name = "locks",
       .stage = std::nullopt,
       .actor = tg::tx_editor(),
       .label = "",
       .ops = [hero] { return std::vector<patch_op>{must(tg::set_name(hero, "renamed"))}; }},
  };
}

std::unique_ptr<workspace> open(const std::shared_ptr<store_backend> &store) {
  workspace_options o = must(tg::scene_options(std::make_shared<manual_clock>(7)));
  o.validators.push_back(std::make_shared<label_policy>());
  auto ws = must(workspace::create_with(store, std::move(o)));
  must(tg::commit_ops(ws->manager(), tg::tx_origin_of(tg::tx_editor()),
                      {must(tg::create_entity("hero"))}));
  return ws;
}

void attempt(transaction_manager &m, const scenario &s) {
  if (!s.stage) {
    m.set_lock_authority(std::make_shared<deny_all>());
  }
  tx_options o;
  o.origin = tg::tx_origin_of(s.actor);
  o.meta.label = s.label;
  auto t = must(m.begin(o));
  for (const patch_op &op : s.ops()) {
    must_ok(t.stage(op));
  }
  const auto r = t.commit();
  REQUIRE_FALSE(r.has_value());
  if (s.stage) {
    CHECK(r.error().code == errc::validation_failed);
    CHECK(t.state() == tx_state::rejected);
    REQUIRE_FALSE(t.diagnostics().empty());
    CHECK(t.diagnostics().front().stage == *s.stage); // sorted: the earliest stage first
  } else {
    CHECK(r.error().code == errc::locked);
    CHECK(t.state() == tx_state::conflict);
    REQUIRE(t.conflict().has_value());
    CHECK(t.conflict().value_or(conflict_report{}).code == errc::locked);
  }
  m.set_lock_authority(nullptr);
}

} // namespace

TEST_CASE("FAB-INV-TX-04: a transaction rejected at each stage leaves memory_backend unchanged") {
  for (const scenario &s : scenarios()) {
    CAPTURE(s.name);
    auto store = std::make_shared<memory_backend>();
    auto ws = open(store);
    const auto objects = store->object_count();
    const auto seq = store->last_seq();
    const auto refs = must(store->list_refs());
    const auto intents = store->intents();
    attempt(ws->manager(), s);
    CHECK(store->object_count() == objects);
    CHECK(store->last_seq() == seq);
    CHECK(must(store->list_refs()) == refs);
    CHECK(store->intents() == intents);
  }
}

TEST_CASE("FAB-INV-TX-04: a transaction rejected at each stage leaves the log store "
          "byte-identical") {
  for (const scenario &s : scenarios()) {
    CAPTURE(s.name);
    tg::tx_log_store log;
    auto ws = open(must(log.open()));
    const auto image = must(tg::file_image(log.memory(), "/ws"));
    REQUIRE_FALSE(image.empty());
    attempt(ws->manager(), s);
    CHECK(must(tg::file_image(log.memory(), "/ws")) == image);
  }
}
