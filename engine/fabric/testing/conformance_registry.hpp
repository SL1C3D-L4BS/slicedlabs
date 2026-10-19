#pragma once

// The domain fixture registry of the architecture conformance suite (WO-062; SL-ARCH-001
// ARCH1-R41 and ARCH1-R69; FAB-INV-CF-02 and FAB-INV-OWN-09; master plan, Volume 04, episode 079).
//
// A domain onboards onto the generic suite by registering one domain_fixtures entry: how to add
// its types to a schema registry, how to claim its domains, two or more canonical-text fixtures
// (ARCH1-R29) per type, and the hooks the transactional cases need (records that strong refs of
// the fixtures point at, the record id of a fixture, the F3 provenance of a derived type, the
// previous module version for the migration case). conformance_suite.hpp then runs every case
// for every registered type in one workspace. Volume 05+ domains add an entry; nothing else in
// the suite changes.
//
// builtin_fixture_registry() registers what Volume 04 has:
//   fabric           the generated system modules (schemas/fabric, WO-045) with the fixtures of
//                    tests/fixtures/fabric/types/<module>/<type>.json
//   content.scene    the test scene, mesh and cooked-mesh schemas (testing/test_schemas.hpp), the
//                    worked example of a Volume 05+ domain: fixtures written here
//   content.demo     the WO-057 migration fixture module (v2, with v1 as the previous version)
//                    and the v2 records of tests/fixtures/fabric/migration/store.json

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/provenance.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric::testing {

/// The fixtures of one type: its qualified name and two or more values in canonical text.
struct type_fixtures {
  std::string type;
  std::vector<std::string> values;
};

/// One onboarded domain (file comment). Only `name`, `add_types`, `claim` and `types` are
/// required; a null hook takes the default named on it.
struct domain_fixtures {
  std::string name;
  /// Adds the domain's types to an unfrozen registry.
  std::function<result<void>(schema_registry &)> add_types;
  /// Claims the domain's domains (the suite binds the types afterwards).
  std::function<result<void>(domain_registry &)> claim;
  std::vector<type_fixtures> types;
  /// Records the fixtures' strong refs need, committed first through `prerequisites_subsystem`.
  /// Null: none.
  std::function<result<std::vector<patch_op>>(const schema_registry &)> prerequisites;
  std::string prerequisites_subsystem;
  /// The id of record fixture `index` (value `body`) of `type`. Null:
  /// derive(record class, well_known(namespace, 1), "conformance/<type>/<index>").
  std::function<result<record_id>(const type_desc &, std::size_t, const value &)> record_id_for;
  /// The F3 provenance of a transaction writing records of `type`. Null: none.
  std::function<std::optional<provenance>(const type_desc &)> provenance_for;
  /// The previous version of the domain's types (migration case). Null: none.
  std::function<result<std::vector<type_desc>>()> previous_types;
};

/// The onboarded domains, in registration order.
class domain_fixture_registry {
public:
  /// `invalid_argument` for an empty name, a missing add_types or claim hook or a type with fewer
  /// than two fixtures; `already_exists` for a repeated domain name or fixture type.
  [[nodiscard]] result<void> add(domain_fixtures d);
  [[nodiscard]] std::span<const domain_fixtures> domains() const noexcept { return domains_; }
  /// The number of types with fixtures over every domain.
  [[nodiscard]] std::size_t type_count() const noexcept;

private:
  std::vector<domain_fixtures> domains_;
};

/// The Volume 04 registry (file comment); `fixtures_dir` is tests/fixtures/fabric. The read and
/// parse errors of the fixture files.
[[nodiscard]] result<domain_fixture_registry>
builtin_fixture_registry(std::string_view fixtures_dir);

/// Compact JSON with members in document order and number tokens as written: the canonical text
/// of a fixture value (tools/fabric/slb_text.py rules).
[[nodiscard]] std::string compact_json(const json_value &j);

/// The whole file at `path`; `not_found` when it cannot be read.
[[nodiscard]] result<std::string> read_text_file(const std::string &path);

} // namespace sl::fabric::testing
