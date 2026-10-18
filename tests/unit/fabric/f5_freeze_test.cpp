// FAB-INV-F5-05 (SL-ARCH-002 ARCH2-R18, ADR-0016): the schema, domain, validator and merge driver
// registries refuse changes after the workspace opens. The workspace freezes the registries it is
// given before anything else and hands them out only as const; a copy of a frozen registry stays
// frozen; the manager's validator and merge driver registries are frozen too, so no type, domain,
// validator or merge rule can be slipped in behind the transaction manager.

#include "sl/core/error.hpp"
#include "sl/fabric/clock.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/tx_fixture.hpp"

#include <doctest/doctest.h>

#include <memory>
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

// No mutating registry call is reachable through the workspace.
template <class W>
concept workspace_adds_type = requires(W &w, type_desc t) { w.schemas().add(std::move(t)); };
template <class W>
concept workspace_claims_domain =
    requires(W &w, domain_spec d) { w.domains().claim(std::move(d)); };
template <class W>
concept workspace_binds_types =
    requires(W &w, const schema_registry &s) { w.domains().bind_types(s); };
template <class W>
concept workspace_refreezes = requires(W &w) { w.schemas().freeze(); };
template <class M>
concept manager_adds_type = requires(M &m, type_desc t) { m.schemas().add(std::move(t)); };
static_assert(!workspace_adds_type<workspace>);
static_assert(!workspace_claims_domain<workspace>);
static_assert(!workspace_binds_types<workspace>);
static_assert(!workspace_refreezes<workspace>);
static_assert(!manager_adds_type<transaction_manager>);

type_desc late_type() {
  type_desc t;
  t.name = "late.Rec";
  t.id = type_id_of(t.name);
  t.kind = type_kind::struct_;
  t.record_class = id_class::entity;
  t.state_class = state_class::document;
  t.domain = "content.scene";
  field_desc f;
  f.no = 1;
  f.name = "name";
  f.type = type_id_of("string");
  t.fields = {f};
  return t;
}

class late_validator final : public validator {
public:
  [[nodiscard]] std::string_view name() const noexcept override { return "test.late"; }
  [[nodiscard]] validator_stage stage() const noexcept override {
    return validator_stage::structural;
  }
  [[nodiscard]] sl::result<void> run(const validation_context & /*ctx*/,
                                     std::vector<diagnostic> & /*out*/) const override {
    return {};
  }
};

class late_driver final : public merge_driver {
public:
  [[nodiscard]] std::string_view name() const noexcept override { return "test.late"; }
  [[nodiscard]] sl::result<merge_result> merge(const merge_input &in) const override {
    return merge_result{.merged = in.ours, .conflicts = {}};
  }
};

std::unique_ptr<workspace> open_scene() {
  auto clock = std::make_shared<manual_clock>(1'000'000);
  workspace_options options = must(tg::scene_options(clock));
  CHECK_FALSE(options.domains.frozen()); // claims are still open before the workspace
  return must(workspace::create_with(std::make_shared<memory_backend>(), std::move(options)));
}

} // namespace

TEST_CASE("FAB-INV-F5-05: the workspace freezes the schema and domain registries when it opens") {
  const auto ws = open_scene();
  CHECK(ws->schemas().frozen());
  CHECK(ws->domains().frozen());
  CHECK(ws->manager().schemas().frozen());
  CHECK(&ws->manager().schemas() == &ws->schemas());
}

TEST_CASE("FAB-INV-F5-05: a copy of a frozen registry refuses types and claims") {
  const auto ws = open_scene();
  schema_registry schemas = ws->schemas();
  const auto added = schemas.add(late_type());
  REQUIRE_FALSE(added.has_value());
  CHECK(added.error().code == errc::schema_frozen);
  CHECK(schemas.find("late.Rec") == nullptr);

  domain_registry domains = ws->domains();
  const auto claimed = domains.claim(domain_spec{.name = "content.late",
                                                 .owner = subsystem::content,
                                                 .state_classes = {state_class::document},
                                                 .types = {}});
  REQUIRE_FALSE(claimed.has_value());
  CHECK(claimed.error().code == errc::registry_frozen);
  CHECK(domains.find("content.late") == nullptr);
  CHECK(ws->domains().find("content.late") == nullptr);
}

TEST_CASE("FAB-INV-F5-05: the manager's validator and merge driver registries are frozen") {
  const auto ws = open_scene();
  transaction_manager &m = ws->manager();
  CHECK(m.validators().frozen());
  const std::size_t validators = m.validators().size();
  const auto v = m.validators().add(std::make_shared<late_validator>());
  REQUIRE_FALSE(v.has_value());
  CHECK(v.error().code == errc::registry_frozen);
  CHECK(m.validators().size() == validators);

  CHECK(m.merge_drivers().frozen());
  const auto d = m.merge_drivers().add(tg::entity_type(), std::make_shared<late_driver>());
  REQUIRE_FALSE(d.has_value());
  CHECK(d.error().code == errc::registry_frozen);
  CHECK(m.merge_drivers().driver_for(tg::entity_type()).name() != "test.late");
}
