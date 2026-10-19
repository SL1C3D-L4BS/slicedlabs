// FAB-INV-CF-02 (SL-ARCH-001 ARCH1-R69; master plan, Volume 04, episode 079): the generic suite
// passes for every registered type: round trip, canonical order, fixtures, ownership, refs,
// transactional mutation (F5 ledger delta), the inverse law and the migration case
// (engine/fabric/testing/conformance_suite.hpp). The registry is the Volume 04 one
// (builtin_fixture_registry: the generated system modules, the test scene schemas and the WO-057
// migration module); a domain onboards by registering fixtures, and a broken fixture fails only
// its own type.

#include "sl/fabric/codec.hpp"
#include "sl/fabric/f5.hpp"
#include "sl/fabric/migration.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/schema.hpp"

#include "../../../engine/fabric/testing/conformance_registry.hpp"
#include "../../../engine/fabric/testing/conformance_suite.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <map>
#include <print>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace sl::fabric;
using sl::errc;
namespace tg = sl::fabric::testing;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

const tg::conformance_run &builtin_run() {
  static const tg::conformance_run run =
      must(tg::run_conformance(must(tg::builtin_fixture_registry(SL_FABRIC_FIXTURES_DIR))));
  return run;
}

constexpr std::string_view probe_module = R"({"format": "sl-type-def/1", "module": "content.probe",
  "domain": "content.probe", "owner": "05", "doc": "WO-062 onboarding probe",
  "types": [{"name": "content.probe.note", "kind": "struct", "version": "1.0", "record_class": "entity",
  "state_class": "document", "doc": "A note.", "fields": [{"no": 1, "name": "text", "type": "string",
  "presence": "required"}]}]})";

tg::domain_fixtures probe_domain(std::string second_fixture) {
  tg::domain_fixtures d;
  d.name = "content.probe";
  d.add_types = [](schema_registry &reg) -> sl::result<void> {
    auto doc = parse_json(probe_module);
    if (!doc) {
      return std::unexpected(doc.error());
    }
    auto types = types_from_module(*doc);
    if (!types) {
      return std::unexpected(types.error());
    }
    for (type_desc &t : *types) {
      if (auto ok = reg.add(std::move(t)); !ok) {
        return ok;
      }
    }
    return {};
  };
  d.claim = [](domain_registry &reg) {
    return reg.claim(domain_spec{.name = "content.probe",
                                 .owner = subsystem::content,
                                 .state_classes = {state_class::document},
                                 .types = {}});
  };
  d.types = {{.type = "content.probe.note",
              .values = {R"({"text":"lantern"})", std::move(second_fixture)}}};
  return d;
}

} // namespace

TEST_CASE("FAB-INV-CF-02: the generic suite passes for every registered type") {
  const auto registry = must(tg::builtin_fixture_registry(SL_FABRIC_FIXTURES_DIR));
  const tg::conformance_run &run = builtin_run();
  REQUIRE(run.types.size() == registry.type_count());
  std::map<std::string, std::pair<std::size_t, std::size_t>> per_check; // passed, not applicable
  std::size_t failures = 0;
  for (const tg::type_report &t : run.types) {
    for (const tg::check_outcome &c : t.checks) {
      auto &[passed, na] = per_check[c.check];
      passed += c.passed && c.applies ? 1 : 0;
      na += c.applies ? 0 : 1;
      if (!c.passed) {
        ++failures;
        MESSAGE(t.domain << " " << t.type << ": " << c.check << ": " << c.detail);
      }
    }
  }
  for (const auto &[check, counts] : per_check) {
    std::println("conformance: {:<10} passed {}, not applicable {}, of {} types", check,
                 counts.first, counts.second, run.types.size());
  }
  CHECK(failures == 0);
}

TEST_CASE("FAB-INV-CF-02: record types get every case, value types the value cases") {
  const tg::conformance_run &run = builtin_run();
  std::size_t records = 0;
  for (const tg::type_report &t : run.types) {
    REQUIRE(t.checks.size() == 8);
    records += t.record ? 1 : 0;
    for (const tg::check_outcome &c : t.checks) {
      const bool transactional = c.check == "mutation" || c.check == "inverse";
      CHECK_MESSAGE(c.applies == (t.record || !transactional), t.type << " " << c.check);
    }
  }
  CHECK(records > 0);
  CHECK(run.counters.commits > records);
  if (f5::enabled) {
    CHECK(run.counters.ledger.mutations >= run.counters.commits);
    CHECK(run.counters.ledger.bypassing == 0);
  }
}

TEST_CASE(
    "FAB-INV-CF-02: a domain onboards by registering fixtures; a broken fixture fails its type") {
  tg::domain_fixture_registry good;
  REQUIRE(good.add(probe_domain(R"({"text":"valley"})")).has_value());
  const tg::conformance_run ok = must(tg::run_conformance(good));
  REQUIRE(ok.types.size() == 1);
  for (const tg::check_outcome &c : ok.types[0].checks) {
    CHECK_MESSAGE(c.passed, c.check << ": " << c.detail);
  }

  tg::domain_fixture_registry broken;
  REQUIRE(broken.add(probe_domain(R"({"text":"valley","extra":1})")).has_value());
  const tg::conformance_run bad = must(tg::run_conformance(broken));
  REQUIRE(bad.types.size() == 1);
  CHECK_FALSE(bad.types[0].passed());

  tg::domain_fixture_registry refused;
  auto one = probe_domain("{}");
  one.types[0].values.pop_back();
  CHECK(refused.add(std::move(one)).error().code == errc::invalid_argument);
  REQUIRE(refused.add(probe_domain(R"({"text":"a"})")).has_value());
  CHECK(refused.add(probe_domain(R"({"text":"b"})")).error().code == errc::already_exists);
}
