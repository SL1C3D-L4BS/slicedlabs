// FAB-INV-TX-14 (SL-ARCH-002 ARCH2-R15): validate() reports exactly what commit() would and
// writes nothing. For a clean patch, a rejected one, a stale one and a locked one, the dry run
// returns the diagnostics or the error the commit then returns; it leaves the transaction Open,
// the store's objects, sequence and refs, the log store's bytes and the event stream untouched,
// and may run any number of times.

#include "sl/fabric/events.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/tx_fixture.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
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

class deny_all final : public lock_authority {
public:
  [[nodiscard]] sl::result<void> check(const edit_origin & /*origin*/,
                                       const footprint & /*fp*/) const override {
    return sl::make_error(errc::locked, "locked by a test");
  }
};

} // namespace

TEST_CASE("FAB-INV-TX-14: validate reports what commit would and writes nothing") {
  tg::tx_log_store log;
  auto store = must(log.open());
  auto ws = must(
      workspace::create_with(store, must(tg::scene_options(std::make_shared<manual_clock>(8)))));
  transaction_manager &m = ws->manager();
  std::uint64_t events = 0;
  auto sub = m.events().subscribe("", [&events](const event_envelope &) noexcept { ++events; });
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  must(tg::commit_ops(m, editor, {must(tg::create_entity("hero"))}));
  const record_id hero = tg::entity_named("hero");

  const auto untouched = [&](const auto &image, std::uint64_t ev) {
    CHECK(must(tg::file_image(log.memory(), "/ws")) == image);
    CHECK(events == ev);
  };

  SUBCASE("clean") {
    tx_options o;
    o.origin = editor;
    auto t = must(m.begin(o));
    must_ok(t.stage(must(tg::set_name(hero, "dry"))));
    const auto image = must(tg::file_image(log.memory(), "/ws"));
    const std::uint64_t ev = events;
    CHECK(must(t.validate()).empty());
    CHECK(must(t.validate()).empty());
    CHECK(t.state() == tx_state::open);
    untouched(image, ev);
    must(t.commit());
    CHECK(t.diagnostics().empty());
  }

  SUBCASE("rejected") {
    tx_options o;
    o.origin = tg::tx_origin_of(tg::tx_agent());
    auto t = must(m.begin(o));
    must_ok(t.stage(must(tg::set_name(hero, "agent"))));
    must_ok(
        t.stage(op_create(tg::entity_named("bad"), tg::entity_type(), must(value::struct_({})))));
    const auto image = must(tg::file_image(log.memory(), "/ws"));
    const std::uint64_t ev = events;
    const std::vector<diagnostic> dry = must(t.validate());
    CHECK(has_error(dry));
    untouched(image, ev);
    const auto r = t.commit();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == errc::validation_failed);
    CHECK(std::vector<diagnostic>(t.diagnostics().begin(), t.diagnostics().end()) == dry);
  }

  SUBCASE("stale") {
    tx_options o;
    o.origin = editor;
    auto t = must(m.begin(o));
    must_ok(t.stage(must(tg::set_name(hero, "late"))));
    must(tg::commit_ops(m, editor, {must(tg::create_entity("other"))}));
    const auto image = must(tg::file_image(log.memory(), "/ws"));
    const std::uint64_t ev = events;
    const auto dry = t.validate();
    REQUIRE_FALSE(dry.has_value());
    CHECK(dry.error().code == errc::stale_base);
    untouched(image, ev);
    const auto r = t.commit();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == dry.error().code);
  }

  SUBCASE("locked") {
    m.set_lock_authority(std::make_shared<deny_all>());
    tx_options o;
    o.origin = editor;
    auto t = must(m.begin(o));
    must_ok(t.stage(must(tg::set_name(hero, "locked"))));
    const auto image = must(tg::file_image(log.memory(), "/ws"));
    const std::uint64_t ev = events;
    const auto dry = t.validate();
    REQUIRE_FALSE(dry.has_value());
    CHECK(dry.error().code == errc::locked);
    untouched(image, ev);
    CHECK(t.commit().error().code == errc::locked);
  }
}
