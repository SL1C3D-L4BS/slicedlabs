// FAB-INV-IMP-03 (SL-ARCH-001 ARCH1-R59; master plan section 3, F4 seam; ADR-0009): is_current
// is false iff a pinned input digest differs from the head. A cooked mesh pins the record objects
// of its input meshes through the provenance of the transaction that wrote it; editing, deleting
// or retiring any input makes it not current, re-cooking over the new inputs makes it current
// again, an older head is judged with the provenance of the version it holds (resolved from
// history), and a seeded sweep over random input sets and random edits holds the "iff" both ways
// for the engine and for the free function over the pins. A read failure is an error, never a
// verdict; a record that is not derived, not live or without complete provenance is refused.

#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/impact.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/provenance.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/store_faults.hpp"
#include "../../../engine/fabric/testing/tx_fixture.hpp"
#include "../../../engine/fabric/testing/undo_fixture.hpp"
#include "../../../engine/fabric/testing/value_gen.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
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

digest text_digest(std::string_view text) {
  digest d;
  d.bytes = sha256::hash(text);
  return d;
}

const tx_origin &content() {
  static const tx_origin o = tg::tx_origin_of(tg::tx_editor(), "content");
  return o;
}

// The origin of a cook: provenance over the current record objects of `inputs` in `head`.
tx_origin cook_origin(const snapshot &head, const std::vector<record_id> &inputs) {
  provenance p = tg::complete_provenance(inputs.front());
  std::vector<digest> revisions;
  revisions.reserve(inputs.size());
  for (const record_id &id : inputs) {
    revisions.push_back(must(head.get(id)).object);
  }
  p.input_ids = inputs;
  p.input_revisions = revisions;
  tx_origin o = tg::tx_origin_of(tg::tx_editor(), "asset");
  o.provenance = p;
  return o;
}

commit_receipt edit_mesh(transaction_manager &m, const record_id &mesh, std::uint64_t vertices) {
  return must(tg::commit_ops(m, content(),
                             {op_set(mesh, {path_segment::field(2)}, value::uint(vertices))}));
}

commit_receipt recook(transaction_manager &m, const record_id &cooked,
                      const std::vector<record_id> &inputs, std::string_view payload) {
  return must(tg::commit_ops(
      m, cook_origin(must(m.head()), inputs),
      {op_set(cooked, {path_segment::field(2)}, value::digest(text_digest(payload)))}));
}

bool current(const impact_engine &e, const record_id &id, const snapshot &head) {
  return must(e.is_current(id, head));
}

} // namespace

TEST_CASE("FAB-INV-IMP-03: is_current is false iff a pinned input digest differs from the head") {
  auto ws =
      must(workspace::create_memory(must(tg::scene_options(std::make_shared<manual_clock>(3)))));
  transaction_manager &m = ws->manager();
  const record_id m1 = tg::asset_named("m1");
  const record_id m2 = tg::asset_named("m2");
  const record_id cooked = tg::derived_named("c1");
  must(tg::commit_ops(m, content(),
                      {must(tg::create_mesh("m1", 10)), must(tg::create_mesh("m2", 20))}));
  must(tg::commit_ops(m, cook_origin(must(m.head()), {m1, m2}),
                      {must(tg::create_cooked("c1", m1))}));
  auto engine = must(impact_engine::attach(m, must(m.head())));
  const snapshot cooked_head = must(m.head());

  // Every input as pinned: current; the index pins both inputs.
  CHECK(current(*engine, cooked, cooked_head));
  const impact_index idx = must(engine->index());
  REQUIRE(idx.pins.contains(cooked));
  CHECK(idx.pins.at(cooked).inputs.size() == 2);
  CHECK(must(is_current(idx.pins.at(cooked).inputs, cooked_head)));
  // The provenance input is an edge: the cooked mesh is affected by its non-body input.
  const impact_set affected = must(engine->impact(std::vector<record_id>{m2}));
  REQUIRE(affected.nodes.size() == 2);
  CHECK(affected.nodes[1].id == cooked);
  CHECK(affected.nodes[1].via_kind == edge_kind::derives_from);

  // An input edited: not current at the new head, still current at the old one.
  edit_mesh(m, m2, 21);
  const snapshot edited = must(m.head());
  CHECK_FALSE(current(*engine, cooked, edited));
  CHECK(current(*engine, cooked, cooked_head));

  // Re-cooked over the new input: current again; the head before the re-cook holds the old
  // version, judged with its own provenance from history.
  recook(m, cooked, {m1, m2}, "v2");
  const snapshot recooked = must(m.head());
  CHECK(current(*engine, cooked, recooked));
  CHECK_FALSE(current(*engine, cooked, edited));
  CHECK(current(*engine, cooked, cooked_head));

  // Editing the input back to its old body restores the old record object: current again for
  // the original pins, not for the re-cook (which pinned the edited object).
  edit_mesh(m, m2, 20);
  CHECK_FALSE(current(*engine, cooked, must(m.head())));

  // A deleted input differs (m2 is referenced by no body, only by provenance).
  recook(m, cooked, {m1, m2}, "v3");
  CHECK(current(*engine, cooked, must(m.head())));
  must(tg::commit_ops(m, content(), {op_delete(m2)}));
  CHECK_FALSE(current(*engine, cooked, must(m.head())));

  // Refusals.
  CHECK(engine->is_current(m1, must(m.head())).error().code == errc::invalid_argument);
  CHECK(engine->is_current(tg::derived_named("nope"), must(m.head())).error().code ==
        errc::not_found);
  must(tg::commit_ops(m, tg::tx_origin_of(tg::tx_editor(), "asset"), {op_delete(cooked)}));
  CHECK(engine->is_current(cooked, must(m.head())).error().code == errc::not_found);
  CHECK(engine->stats().rebuilds == 1);
}

TEST_CASE("FAB-INV-IMP-03: seeded sweep: current exactly when no pinned input changed") {
  const std::size_t seeds = tg::sweep_scaled(4);
  std::size_t verdicts = 0;
  for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
    CAPTURE(seed);
    auto ws = must(
        workspace::create_memory(must(tg::scene_options(std::make_shared<manual_clock>(3), seed))));
    transaction_manager &m = ws->manager();
    tg::rng r(seed * 6364136223846793005ULL);
    std::vector<record_id> meshes;
    std::vector<patch_op> creates;
    for (int i = 0; i < 6; ++i) {
      const std::string name = "mesh" + std::to_string(i);
      creates.push_back(must(tg::create_mesh(name, 1)));
      meshes.push_back(tg::asset_named(name));
    }
    must(tg::commit_ops(m, content(), creates));
    auto engine = must(impact_engine::attach(m, must(m.head())));
    const record_id cooked = tg::derived_named("swept");
    bool created = false;
    for (int round = 0; round < 12; ++round) {
      CAPTURE(round);
      // Cook over a random non-empty input set (input 0 is the body's source).
      std::vector<record_id> inputs{meshes[0]};
      for (std::size_t i = 1; i < meshes.size(); ++i) {
        if (r.chance(50)) {
          inputs.push_back(meshes[i]);
        }
      }
      if (!created) {
        must(tg::commit_ops(m, cook_origin(must(m.head()), inputs),
                            {must(tg::create_cooked("swept", meshes[0]))}));
        created = true;
      } else {
        recook(m, cooked, inputs, "round" + std::to_string(round));
      }
      CHECK(current(*engine, cooked, must(m.head())));
      // Edit a random subset of meshes in one commit.
      std::set<record_id> edited;
      std::vector<patch_op> edits;
      for (const record_id &mesh : meshes) {
        if (r.chance(30)) {
          edited.insert(mesh);
          edits.push_back(op_set(mesh, {path_segment::field(2)},
                                 value::uint(1000 + static_cast<std::uint64_t>(round))));
        }
      }
      if (!edits.empty()) {
        must(tg::commit_ops(m, content(), edits));
      }
      bool touched = false;
      for (const record_id &in : inputs) {
        touched = touched || edited.contains(in);
      }
      const snapshot head = must(m.head());
      CHECK(current(*engine, cooked, head) == !touched);
      const impact_index idx = must(engine->index());
      CHECK(must(is_current(idx.pins.at(cooked).inputs, head)) == !touched);
      ++verdicts;
    }
    CHECK(engine->stats().rebuilds == 1);
  }
  CHECK(verdicts == seeds * 12);
}

TEST_CASE("FAB-INV-IMP-03: a read failure is an error and missing provenance is refused") {
  auto faults = std::make_shared<tg::store_faults>();
  auto store = std::make_shared<memory_backend>(faults);
  auto ws = must(
      workspace::create_with(store, must(tg::scene_options(std::make_shared<manual_clock>(3)))));
  transaction_manager &m = ws->manager();
  const record_id m1 = tg::asset_named("m1");
  const record_id cooked = tg::derived_named("c1");
  must(tg::commit_ops(m, content(), {must(tg::create_mesh("m1", 10))}));
  must(tg::commit_ops(m, cook_origin(must(m.head()), {m1}), {must(tg::create_cooked("c1", m1))}));
  auto engine = must(impact_engine::attach(m, must(m.head())));
  const snapshot head = must(m.head());
  CHECK(current(*engine, cooked, head));

  // The input's record object no longer verifies: neither current nor stale, an error.
  const digest object = must(head.get(m1)).object;
  faults->flip_on_read(object, 12, 0x5A);
  const auto verdict = engine->is_current(cooked, head);
  REQUIRE_FALSE(verdict.has_value());
  const auto direct = is_current(must(engine->index()).pins.at(cooked).inputs, head);
  CHECK_FALSE(direct.has_value());
  faults->clear_flips();
  CHECK(current(*engine, cooked, head));

  // A derived record written without provenance (the provenance validator left out): refused.
  auto opts = must(tg::scene_options(std::make_shared<manual_clock>(3)));
  opts.omit_builtin.emplace_back("fabric.provenance");
  auto bare = must(workspace::create_memory(std::move(opts)));
  transaction_manager &bm = bare->manager();
  must(tg::commit_ops(bm, content(), {must(tg::create_mesh("m1", 10))}));
  must(tg::commit_ops(bm, tg::tx_origin_of(tg::tx_editor(), "asset"),
                      {must(tg::create_cooked("c1", m1))}));
  auto bare_engine = must(impact_engine::attach(bm, must(bm.head())));
  CHECK(bare_engine->is_current(cooked, must(bm.head())).error().code ==
        errc::provenance_incomplete);
  CHECK_FALSE(must(bare_engine->index()).pins.contains(cooked));

  // A head opened from a root (no revision): judged from the index pin of the same version.
  const snapshot rooted = must(snapshot::open_root(ws->store(), head.root_digest()));
  CHECK(current(*engine, cooked, rooted));
}
