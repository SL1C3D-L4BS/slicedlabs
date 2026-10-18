// FAB-INV-TX-11 (SL-ARCH-002 ARCH2-R46): a lock_authority denial is `locked` and writes nothing.
// The authority sees the origin and the whole footprint; it is consulted at validation (the locks
// stage, also in the dry run) and again under the commit mutex at step C2, where a lock taken
// between the two still refuses the commit. Footprints outside the lock commit normally, and the
// null authority (the default) allows everything.

#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/tx_fixture.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

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

// Locks one record after `allow_first` checks (0: from the start).
class record_lock final : public lock_authority {
public:
  record_lock(record_id locked, std::uint64_t allow_first) : locked_(locked), allow_(allow_first) {}

  [[nodiscard]] sl::result<void> check(const edit_origin &origin,
                                       const footprint &fp) const override {
    const std::uint64_t n = calls_.fetch_add(1);
    last_actor_ = origin.actor; // single-threaded test
    if (n < allow_) {
      return {};
    }
    for (const footprint_entry &e : fp.entries) {
      if (e.record == locked_) {
        return sl::make_error(errc::locked, locked_.to_string() + " is locked by a test session");
      }
    }
    return {};
  }
  [[nodiscard]] std::uint64_t calls() const noexcept { return calls_.load(); }
  [[nodiscard]] actor_id last_actor() const noexcept { return last_actor_; }

private:
  record_id locked_;
  std::uint64_t allow_;
  mutable std::atomic<std::uint64_t> calls_{0};
  mutable actor_id last_actor_;
};

} // namespace

TEST_CASE("FAB-INV-TX-11: a lock_authority denial is locked and writes nothing") {
  auto store = std::make_shared<memory_backend>();
  auto ws = must(
      workspace::create_with(store, must(tg::scene_options(std::make_shared<manual_clock>(4)))));
  transaction_manager &m = ws->manager();
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  const commit_receipt base = must(
      tg::commit_ops(m, editor, {must(tg::create_entity("hero")), must(tg::create_entity("foe"))}));
  const record_id hero = tg::entity_named("hero");

  SUBCASE("denied at validation") {
    auto lock = std::make_shared<record_lock>(hero, 0);
    m.set_lock_authority(lock);
    tx_options o;
    o.origin = editor;
    auto t = must(m.begin(o));
    must_ok(t.stage(must(tg::set_name(hero, "locked out"))));
    const auto dry = t.validate();
    REQUIRE_FALSE(dry.has_value());
    CHECK(dry.error().code == errc::locked);
    CHECK(t.state() == tx_state::open);
    const auto r = t.commit();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == errc::locked);
    CHECK(t.state() == tx_state::conflict);
    REQUIRE(t.conflict().has_value());
    const conflict_report report = t.conflict().value_or(conflict_report{});
    CHECK(report.code == errc::locked);
    CHECK_FALSE(report.ours.entries.empty());
    CHECK(lock->last_actor() == tg::tx_editor());
    // A footprint outside the lock commits.
    const commit_receipt ok =
        must(tg::commit_ops(m, editor, {must(tg::set_name(tg::entity_named("foe"), "free"))}));
    CHECK(ok.seq == base.seq + 1);
  }

  SUBCASE("a lock taken after validation is caught at C2") {
    auto lock = std::make_shared<record_lock>(hero, 1); // validation passes, C2 denies
    m.set_lock_authority(lock);
    const auto r = tg::commit_ops(m, editor, {must(tg::set_name(hero, "late"))});
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == errc::locked);
    CHECK(lock->calls() == 2);
    CHECK(must(store->read_ref("main")) == base.revision);
    CHECK(store->last_seq() == base.seq);
  }

  SUBCASE("the null authority allows everything") {
    m.set_lock_authority(nullptr);
    must(tg::commit_ops(m, editor, {must(tg::set_name(hero, "free"))}));
  }
}
