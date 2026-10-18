// FAB-INV-TX-01 (SL-ARCH-002 ARCH2-R19, `failpoints`): at every failpoint FP0-FP6 the head is the
// old or the new revision, never between, and a retry under the same idempotency key converges on
// the one revision a clean commit makes. FP0, FP1, FP2, FP5 and FP6 are armed on the manager
// (testing::tx_failpoints); FP3 and FP4 lie inside store_backend::commit and are reached through
// the store's own fault seams: a failure at every step of a memory_backend commit, and a failure
// at every storage primitive of a log_backend commit followed by a crash under each faulty-vfs
// policy. Seeded patch_gen workloads cover every failpoint on both backends.

#include "sl/fabric/failpoints.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/patch_gen.hpp"
#include "../../../engine/fabric/testing/store_faults.hpp"
#include "../../../engine/fabric/testing/tx_fixture.hpp"

#include <doctest/doctest.h>

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

void must_ok(const sl::result<void> &r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
}

std::shared_ptr<manual_clock> fresh_clock() { return std::make_shared<manual_clock>(1'000'000); }

std::unique_ptr<workspace> scene_ws(std::shared_ptr<store_backend> store) {
  return must(workspace::create_with(std::move(store), must(tg::scene_options(fresh_clock()))));
}

// The first commit: two entities. The second (keyed) commit renames one of them.
std::vector<patch_op> first_ops() {
  return {must(tg::create_entity("a")), must(tg::create_entity("b"))};
}
std::vector<patch_op> second_ops() { return {must(tg::set_name(tg::entity_named("a"), "a2"))}; }
patch_meta second_meta() {
  patch_meta m;
  m.label = "rename a";
  m.idempotency_key = "rename-a";
  return m;
}

const tx_origin &editor() {
  static const tx_origin o = tg::tx_origin_of(tg::tx_editor());
  return o;
}

struct clean_run {
  digest first;
  digest second;
};

// The two revisions a fault-free run makes (deterministic: no clock or entropy in them).
clean_run clean() {
  auto ws = scene_ws(std::make_shared<memory_backend>());
  clean_run out;
  out.first = must(tg::commit_ops(ws->manager(), editor(), first_ops())).revision;
  out.second = must(tg::commit_ops(ws->manager(), editor(), second_ops(), second_meta())).revision;
  return out;
}

std::optional<digest> head_of(const store_backend &s) {
  auto r = s.read_ref("main");
  return r ? std::optional<digest>(*r) : std::nullopt;
}

} // namespace

TEST_CASE("FAB-INV-TX-01: FP0, FP1 and FP2 leave the head and the store untouched; a keyed retry "
          "commits once") {
  const clean_run want = clean();
  for (const failpoint fp : {failpoint::fp0, failpoint::fp1, failpoint::fp2}) {
    CAPTURE(to_string(fp));
    auto store = std::make_shared<memory_backend>();
    auto ws = scene_ws(store);
    REQUIRE(must(tg::commit_ops(ws->manager(), editor(), first_ops())).revision == want.first);
    const std::size_t objects = store->object_count();
    const std::uint64_t seq = store->last_seq();

    must_ok(tg::tx_failpoints::arm(ws->manager().failpoints(), fp));
    tx_options o;
    o.origin = editor();
    o.meta = second_meta();
    auto t = must(ws->manager().begin(o));
    for (const patch_op &op : second_ops()) {
      must_ok(t.stage(op));
    }
    const auto r = t.commit();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == errc::store_io);
    CHECK(t.state() == tx_state::failed);
    CHECK(ws->manager().failpoints().trips(fp) == 1);
    CHECK(head_of(*store) == want.first);
    CHECK(must(ws->manager().head()).revision_digest() == want.first);
    CHECK(store->object_count() == objects); // FP2: the objects lived in memory only
    CHECK(store->last_seq() == seq);
    CHECK_FALSE(ws->manager().recovery_required());

    const commit_receipt retry =
        must(tg::commit_ops(ws->manager(), editor(), second_ops(), second_meta()));
    CHECK(retry.revision == want.second);
    CHECK(store->last_seq() == seq + 1);
    const commit_receipt again =
        must(tg::commit_ops(ws->manager(), editor(), second_ops(), second_meta()));
    CHECK(again == retry);
    CHECK(store->last_seq() == seq + 1);
  }
}

TEST_CASE("FAB-INV-TX-01: FP5 and FP6 commit, make the manager indeterminate, and after a reopen "
          "the keyed retry returns the original revision") {
  const clean_run want = clean();
  for (const failpoint fp : {failpoint::fp5, failpoint::fp6}) {
    CAPTURE(to_string(fp));
    auto store = std::make_shared<memory_backend>();
    auto ws = scene_ws(store);
    REQUIRE(must(tg::commit_ops(ws->manager(), editor(), first_ops())).revision == want.first);
    std::vector<std::string> seen;
    auto sub = ws->manager().events().subscribe(
        "fabric.tx.", [&seen](const event_envelope &e) noexcept { seen.push_back(e.type); });

    must_ok(tg::tx_failpoints::arm(ws->manager().failpoints(), fp));
    const auto r = tg::commit_ops(ws->manager(), editor(), second_ops(), second_meta());
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == errc::commit_indeterminate);
    CHECK(head_of(*store) == want.second); // durable: the commit point was passed
    CHECK(seen.empty());                   // no committed event before the crash
    CHECK(ws->manager().recovery_required());
    tx_options plain;
    plain.origin = editor();
    const auto blocked = ws->manager().begin(plain);
    REQUIRE_FALSE(blocked.has_value());
    CHECK(blocked.error().code == errc::recovery_required);
    const std::uint64_t seq = store->last_seq();
    sub.reset();
    ws.reset();

    // Reopen over the same store: recovery reports the head, the keyed retry is a no-op.
    workspace_options again = must(tg::scene_options(fresh_clock()));
    std::vector<event_envelope> recovered;
    again.subscribers.emplace_back(
        "fabric.tx.recovered",
        [&recovered](const event_envelope &e) noexcept { recovered.push_back(e); });
    auto reopened = must(workspace::create_with(store, std::move(again)));
    REQUIRE(recovered.size() == 1);
    CHECK(recovered[0].causality.revision == want.second);
    CHECK(must(reopened->manager().head()).revision_digest() == want.second);
    const commit_receipt retry =
        must(tg::commit_ops(reopened->manager(), editor(), second_ops(), second_meta()));
    CHECK(retry.revision == want.second);
    CHECK(retry.recovered);
    CHECK(store->last_seq() == seq);
  }
}

TEST_CASE("FAB-INV-TX-01: FP3 and FP4 inside memory_backend: a failure at every commit step rolls "
          "the whole batch back") {
  const clean_run want = clean();
  auto faults = std::make_shared<tg::store_faults>();
  auto store = std::make_shared<memory_backend>(faults);
  auto ws = scene_ws(store);
  REQUIRE(must(tg::commit_ops(ws->manager(), editor(), first_ops())).revision == want.first);
  const std::size_t objects = store->object_count();
  std::uint64_t failures = 0;
  for (std::uint64_t k = 0;; ++k) {
    CAPTURE(k);
    faults->fail_after(k);
    const auto r = tg::commit_ops(ws->manager(), editor(), second_ops(), second_meta());
    faults->disarm();
    if (r) {
      CHECK(r->revision == want.second);
      break;
    }
    ++failures;
    CHECK(r.error().code == errc::store_io);
    CHECK(head_of(*store) == want.first);
    CHECK(store->object_count() == objects);
    CHECK_FALSE(ws->manager().recovery_required());
    REQUIRE(k < 64);
  }
  CHECK(failures >= 3); // objects, the ref and the sequence step all failed once
}

TEST_CASE("FAB-INV-TX-01: FP3 and FP4 inside log_backend: a crash after every storage primitive "
          "leaves the old or the new head, and a keyed retry converges") {
  const clean_run want = clean();
  struct policy_case {
    tg::crash_policy policy;
    std::uint64_t tear;
  };
  const std::vector<policy_case> policies = {
      {.policy = tg::crash_policy::drop_unsynced, .tear = 0},
      {.policy = tg::crash_policy::keep_unsynced, .tear = 0},
      {.policy = tg::crash_policy::tear_last_append, .tear = 24}};
  std::size_t old_heads = 0;
  std::size_t new_heads = 0;
  std::size_t torn = 0;
  for (const policy_case &pc : policies) {
    for (std::uint64_t k = 0;; ++k) {
      CAPTURE(k);
      CAPTURE(static_cast<int>(pc.policy));
      tg::tx_log_store log;
      {
        auto ws = scene_ws(must(log.open()));
        REQUIRE(must(tg::commit_ops(ws->manager(), editor(), first_ops())).revision == want.first);
        log.fs().fail_after(k);
        const auto r = tg::commit_ops(ws->manager(), editor(), second_ops(), second_meta());
        log.fs().disarm();
        if (r) {
          CHECK(r->revision == want.second);
          break; // k is past the last primitive of the commit
        }
        CAPTURE(r.error().message);
        CHECK((r.error().code == errc::commit_indeterminate || r.error().code == errc::store_io ||
               r.error().code == errc::recovery_required));
      }
      must_ok(log.fs().crash(pc.policy, pc.tear));
      auto reopened = log.open();
      if (!reopened) {
        // A torn commit frame is a torn tail: WO-055's recovery truncates it (not committed).
        CAPTURE(reopened.error().message);
        CHECK(pc.policy == tg::crash_policy::tear_last_append);
        CHECK(reopened.error().code == errc::recovery_required);
        ++torn;
        continue;
      }
      const std::optional<digest> head = head_of(**reopened);
      CHECK((head == want.first || head == want.second));
      (head == want.first ? old_heads : new_heads) += 1;
      auto ws = scene_ws(*reopened);
      const commit_receipt retry =
          must(tg::commit_ops(ws->manager(), editor(), second_ops(), second_meta()));
      CHECK(retry.revision == want.second);
      CHECK(head_of(ws->store()) == want.second);
      REQUIRE(k < 64);
    }
  }
  CHECK(old_heads > 0);
  CHECK(torn > 0);
  CHECK(new_heads > 0); // keep_unsynced after the append: the frame is intact (FP4 "either")
}

TEST_CASE("FAB-INV-TX-01: seeded workloads on both backends: every failpoint leaves old or new and "
          "the retry equals the oracle") {
  const std::vector<failpoint> fps = {failpoint::fp0, failpoint::fp1, failpoint::fp2,
                                      failpoint::fp5, failpoint::fp6};
  for (std::uint64_t seed = 1; seed <= 3; ++seed) {
    for (const bool durable : {false, true}) {
      CAPTURE(seed);
      CAPTURE(durable);
      tg::patch_gen_options gen_options;
      gen_options.meta_percent = 0;
      tg::patch_gen gen(seed, gen_options);
      tg::oracle_store oracle = must(gen.state(6));
      tg::tx_log_store log;
      std::shared_ptr<store_backend> store =
          durable ? must(log.open()) : std::make_shared<memory_backend>();
      auto open_ws = [&store] {
        return must(workspace::create_with(store, must(tg::gen_options(fresh_clock()))));
      };
      auto ws = open_ws();
      tx_options o;
      o.origin = editor();
      auto t = must(ws->manager().begin(o));
      must_ok(t.stage(tg::load_patch(oracle)));
      must(t.commit());
      for (std::size_t step = 0; step < fps.size() * 2; ++step) {
        const failpoint fp = fps[step % fps.size()];
        CAPTURE(step);
        patch p = must(gen.next(oracle));
        p.base.reset();
        p.meta = patch_meta{};
        p.meta.idempotency_key = "step-" + std::to_string(step);
        const std::optional<digest> before = must(ws->manager().head()).revision_digest();
        must_ok(tg::tx_failpoints::arm(ws->manager().failpoints(), fp));
        auto tx = must(ws->manager().begin(o));
        must_ok(tx.stage(p));
        const auto r = tx.commit();
        REQUIRE_FALSE(r.has_value());
        const std::optional<digest> after = head_of(*store);
        const bool committed = fp == failpoint::fp5 || fp == failpoint::fp6;
        CHECK(after == (committed ? after : before));
        if (committed) {
          CHECK(after != before);
          ws.reset();
          ws = open_ws(); // reopen over the same store
        }
        // The client re-sends the patch on the base it was authored on (ARCH2-R49).
        tx_options again = o;
        again.base = before;
        auto retry = must(ws->manager().begin(again));
        must_ok(retry.stage(p));
        const commit_receipt receipt = must(retry.commit());
        CHECK(receipt.recovered == committed);
        if (committed) {
          CHECK(std::optional<digest>(receipt.revision) == after);
        }
        must_ok(oracle.apply(p));
        CHECK(must(tg::oracle_of(must(ws->manager().head()))) == oracle);
      }
    }
  }
}
