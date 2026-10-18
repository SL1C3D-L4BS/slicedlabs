// FAB-INV-F5-01 (SL-ARCH-002 ARCH2-R17, ADR-0016; SL-GOV-006 F5): a normal commit increments the
// ledger's `mutations` and never `bypassing`. The F5 audit: a transaction workload (commits,
// create_ref, reset_ref, a rejected and a stale commit) over a memory_backend and a log_backend,
// every write through transaction_manager. It prints `f5 audit: ...` with the numerator
// (bypassing) and the denominator (mutations) and requires the numerator to be 0. A captured
// assertion handler keeps a bypass from aborting, so a broken scope (neg-fabric-f5 removes the
// authorized_scope line from tx_manager.cpp) fails here with both counters reported.

#include "sl/core/assert.hpp"
#include "sl/fabric/clock.hpp"
#include "sl/fabric/f5.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/tx_fixture.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <print>
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

// Counts the assertions a bypass trips instead of aborting.
struct captured_asserts {
  std::uint64_t count = 0;
  std::string last;
  sl::scoped_assert_handler handler{[this](const sl::assert_report &r) {
    ++count;
    last = std::string(r.message);
  }};
};

struct workload_totals {
  std::uint64_t commits = 0;  // operations that reached the backend
  std::uint64_t refusals = 0; // operations refused before the backend
};

// Runs `op` and checks the ledger: a write adds at least one mutation, a refusal none, and
// neither adds a bypass.
template <class Op> void step(const char *what, bool writes, workload_totals &totals, Op op) {
  const f5::ledger before = f5::read();
  op();
  const f5::ledger after = f5::read();
  INFO(what);
  CHECK(after.bypassing == before.bypassing);
  if (writes) {
    CHECK(after.mutations >= before.mutations + 1);
    ++totals.commits;
  } else {
    CHECK(after.mutations == before.mutations);
    ++totals.refusals;
  }
}

workload_totals run_workload(transaction_manager &m) {
  workload_totals totals;
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  commit_receipt first;
  step("commit two entities", true, totals, [&] {
    first = must(
        tg::commit_ops(m, editor, {must(tg::create_entity("a")), must(tg::create_entity("b"))}));
  });
  step("rename an entity", true, totals, [&] {
    static_cast<void>(
        must(tg::commit_ops(m, editor, {must(tg::set_name(tg::entity_named("a"), "a2"))})));
  });
  step("rename a missing entity (refused before the backend)", false, totals, [&] {
    CHECK_FALSE(tg::commit_ops(m, editor, {must(tg::set_name(tg::entity_named("zz"), "x"))}));
  });
  step("create_ref", true, totals,
       [&] { static_cast<void>(must(m.create_ref("feature", first.revision, editor))); });
  step("commit on the new ref", true, totals, [&] {
    static_cast<void>(must(tg::commit_ops(
        m, editor, {must(tg::set_name(tg::entity_named("b"), "b2"))}, {}, "feature")));
  });
  step("reset_ref", true, totals,
       [&] { static_cast<void>(must(m.reset_ref("feature", first.revision, editor))); });
  tx_options o;
  o.origin = editor;
  auto stale = must(m.begin(o));
  REQUIRE(stale.stage(must(tg::set_name(tg::entity_named("b"), "late"))).has_value());
  step("a commit that makes the open transaction stale", true, totals, [&] {
    static_cast<void>(
        must(tg::commit_ops(m, editor, {must(tg::set_name(tg::entity_named("a"), "a3"))})));
  });
  step("a stale commit (refused at C1)", false, totals, [&] {
    const auto r = stale.commit();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == errc::stale_base);
  });
  return totals;
}

void audit(const char *backend, const f5::ledger &l, const workload_totals &totals,
           const captured_asserts &asserts) {
  std::println("f5 audit: {} numerator (bypassing) {} denominator (mutations) {}; {} commits, {} "
               "refusals, {} assertions",
               backend, l.bypassing, l.mutations, totals.commits, totals.refusals, asserts.count);
  static_cast<void>(std::fflush(stdout));
}

} // namespace

TEST_CASE("FAB-INV-F5-01: every commit through the manager counts as a mutation, none as a bypass "
          "(memory_backend)") {
  captured_asserts asserts;
  auto store = std::make_shared<memory_backend>();
  auto clock = std::make_shared<manual_clock>(1'000'000);
  auto ws = must(workspace::create_with(store, must(tg::scene_options(clock))));
  f5::reset_for_test();

  const workload_totals totals = run_workload(ws->manager());
  const f5::ledger l = f5::read();
  audit("memory_backend", l, totals, asserts);
  if constexpr (f5::enabled) {
    CHECK(l.mutations >= totals.commits);
    CHECK(l.mutations > 0);
  } else {
    CHECK(l == f5::ledger{});
  }
  CHECK(l.bypassing == 0);
  CHECK(asserts.count == 0);
  CHECK(f5::last_bypass_site().empty());
}

TEST_CASE("FAB-INV-F5-01: every commit through the manager counts as a mutation, none as a bypass "
          "(log_backend)") {
  captured_asserts asserts;
  tg::tx_log_store log;
  auto store = must(log.open());
  auto clock = std::make_shared<manual_clock>(1'000'000);
  auto ws = must(workspace::create_with(store, must(tg::scene_options(clock))));
  f5::reset_for_test();

  const workload_totals totals = run_workload(ws->manager());
  const f5::ledger l = f5::read();
  audit("log_backend", l, totals, asserts);
  if constexpr (f5::enabled) {
    CHECK(l.mutations >= totals.commits);
    CHECK(l.mutations > 0);
  } else {
    CHECK(l == f5::ledger{});
  }
  CHECK(l.bypassing == 0);
  CHECK(asserts.count == 0);
  CHECK(f5::last_bypass_site().empty());
}

TEST_CASE("FAB-INV-F5-01: opening a workspace and reading write nothing") {
  captured_asserts asserts;
  auto store = std::make_shared<memory_backend>();
  f5::reset_for_test();
  auto clock = std::make_shared<manual_clock>(1'000'000);
  auto ws = must(workspace::create_with(store, must(tg::scene_options(clock))));
  static_cast<void>(must(ws->manager().head()));
  CHECK(ws->manager().refs().empty());
  CHECK(f5::read() == f5::ledger{});
  CHECK(asserts.count == 0);
}
