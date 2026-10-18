// FAB-INV-IMP-01 (SL-ARCH-001 ARCH1-R57; master plan section 1.3): the incremental dependency
// index equals a full rebuild after every commit of a seeded workload. Two workloads drive an
// attached impact_engine: seeded patch_gen patches over every op kind (with ref-carrying fields
// injected between existing records and an id-scanning edge source), and a scene workload over
// the test schemas with the default edge rule (strong refs from mesh renderers, owned entity
// lists, cooked meshes whose provenance pins their source mesh, deletes, and a reset of the ref).
// After every commit the engine's index equals impact_engine::rebuild() of the head, the scene
// graph also equals dependency_graph::build() of an independent model, the receipt carries the
// report of that commit, and no commit but a reset rebuilt the index. An internal error in the
// observer never fails a commit: it marks the index, and the next query rebuilds it.

#include "sl/fabric/deps.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/impact.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/deps_model_store.hpp"
#include "../../../engine/fabric/testing/patch_gen.hpp"
#include "../../../engine/fabric/testing/test_schemas.hpp"
#include "../../../engine/fabric/testing/tx_fixture.hpp"
#include "../../../engine/fabric/testing/undo_fixture.hpp"
#include "../../../engine/fabric/testing/value_gen.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
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

void must_ok(const sl::result<void> &r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
}

// Every valid id leaf of a value is a reference edge (patch_gen bodies follow no schema).
void scan_ids(const record_id &from, const value &v, std::vector<edge> &out) {
  switch (v.kind()) {
  case value_kind::id: {
    auto id = v.as_record_id();
    if (id && !id->is_null() && is_valid(id->cls())) {
      out.push_back(edge{.from = from, .to = *id, .kind = edge_kind::reference});
    }
    return;
  }
  case value_kind::list:
    for (const value &item : v.items()) {
      scan_ids(from, item, out);
    }
    return;
  case value_kind::map:
    for (const map_entry &e : v.entries()) {
      scan_ids(from, e.v, out);
    }
    return;
  case value_kind::struct_:
    for (const field &f : v.fields()) {
      scan_ids(from, f.v, out);
    }
    return;
  case value_kind::variant:
    scan_ids(from, v.payload(), out);
    return;
  case value_kind::oset:
    for (const oset_entry &e : v.elements()) {
      scan_ids(from, e.v, out);
    }
    return;
  default:
    return;
  }
}

sl::result<std::vector<edge>> id_scan_edges(const record_view &rv,
                                            const schema_registry & /*reg*/) {
  std::vector<edge> out;
  scan_ids(rv.id, *rv.body, out);
  return out;
}

digest text_digest(std::string_view text) {
  digest d;
  d.bytes = sha256::hash(text);
  return d;
}

std::vector<record_id> diff_ids(const snapshot &before, const snapshot &after) {
  std::vector<record_id> ids;
  for (const record_delta &d : must(before.diff(after))) {
    ids.push_back(d.id);
  }
  return ids;
}

// The receipt carries the report of exactly this commit.
void check_receipt(const impact_engine &engine, const commit_receipt &r,
                   std::span<const record_id> changed) {
  REQUIRE(r.attachments.contains(std::string(impact_attachment)));
  const impact_report attached =
      must(impact_report_from_value(r.attachments.find(impact_attachment)->second));
  CHECK(attached == must(engine.report(changed)));
  CHECK(attached.at_revision == r.revision);
  CHECK(engine.committed(r.tx.id()) == attached);
}

std::vector<record_id> live_ids(const snapshot &s) {
  std::vector<record_id> ids;
  must_ok(s.for_each_entry([&ids](const snapshot_entry &e) -> sl::result<void> {
    ids.push_back(e.id);
    return {};
  }));
  return ids;
}

// ---- the scene workload -------------------------------------------------------------------

struct scene_state {
  std::vector<record_id> meshes;
  std::vector<record_id> entities; // not owned by a scene
  std::vector<record_id> scenes;
  std::vector<record_id> cooked;
  std::set<record_id> owned;
  std::map<record_id, std::vector<record_id>> members; // scene -> owned entities
};

scene_state read_scene(const snapshot &s) {
  scene_state st;
  std::vector<record_id> all_entities;
  must_ok(s.for_each_entry([&](const snapshot_entry &e) -> sl::result<void> {
    if (e.type == tg::mesh_type()) {
      st.meshes.push_back(e.id);
    } else if (e.type == tg::entity_type()) {
      all_entities.push_back(e.id);
    } else if (e.type == tg::scene_type()) {
      st.scenes.push_back(e.id);
      std::vector<record_id> &m = st.members[e.id];
      if (const value *list = e.body.find_field(2); list != nullptr) {
        for (const oset_entry &el : list->elements()) {
          m.push_back(must(el.v.as_record_id()));
          st.owned.insert(m.back());
        }
      }
    } else if (e.type == tg::cooked_type()) {
      st.cooked.push_back(e.id);
    }
    return {};
  }));
  for (const record_id &id : all_entities) {
    if (!st.owned.contains(id)) {
      st.entities.push_back(id);
    }
  }
  return st;
}

const record_id &pick(tg::rng &r, const std::vector<record_id> &ids) {
  return ids[static_cast<std::size_t>(r.below(ids.size()))];
}

value renderer_of(const record_id &mesh) {
  return must(value::variant(2, must(value::struct_({field{.no = 1, .v = value::id(mesh)}}))));
}

// An independent model of the scene graph: every live record with the cooked meshes' provenance
// inputs (the workload always cooks a mesh from its own source).
tg::model_store model_of(const snapshot &s) {
  tg::model_store m;
  must_ok(s.for_each_entry([&m](const snapshot_entry &e) -> sl::result<void> {
    std::vector<record_id> inputs;
    if (e.type == tg::cooked_type()) {
      inputs.push_back(must(e.body.find_field(1)->as_record_id()));
    }
    return m.put(e.id, e.type, e.body, std::move(inputs));
  }));
  return m;
}

} // namespace

TEST_CASE("FAB-INV-IMP-01: incremental index equals a full rebuild after every patch_gen commit") {
  const std::size_t seeds = tg::sweep_scaled(6);
  std::size_t commits = 0;
  for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
    CAPTURE(seed);
    auto ws = must(
        workspace::create_memory(must(tg::gen_options(std::make_shared<manual_clock>(1), seed))));
    transaction_manager &m = ws->manager();
    const tx_origin origin = tg::tx_origin_of(tg::tx_editor());
    tg::patch_gen_options gopts;
    gopts.meta_percent = 0;
    gopts.max_ops = 3;
    tg::patch_gen gen(seed * 7919, gopts);
    must(tg::commit_ops(m, origin, tg::load_patch(must(gen.state(10))).ops));

    impact_options opts;
    opts.edges = id_scan_edges;
    auto engine = must(impact_engine::attach(m, must(m.head()), opts));
    CHECK(must(engine->index()) ==
          must(impact_engine::rebuild(must(m.head()), ws->schemas(), opts)));

    std::size_t local = 0;
    std::size_t max_edges = 0;
    for (int step = 0; step < 40; ++step) {
      CAPTURE(step);
      const snapshot before = must(m.head());
      std::vector<patch_op> ops;
      if (gen.random().chance(40) && before.revision_digest()) {
        // A ref-carrying field between two records (or to a retired id).
        const std::vector<record_id> live = live_ids(before);
        if (!live.empty()) {
          const record_id from = live[gen.random().below(live.size())];
          const record_id to = live[gen.random().below(live.size())];
          ops.push_back(op_set(from, {path_segment::field(900)}, value::id(to)));
        }
      }
      if (ops.empty()) {
        patch p = must(gen.next(must(tg::oracle_of(before))));
        ops = std::move(p.ops);
      }
      const commit_receipt r = must(tg::commit_ops(m, origin, ops));
      const snapshot after = must(m.head());
      CHECK(must(engine->index()) == must(impact_engine::rebuild(after, ws->schemas(), opts)));
      CHECK(must(engine->index()).at_revision == after.revision_digest());
      check_receipt(*engine, r, diff_ids(before, after));
      max_edges = std::max(max_edges, must(engine->index()).graph.edge_count());
      ++local;
    }
    const impact_stats st = engine->stats();
    CHECK(max_edges > 0);
    CHECK(st.rebuilds == 1); // the attach: never a rebuild per commit
    CHECK(st.incremental == local);
    CHECK(st.failures == 0);
    commits += local;
  }
  CHECK(commits == seeds * 40);
}

TEST_CASE("FAB-INV-IMP-01: incremental index equals a full rebuild over the scene workload") {
  const std::size_t seeds = tg::sweep_scaled(4);
  for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
    CAPTURE(seed);
    auto ws = must(
        workspace::create_memory(must(tg::scene_options(std::make_shared<manual_clock>(5), seed))));
    transaction_manager &m = ws->manager();
    const tx_origin content = tg::tx_origin_of(tg::tx_editor(), "content");
    // Attached over the empty state: the first commit applies over no revision.
    auto engine = must(impact_engine::attach(m, must(m.head())));
    tg::rng r(seed * 104729);
    std::vector<digest> history;
    std::size_t committed = 0;
    std::size_t resets = 0;
    std::size_t serial = 0;
    edge_kind_mask kinds = 0;
    for (int step = 0; step < 60; ++step) {
      CAPTURE(step);
      const snapshot before = must(m.head());
      const scene_state st = read_scene(before);
      std::vector<patch_op> ops;
      tx_origin origin = content;
      const std::string name = "s" + std::to_string(seed) + "n" + std::to_string(serial++);
      bool reset = false;
      switch (r.below(10)) {
      case 0:
        ops.push_back(must(tg::create_mesh(name, 8)));
        break;
      case 1: {
        ops.push_back(must(tg::create_entity(name)));
        if (!st.meshes.empty()) {
          const record_id id = tg::entity_named(name);
          ops.push_back(op_insert(
              id, {path_segment::field(4)}, derive(id_class::element, id, "renderer").bytes(),
              must(order_key_between({}, {})), renderer_of(pick(r, st.meshes))));
        }
        break;
      }
      case 2: {
        const record_id scene = tg::entity_named(name);
        std::vector<record_id> members;
        for (int i = 0; i < 2; ++i) {
          const std::string member = name + "/e" + std::to_string(i);
          ops.push_back(must(tg::create_entity(member)));
          members.push_back(tg::entity_named(member));
        }
        ops.push_back(must(tg::create_scene(scene, name, members)));
        break;
      }
      case 3: {
        if (st.meshes.empty()) {
          break;
        }
        const record_id mesh = pick(r, st.meshes);
        const snapshot_entry src = must(before.get(mesh));
        provenance p = tg::complete_provenance(mesh);
        p.input_revisions = std::vector<digest>{src.object};
        origin = tg::tx_origin_of(tg::tx_editor(), "asset");
        origin.provenance = p;
        if (!st.cooked.empty() && r.chance(50)) {
          const record_id c = pick(r, st.cooked);
          ops.push_back(op_set(c, {path_segment::field(1)}, value::id(mesh)));
          ops.push_back(
              op_set(c, {path_segment::field(2)}, value::digest(text_digest("payload/" + name))));
        } else {
          ops.push_back(must(tg::create_cooked(name, mesh)));
        }
        break;
      }
      case 4:
        if (!st.meshes.empty()) {
          ops.push_back(
              op_set(pick(r, st.meshes), {path_segment::field(2)}, value::uint(r.below(1000))));
        }
        break;
      case 5:
        if (!st.entities.empty()) {
          ops.push_back(must(tg::set_name(pick(r, st.entities), name)));
        }
        break;
      case 6:
        if (!st.entities.empty()) {
          ops.push_back(op_delete(pick(r, st.entities)));
        }
        break;
      case 7:
        if (!st.cooked.empty()) {
          origin = tg::tx_origin_of(tg::tx_editor(), "asset");
          ops.push_back(op_delete(pick(r, st.cooked)));
        }
        break;
      case 8:
        if (!st.scenes.empty()) {
          const record_id scene = pick(r, st.scenes);
          ops.push_back(op_delete(scene));
          for (const record_id &e : st.members.at(scene)) {
            ops.push_back(op_delete(e));
          }
        } else if (!st.meshes.empty()) {
          ops.push_back(op_delete(pick(r, st.meshes))); // refused while referenced
        }
        break;
      default:
        reset = step >= 30 && history.size() > 4 && resets == 0;
        if (!reset && !st.meshes.empty()) {
          ops.push_back(op_delete(pick(r, st.meshes))); // refused while referenced
        }
        break;
      }
      if (step == 45 && resets == 0) {
        reset = true;
      }
      std::optional<commit_receipt> receipt;
      if (reset) {
        receipt = must(m.reset_ref("main", history[history.size() / 2], content));
        ++resets;
      } else if (!ops.empty()) {
        auto c = tg::commit_ops(m, origin, ops);
        if (!c) {
          REQUIRE_MESSAGE(c.error().code == errc::validation_failed, c.error().message);
          continue;
        }
        receipt = std::move(*c);
      } else {
        continue;
      }
      const snapshot after = must(m.head());
      history.push_back(receipt->revision);
      ++committed;
      const impact_index idx = must(engine->index());
      CHECK(idx == must(impact_engine::rebuild(after, ws->schemas())));
      CHECK(idx.at_revision == after.revision_digest());
      // The same graph from an independent model through dependency_graph::build.
      CHECK(idx.graph == must(dependency_graph::build(model_of(after), ws->schemas())));
      check_receipt(*engine, *receipt, diff_ids(before, after));
      for (const edge &e : idx.graph.edges()) {
        kinds |= mask_of(e.kind);
      }
      for (const auto &[id, pin] : idx.pins) {
        CHECK(must(after.get(id)).object == pin.record_object);
        CHECK(pin.inputs.size() == 1);
      }
    }
    CHECK(committed >= 20);
    CHECK(resets == 1);
    CHECK(kinds == (mask_of(edge_kind::reference) | mask_of(edge_kind::containment) |
                    mask_of(edge_kind::derives_from)));
    const impact_stats st = engine->stats();
    CHECK(st.rebuilds == 1 + resets); // the attach and each reset
    CHECK(st.incremental + resets == committed);
    CHECK(st.failures == 0);
  }
}

TEST_CASE("FAB-INV-IMP-01: an observer error never fails a commit and the next query rebuilds") {
  auto ws =
      must(workspace::create_memory(must(tg::gen_options(std::make_shared<manual_clock>(1), 3))));
  transaction_manager &m = ws->manager();
  const tx_origin origin = tg::tx_origin_of(tg::tx_editor());
  tg::patch_gen gen(31);
  must(tg::commit_ops(m, origin, tg::load_patch(must(gen.state(6))).ops));

  auto broken = std::make_shared<std::atomic<bool>>(false);
  impact_options opts;
  opts.edges = [broken](const record_view &rv,
                        const schema_registry &reg) -> sl::result<std::vector<edge>> {
    if (broken->load()) {
      return sl::make_error(errc::io_error, "edge source down");
    }
    return id_scan_edges(rv, reg);
  };
  auto engine = must(impact_engine::attach(m, must(m.head()), opts));

  broken->store(true);
  const commit_receipt r =
      must(tg::commit_ops(m, origin, must(gen.next(must(tg::oracle_of(must(m.head()))))).ops));
  CHECK(m.head().value().revision_digest() == r.revision); // committed regardless
  CHECK_FALSE(r.attachments.contains(std::string(impact_attachment)));
  CHECK(engine->needs_rebuild());
  CHECK(engine->stats().failures == 1);
  CHECK_FALSE(engine->committed(r.tx.id()).has_value());

  // Still broken: the query reports the rebuild's error and the mark stays.
  CHECK(engine->index().error().code == errc::io_error);
  CHECK(engine->needs_rebuild());

  broken->store(false);
  const impact_index idx = must(engine->index());
  CHECK_FALSE(engine->needs_rebuild());
  CHECK(idx == must(impact_engine::rebuild(must(m.head()), ws->schemas(), opts)));
  CHECK(engine->stats().rebuilds == 2);

  // Back to incremental updates.
  const snapshot before = must(m.head());
  const commit_receipt next =
      must(tg::commit_ops(m, origin, must(gen.next(must(tg::oracle_of(before)))).ops));
  check_receipt(*engine, next, diff_ids(before, must(m.head())));
  CHECK(engine->stats().incremental == 1);
  CHECK(must(engine->index()) == must(impact_engine::rebuild(must(m.head()), ws->schemas(), opts)));
}
