// FAB-INV-UNDO-11 (SL-ARCH-002 ARCH2-R26; ADR-0018): 200 coalesced sets in one open transaction
// make one revision and one undo entry. A drag stages 200 sets of one path and commits once on
// release: the store gains one revision whose patch holds one set with the first expectation (the
// value before the drag) and the last value; the actor's stack gains one entry, and one undo
// restores the value before the drag. Interleaved drags on two paths coalesce per path; an aborted
// drag leaves nothing.

#include "sl/fabric/clock.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/undo.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/tx_fixture.hpp"
#include "../../../engine/fabric/testing/undo_fixture.hpp"

#include <doctest/doctest.h>

#include <cstddef>
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

value body_of(const transaction_manager &m, const record_id &id) {
  return must(must(m.head()).get(id)).body;
}

// A struct body {1 x uint, 2 y uint} of generator type gen.rec0 (no schema validators).
patch_op create_point(const record_id &id) {
  const value body = must(
      value::struct_({field{.no = 1, .v = value::uint(0)}, field{.no = 2, .v = value::uint(0)}}));
  return op_create(id, type_id_of("gen.rec0"), body);
}

} // namespace

TEST_CASE("FAB-INV-UNDO-11: 200 coalesced sets in one open transaction make one revision and one "
          "undo entry") {
  auto store = std::make_shared<memory_backend>();
  auto ws = must(
      workspace::create_with(store, must(tg::scene_options(std::make_shared<manual_clock>(1)))));
  transaction_manager &m = ws->manager();
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  undo_manager um(m, tg::tx_editor());
  const record_id hero = tg::entity_named("hero");
  must(tg::commit_ops(m, editor, {must(tg::create_entity("hero"))}));
  const value before = body_of(m, hero);
  const value first_name = *before.find_field(1);

  const std::size_t entries = um.undo_stack().size();
  const std::uint64_t commits = m.commits();
  const std::size_t len = must(tg::history_length(*store, must(m.head()).revision_digest()));
  tx_options o;
  o.origin = editor;
  o.meta.label = "drag";
  o.meta.coalesce_key = "drag/name";
  auto drag = must(m.begin(o));
  for (int i = 0; i < 200; ++i) {
    must_ok(drag.stage(must(tg::set_name(hero, "step " + std::to_string(i)))));
  }
  CHECK(drag.staged().ops.size() == 200);
  const commit_receipt released = must(drag.commit());
  CHECK(m.commits() == commits + 1);
  CHECK(must(tg::history_length(*store, released.revision)) == len + 1);
  const patch p = must(tg::stored_patch(*store, released.revision));
  REQUIRE(p.ops.size() == 1);
  CHECK(p.ops[0].kind == op_kind::set);
  CHECK(p.ops[0].expect == expectation::of(first_name));
  CHECK(p.ops[0].new_value == must(value::string("step 199")));
  CHECK(um.undo_stack().size() == entries + 1);
  CHECK(um.undo_stack().back().txs == std::vector<tx_id>{released.tx});
  CHECK(um.undo_label() == "drag");

  const commit_receipt u = must(um.undo());
  CHECK(body_of(m, hero) == before);
  CHECK(must(tg::stored_patch(*store, u.revision)).ops.size() == 1);
  CHECK(um.undo_stack().size() == entries);

  // An aborted drag leaves no revision and no entry.
  auto aborted = must(m.begin(o));
  for (int i = 0; i < 50; ++i) {
    must_ok(aborted.stage(must(tg::set_name(hero, "lost " + std::to_string(i)))));
  }
  aborted.abort();
  CHECK(m.commits() == commits + 2);
  CHECK(um.undo_stack().size() == entries);
}

TEST_CASE("FAB-INV-UNDO-11: interleaved drags on two paths coalesce per path") {
  auto store = std::make_shared<memory_backend>();
  auto ws =
      must(workspace::create_with(store, must(tg::gen_options(std::make_shared<manual_clock>(1)))));
  transaction_manager &m = ws->manager();
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  undo_manager um(m, tg::tx_editor());
  const record_id point = tg::entity_named("point");
  must(tg::commit_ops(m, editor, {create_point(point)}));
  const value before = body_of(m, point);

  tx_options o;
  o.origin = editor;
  auto drag = must(m.begin(o));
  for (std::uint64_t i = 1; i <= 100; ++i) {
    must_ok(drag.stage(op_set(point, {path_segment::field(1)}, value::uint(i))));
    must_ok(drag.stage(op_set(point, {path_segment::field(2)}, value::uint(i * 2))));
  }
  const commit_receipt released = must(drag.commit());
  const patch p = must(tg::stored_patch(*store, released.revision));
  REQUIRE(p.ops.size() == 2);
  CHECK(p.ops[0].new_value == value::uint(100));
  CHECK(p.ops[0].expect == expectation::of(value::uint(0)));
  CHECK(p.ops[1].new_value == value::uint(200));
  CHECK(p.ops[1].expect == expectation::of(value::uint(0)));
  CHECK(um.undo_stack().size() == 2);
  must(um.undo());
  CHECK(body_of(m, point) == before);
}
