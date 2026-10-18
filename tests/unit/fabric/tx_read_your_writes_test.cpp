// FAB-INV-TX-09 (SL-ARCH-002 ARCH2-R05): validators and readers inside a transaction see the
// staged state. The view reads staged writes before the base; a validator registered with the
// workspace sees the patch's effect in ctx.after() and the base in ctx.before(), both in the dry
// run and at commit, including a record created and then edited in the same transaction.

#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/tx_fixture.hpp"

#include <doctest/doctest.h>

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;
namespace tg = sl::fabric::testing;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

void must_ok(const sl::result<void> &r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
}

std::string name_of(const state_reader &s, const record_id &id) {
  const auto r = s.read(id);
  if (!r || !*r) {
    return "<absent>";
  }
  const auto v = (*r)->body.get({path_segment::field(1)});
  return v ? std::string(v->as_string()) : "<no name>";
}

// Records what the after and before states say about two records, per run.
class witness final : public validator {
public:
  struct seen {
    std::string hero_after;
    std::string hero_before;
    std::string newbie_after;
    std::string newbie_before;
  };

  [[nodiscard]] std::string_view name() const noexcept override { return "test.witness"; }
  [[nodiscard]] validator_stage stage() const noexcept override {
    return validator_stage::invariants;
  }
  [[nodiscard]] sl::result<void> run(const validation_context &ctx,
                                     std::vector<diagnostic> & /*out*/) const override {
    const std::scoped_lock lock(mutex_);
    runs_.push_back(seen{.hero_after = name_of(ctx.after(), tg::entity_named("hero")),
                         .hero_before = name_of(ctx.before(), tg::entity_named("hero")),
                         .newbie_after = name_of(ctx.after(), tg::entity_named("newbie")),
                         .newbie_before = name_of(ctx.before(), tg::entity_named("newbie"))});
    return {};
  }
  [[nodiscard]] std::vector<seen> runs() const {
    const std::scoped_lock lock(mutex_);
    return runs_;
  }

private:
  mutable std::mutex mutex_;
  mutable std::vector<seen> runs_;
};

} // namespace

TEST_CASE("FAB-INV-TX-09: validators and readers inside a transaction see staged state") {
  auto w = std::make_shared<witness>();
  workspace_options opts = must(tg::scene_options(std::make_shared<manual_clock>(2)));
  opts.validators.push_back(w);
  auto ws = must(workspace::create_memory(std::move(opts)));
  transaction_manager &m = ws->manager();
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  must(tg::commit_ops(m, editor, {must(tg::create_entity("hero"))}));
  REQUIRE(w->runs().size() == 1);

  tx_options o;
  o.origin = editor;
  auto t = must(m.begin(o));
  must_ok(t.stage(must(tg::set_name(tg::entity_named("hero"), "renamed"))));
  CHECK(name_of(t.view(), tg::entity_named("hero")) == "renamed");
  CHECK(name_of(t.view().base(), tg::entity_named("hero")) == "hero");
  must_ok(t.stage(must(tg::create_entity("newbie"))));
  must_ok(t.stage(must(tg::set_name(tg::entity_named("newbie"), "newbie edited"))));
  CHECK(name_of(t.view(), tg::entity_named("newbie")) == "newbie edited");

  CHECK(must(t.validate()).empty());
  must(t.commit());
  const auto runs = w->runs();
  REQUIRE(runs.size() == 3); // the first commit, the dry run, the commit
  for (std::size_t i = 1; i < runs.size(); ++i) {
    CAPTURE(i);
    CHECK(runs[i].hero_after == "renamed");
    CHECK(runs[i].hero_before == "hero");
    CHECK(runs[i].newbie_after == "newbie edited");
    CHECK(runs[i].newbie_before == "<absent>");
  }
  CHECK(name_of(must(m.head()), tg::entity_named("newbie")) == "newbie edited");
}
