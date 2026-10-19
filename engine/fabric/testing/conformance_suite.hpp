#pragma once

// The generic per-type conformance suite and the release gate 1 measure (WO-062; SL-ARCH-001
// ARCH1-R41 and ARCH1-R69; SL-ARCH-002 ARCH2-R01, ARCH2-R17 and ARCH2-R20; FAB-INV-CF-02,
// FAB-INV-OWN-09; master plan section 3 F2 and F5, section 6.1 and Release gates, gate 1).
//
// run_conformance() builds one memory workspace holding every domain of a
// domain_fixture_registry (types added, domains claimed and bound, one human editor, a manual
// clock, a deterministic id source, a probe validator counting validation passes and an observer
// counting changed records), then runs, for every registered type in registration order:
//   fixtures     two or more fixtures, and the type is registered
//   round_trip   from_text, encode, strict schema-directed decode and to_text give the value back
//   canonical    re-encoding the decoded bytes gives the same bytes, the fixture text is the
//                canonical text of its value, and the fixtures differ pairwise
//   ownership    the type has a domain bound to exactly one claimed owner (F2)
//   refs         schema_registry::check accepts every fixture (presence, ranges, ref targets)
//   mutation     record types: the fixtures are created by one transaction through the owner
//                subsystem; the F5 ledger counts mutations and no bypass (compiled-out ledger:
//                skipped); the records are live afterwards
//   inverse      record types: over the head, set(body = fixture 1) then its inverse, and
//                delete then its inverse, restore the staged records exactly (patch layer,
//                FAB-INV-UNDO-01 law)
//   migration    classify(type, type) is identical; with a previous module version, the change
//                from it is classified and not forbidden
// Then the workload: one edit transaction per record type, undo and redo of the last one, and
// one delete per record type (tombstones). The gate 1 measure counts the live records of the
// head without an owner, a stable id or a revision (measure_gate1); the run carries it with the
// F2 report, the F5 ledger delta, commits, objects written, validator invocations and changed
// records for the scorecard (scorecard_input_json, tools/fabric/scorecard.py).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/f5.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"

#include "conformance_registry.hpp"

namespace sl::fabric::testing {

/// One case of one type.
struct check_outcome {
  std::string check;
  bool applies = true; // false: not applicable to this type (passed stays true)
  bool passed = true;
  std::string detail;
};

/// Every case of one type.
struct type_report {
  std::string domain;
  std::string type;
  bool record = false;
  std::size_t fixtures = 0;
  std::vector<check_outcome> checks;

  [[nodiscard]] bool passed() const noexcept;
};

/// What the workload did, for the scorecard (SL-ARCH-002 Measures: objects per commit, validator
/// invocations at most changed records times validators).
struct workload_counters {
  f5::ledger ledger;                   // delta over the whole run
  bool ledger_enabled = f5::enabled;   // false: the ledger is compiled out
  std::uint64_t commits = 0;           // commits published by the manager
  std::uint64_t objects = 0;           // objects the run added to the store
  std::uint64_t validation_passes = 0; // validator registry runs (probe validator)
  std::uint64_t validators = 0;        // registered validators
  std::uint64_t changed_records = 0;   // records changed over every commit (observer)
};

/// Release gate 1 (ARCH1-R41): records without an owner, a stable id or a revision.
struct gate1_report {
  std::uint64_t numerator = 0;
  std::uint64_t denominator = 0;
  std::vector<std::string> offenders; // "<record id>: <why>"
};

struct conformance_run {
  std::vector<type_report> types;
  workload_counters counters;
  f2_report f2;
  gate1_report gate1;
};

/// The suite over `registry` (file comment). Errors only when the workspace or the workload
/// cannot run; a failing case is data in the report.
[[nodiscard]] result<conformance_run> run_conformance(const domain_fixture_registry &registry);

/// Gate 1 over the live records of `head`: owner = the record's type is bound to a claimed
/// domain; stable id = a non-null id of an assigned class equal to the type's record class (any
/// class for record class none); revision = `head` is a revision and the record object is stored.
[[nodiscard]] result<gate1_report>
measure_gate1(const snapshot &head, const schema_registry &schemas, const domain_registry &domains);

/// The scorecard input document ("sl-fabric-scorecard-input/1").
[[nodiscard]] std::string scorecard_input_json(const conformance_run &run);

/// SL_FABRIC_SCORECARD_INPUT when set, else
/// <fixtures_dir>/../../../build/fabric/scorecard-input.json (the repository's build/fabric).
[[nodiscard]] std::string scorecard_input_path(std::string_view fixtures_dir);

/// Writes `text` to `path`, creating its parent directories; `store_io` on failure.
[[nodiscard]] result<void> write_text_file(const std::string &path, std::string_view text);

} // namespace sl::fabric::testing
