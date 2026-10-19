// sl_fabric_cli (WO-062): the Volume 04 end-to-end demo (`just fabric-demo`, system test
// fabric_demo).
//
//   sl_fabric_cli demo <dir>
//
// Works in `<dir>` (created; an existing `<dir>` is emptied only when it holds the demo marker
// file `.sl-fabric-demo`, else refused) and walks the whole Fabric through its public API:
//
//   1. create project      create_project in <dir>/project: three content.lantern types (material,
//                          mesh with a strong ref to a material, prop with a strong ref to a mesh)
//                          and one human editor; one transaction writes two materials, two meshes
//                          and two props
//   2. change + impact     set brass.glow; the impact engine's reverse closure of the change is
//                          printed and must hold brass, lantern-body and lantern-1 and nothing else
//   3. undo                undo_manager restores the old glow
//   4. commit              rename crate-1
//   5. reopen              close, open_project: same head revision, a lossless recovery
//   6. crash at FP4        a crash vfs over vfs_posix tears the commit frame of the next commit
//                          after it is written and before it is durable (failpoint FP4) and
//                          fails every later primitive: the commit is not acknowledged
//   7. recover             open_project with the plain vfs: the recovery report (lost bytes,
//                          truncated frames, quarantine, the stale lock taken over) is printed and
//                          the retry by idempotency key commits exactly once
//   8. counters            F2 (domains with more than one owner / domains) and F5 (store mutations
//                          bypassing the transaction manager / store mutations; "compiled out"
//                          when the ledger is not built in)
//
// The last line is `fabric-demo: ok` (exit 0); a failed step prints `fabric-demo: FAIL <step>:
// <why>` (exit 1). Exit 2 on bad usage. The same <dir> gives the same output but for the paths.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/clock.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/deps.hpp"
#include "sl/fabric/f5.hpp"
#include "sl/fabric/failpoints.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/impact.hpp"
#include "sl/fabric/manifest.hpp"
#include "sl/fabric/migration.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/recovery.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/undo.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/vfs.hpp"
#include "sl/fabric/workspace.hpp"

namespace {

using namespace sl::fabric;
using sl::errc;

constexpr std::string_view marker = ".sl-fabric-demo";

constexpr std::string_view lantern_module =
    R"({"format": "sl-type-def/1", "module": "content.lantern",
"domain": "content.lantern", "owner": "05", "doc": "WO-062 demo: lanterns of the valley.",
"types": [
 {"name": "content.lantern.material", "kind": "struct", "version": "1.0", "record_class": "asset",
  "state_class": "document", "doc": "A surface.", "fields": [
  {"no": 1, "name": "name", "type": "string", "presence": "required"},
  {"no": 2, "name": "glow", "type": "f64", "presence": "required"}]},
 {"name": "content.lantern.mesh", "kind": "struct", "version": "1.0", "record_class": "asset",
  "state_class": "document", "doc": "A shape.", "fields": [
  {"no": 1, "name": "name", "type": "string", "presence": "required"},
  {"no": 2, "name": "material", "type": "ref", "presence": "required",
   "ref": {"strength": "strong", "targets": ["content.lantern.material"]}},
  {"no": 3, "name": "vertices", "type": "u32", "presence": "required"}]},
 {"name": "content.lantern.prop", "kind": "struct", "version": "1.0", "record_class": "entity",
  "state_class": "document", "doc": "A placed thing.", "fields": [
  {"no": 1, "name": "name", "type": "string", "presence": "required"},
  {"no": 2, "name": "mesh", "type": "ref", "presence": "required",
   "ref": {"strength": "strong", "targets": ["content.lantern.mesh"]}}]}]})";

record_id demo_id(id_class c, std::string_view key) noexcept {
  return derive(c, well_known(id_class::namespace_, 1), "fabric-demo/" + std::string(key));
}

actor_id editor() noexcept {
  return actor_id::from(demo_id(id_class::actor, "editor")).value_or(actor_id{});
}

type_id material_type() { return type_id_of("content.lantern.material"); }
type_id mesh_type() { return type_id_of("content.lantern.mesh"); }
type_id prop_type() { return type_id_of("content.lantern.prop"); }

// The demo's records: name -> id.
struct cast {
  std::map<std::string, record_id, std::less<>> ids{
      {"brass", demo_id(id_class::asset, "brass")},
      {"iron", demo_id(id_class::asset, "iron")},
      {"lantern-body", demo_id(id_class::asset, "lantern-body")},
      {"crate", demo_id(id_class::asset, "crate")},
      {"lantern-1", demo_id(id_class::entity, "lantern-1")},
      {"crate-1", demo_id(id_class::entity, "crate-1")},
  };

  [[nodiscard]] std::string name_of(const record_id &id) const {
    for (const auto &[n, i] : ids) {
      if (i == id) {
        return n;
      }
    }
    return id.to_string();
  }
};

// A failed step: its name and why.
struct failure {
  std::string step;
  std::string why;
};

template <class T> using step_result = std::expected<T, failure>;

template <class T> step_result<T> in(std::string_view step, sl::result<T> r) {
  if (!r) {
    return std::unexpected(failure{std::string(step), r.error().message});
  }
  if constexpr (std::is_void_v<T>) {
    return {};
  } else {
    return std::move(*r);
  }
}

std::unexpected<failure> fail(std::string_view step, std::string why) {
  return std::unexpected(failure{.step = std::string(step), .why = std::move(why)});
}

// A vfs over vfs_posix that, once armed, turns the next fsync or fdatasync of a log segment
// (.slg) into a crash at FP4: half of the bytes appended since the last sync are cut (a torn
// commit frame), the sync fails with `store_io`, and every later primitive fails (the process is
// gone). close() still releases the descriptor, as the kernel does for a dead process.
class crash_vfs final : public vfs {
public:
  void arm() noexcept { armed_ = true; }
  [[nodiscard]] bool crashed() const noexcept { return dead_; }
  [[nodiscard]] std::uint64_t torn_bytes() const noexcept { return torn_; }

  sl::result<std::uint64_t> open_handle(const std::filesystem::path &path,
                                        const open_options &options) noexcept override {
    if (dead_) {
      return gone();
    }
    auto h = inner_.open_handle(path, options);
    if (h && path.extension() == ".slg") {
      segments_[*h] = std::nullopt;
    }
    return h;
  }
  void close(std::uint64_t handle) noexcept override {
    segments_.erase(handle);
    inner_.close(handle);
  }
  sl::result<std::uint64_t> append(std::uint64_t handle, byte_span data) noexcept override {
    if (dead_) {
      return gone();
    }
    if (auto it = segments_.find(handle); it != segments_.end() && !it->second) {
      auto size = inner_.size(handle);
      if (!size) {
        return std::unexpected(size.error());
      }
      it->second = *size;
    }
    return inner_.append(handle, data);
  }
  sl::result<void> pwrite(std::uint64_t handle, std::uint64_t offset,
                          byte_span data) noexcept override {
    if (dead_) {
      return gone();
    }
    return inner_.pwrite(handle, offset, data);
  }
  sl::result<std::size_t> read(std::uint64_t handle, std::uint64_t offset,
                               std::span<std::uint8_t> out) noexcept override {
    if (dead_) {
      return gone();
    }
    return inner_.read(handle, offset, out);
  }
  sl::result<std::uint64_t> size(std::uint64_t handle) noexcept override {
    if (dead_) {
      return gone();
    }
    return inner_.size(handle);
  }
  sl::result<void> truncate(std::uint64_t handle, std::uint64_t length) noexcept override {
    if (dead_) {
      return gone();
    }
    return inner_.truncate(handle, length);
  }
  sl::result<void> fsync(std::uint64_t handle) noexcept override {
    if (auto c = crash_at(handle); !c) {
      return c;
    }
    return inner_.fsync(handle);
  }
  sl::result<void> fdatasync(std::uint64_t handle) noexcept override {
    if (auto c = crash_at(handle); !c) {
      return c;
    }
    return inner_.fdatasync(handle);
  }
  sl::result<void> sync_dir(const std::filesystem::path &dir) noexcept override {
    if (dead_) {
      return gone();
    }
    return inner_.sync_dir(dir);
  }
  sl::result<void> rename(const std::filesystem::path &from,
                          const std::filesystem::path &to) noexcept override {
    if (dead_) {
      return gone();
    }
    return inner_.rename(from, to);
  }
  sl::result<void> remove(const std::filesystem::path &path) noexcept override {
    if (dead_) {
      return gone();
    }
    return inner_.remove(path);
  }
  sl::result<void> make_dir(const std::filesystem::path &dir) noexcept override {
    if (dead_) {
      return gone();
    }
    return inner_.make_dir(dir);
  }
  sl::result<std::vector<std::string>> list(const std::filesystem::path &dir) noexcept override {
    if (dead_) {
      return gone();
    }
    return inner_.list(dir);
  }
  sl::result<bool> exists(const std::filesystem::path &path) noexcept override {
    if (dead_) {
      return gone();
    }
    return inner_.exists(path);
  }
  sl::result<void> lock_ofd(std::uint64_t handle) noexcept override {
    if (dead_) {
      return gone();
    }
    return inner_.lock_ofd(handle);
  }
  sl::result<void> unlock(std::uint64_t handle) noexcept override {
    if (dead_) {
      return gone();
    }
    return inner_.unlock(handle);
  }

private:
  static std::unexpected<sl::error> gone() {
    return sl::make_error(errc::store_io, "crash_vfs: the process crashed at FP4");
  }

  // The FP4 crash on an armed sync of a segment with unsynced appends; clears the mark otherwise.
  sl::result<void> crash_at(std::uint64_t handle) noexcept {
    if (dead_) {
      return gone();
    }
    auto it = segments_.find(handle);
    if (it == segments_.end() || !it->second) {
      return {};
    }
    const std::uint64_t start = it->second.value_or(0);
    it->second = std::nullopt;
    if (!armed_) {
      return {};
    }
    auto end = inner_.size(handle);
    if (!end) {
      return std::unexpected(end.error());
    }
    const std::uint64_t cut = start + ((*end - start) / 2);
    if (auto t = inner_.truncate(handle, cut); !t) {
      return t;
    }
    torn_ = *end - cut;
    dead_ = true;
    return gone();
  }

  vfs_posix inner_;
  std::map<std::uint64_t, std::optional<std::uint64_t>> segments_; // handle -> unsynced from
  bool armed_ = false;
  bool dead_ = false;
  std::uint64_t torn_ = 0;
};

sl::result<workspace_options> demo_options(std::shared_ptr<vfs> fs, std::int64_t now_us) {
  workspace_options o;
  auto doc = parse_json(lantern_module);
  if (!doc) {
    return std::unexpected(doc.error());
  }
  auto types = types_from_module(*doc);
  if (!types) {
    return std::unexpected(types.error());
  }
  for (type_desc &t : *types) {
    if (auto ok = o.schemas.add(std::move(t)); !ok) {
      return std::unexpected(ok.error());
    }
  }
  if (auto ok = o.domains.claim(domain_spec{.name = "content.lantern",
                                            .owner = subsystem::content,
                                            .state_classes = {state_class::document},
                                            .types = {}});
      !ok) {
    return std::unexpected(ok.error());
  }
  if (auto ok = o.actors.add(actor{.id = editor(), .kind = actor_kind::human, .grants = {}}); !ok) {
    return std::unexpected(ok.error());
  }
  o.clock = std::make_shared<manual_clock>(now_us);
  o.ids = std::make_shared<deterministic_source>(62, "fabric-demo");
  o.fs = std::move(fs);
  return o;
}

tx_options editor_tx(std::string label, std::string idempotency_key = {}) {
  tx_options o;
  o.origin.actor = editor();
  o.origin.subsystem = "content";
  o.meta.label = std::move(label);
  o.meta.idempotency_key = std::move(idempotency_key);
  return o;
}

sl::result<commit_receipt> commit_ops(transaction_manager &m, tx_options o,
                                      const std::vector<patch_op> &ops) {
  auto t = m.begin(std::move(o));
  if (!t) {
    return std::unexpected(t.error());
  }
  for (const patch_op &op : ops) {
    if (auto ok = t->stage(op); !ok) {
      return std::unexpected(ok.error());
    }
  }
  return t->commit();
}

sl::result<patch_op> create(const workspace &ws, const record_id &id, const type_id &type,
                            const std::string &text) {
  auto body = from_text(text, type, ws.schemas());
  if (!body) {
    return std::unexpected(body.error());
  }
  return op_create(id, type, std::move(*body));
}

step_result<double> glow_of(const workspace &ws, const record_id &id) {
  auto head = in("read", ws.manager().head());
  if (!head) {
    return std::unexpected(head.error());
  }
  auto e = in("read", head->get(id));
  if (!e) {
    return std::unexpected(e.error());
  }
  const value *g = e->body.find_field(2);
  if (g == nullptr || g->kind() != value_kind::f64) {
    return fail("read", "brass has no glow");
  }
  return g->as_f64();
}

std::string hex8(const std::optional<digest> &d) {
  return d ? d->to_hex().substr(0, 16) : std::string("none");
}

void print_recovery(const recovery_report &r) {
  std::println("  recovery: lost bytes {}, truncated frames {}, orphan objects {}, last seq {}",
               r.lost_bytes, r.truncated_frames, r.orphan_objects, r.last_seq);
  std::println("  recovery: frames scanned {}, commits scanned {}, checkpoint used {}",
               r.frames_scanned, r.commits_scanned, r.used_checkpoint ? "yes" : "no");
  std::println("  recovery: quarantined {} file(s), never deleted", r.quarantined.size());
  for (const std::string &q : r.quarantined) {
    std::println("    {}", q);
  }
  std::println("  recovery: stale lock {}",
               r.stale_lock ? "taken over (C11), previous holder recorded" : "none");
  for (const recovery_finding &f : r.findings) {
    std::println("  recovery: finding {}", f.message);
  }
}

step_result<void> run_demo(const std::filesystem::path &dir) {
  const cast c;
  const std::filesystem::path root = dir / "project";
  constexpr std::int64_t t0 = 1'760'000'000'000'000;

  // 1. create project
  std::println("fabric-demo: 1 create project {}", root.string());
  project_manifest pm;
  pm.project = demo_id(id_class::project, "valley");
  pm.name = "Valley of Lanterns demo";
  pm.default_branch = "main";
  auto opts = in("create", demo_options(nullptr, t0));
  if (!opts) {
    return std::unexpected(opts.error());
  }
  auto ws = in("create", create_project(root, pm, engine_profile{}, editor(), std::move(*opts)));
  if (!ws) {
    return std::unexpected(ws.error());
  }
  transaction_manager &m = (*ws)->manager();
  const auto &id = c.ids;
  std::vector<patch_op> ops;
  const std::vector<std::tuple<std::string, type_id, std::string>> records{
      {"brass", material_type(), R"({"name":"brass","glow":0.5})"},
      {"iron", material_type(), R"({"name":"iron","glow":0.0})"},
      {"lantern-body", mesh_type(),
       std::format(R"({{"name":"lantern body","material":"{}","vertices":480}})",
                   id.at("brass").to_string())},
      {"crate", mesh_type(),
       std::format(R"({{"name":"crate","material":"{}","vertices":24}})",
                   id.at("iron").to_string())},
      {"lantern-1", prop_type(),
       std::format(R"({{"name":"lantern 1","mesh":"{}"}})", id.at("lantern-body").to_string())},
      {"crate-1", prop_type(),
       std::format(R"({{"name":"crate 1","mesh":"{}"}})", id.at("crate").to_string())},
  };
  for (const auto &[name, type, text] : records) {
    auto op = in("create", create(**ws, id.at(name), type, text));
    if (!op) {
      return std::unexpected(op.error());
    }
    ops.push_back(std::move(*op));
  }
  auto genesis = in("create", m.head());
  if (!genesis) {
    return std::unexpected(genesis.error());
  }
  auto engine = in("create", impact_engine::attach(m, *genesis, impact_options{}));
  if (!engine) {
    return std::unexpected(engine.error());
  }
  // Destroyed before the workspace: it observes the manager.
  auto undo = std::make_unique<undo_manager>(m, editor());
  auto made = in("create", commit_ops(m, editor_tx("place lanterns"), ops));
  if (!made) {
    return std::unexpected(made.error());
  }
  std::println("  committed {} records at revision {}", records.size(), hex8(made->revision));

  // 2. change one property, print the impact set
  std::println("fabric-demo: 2 change brass.glow 0.5 -> 0.9");
  auto glow = in("change", value::f64(0.9));
  if (!glow) {
    return std::unexpected(glow.error());
  }
  auto changed =
      in("change", commit_ops(m, editor_tx("brighten brass"),
                              {op_set(id.at("brass"), {path_segment::field(2)}, *glow)}));
  if (!changed) {
    return std::unexpected(changed.error());
  }
  const std::vector<record_id> seed{id.at("brass")};
  auto set = in("impact", (*engine)->impact(seed));
  if (!set) {
    return std::unexpected(set.error());
  }
  std::vector<std::string> affected;
  for (const impact_node &n : set->nodes) {
    affected.push_back(c.name_of(n.id));
    std::println("  impact: {} (distance {}{})", c.name_of(n.id), n.distance,
                 n.distance == 0
                     ? ", changed"
                     : std::format(", via {} {}", to_string(n.via_kind), c.name_of(n.via)));
  }
  const std::vector<std::string> expected{"brass", "lantern-body", "lantern-1"};
  if (affected != expected) {
    return fail("impact", "the impact set is not exactly brass, lantern-body, lantern-1");
  }
  std::println("  impact: unaffected iron, crate, crate-1 are not in the set");

  // 3. undo
  std::println("fabric-demo: 3 undo");
  auto undone = in("undo", undo->undo());
  if (!undone) {
    return std::unexpected(undone.error());
  }
  auto g = glow_of(**ws, id.at("brass"));
  if (!g) {
    return std::unexpected(g.error());
  }
  if (*g != 0.5) {
    return fail("undo", std::format("brass.glow is {} after undo, expected 0.5", *g));
  }
  std::println("  brass.glow is {} again (revision {})", *g, hex8(undone->revision));

  // 4. commit
  std::println("fabric-demo: 4 commit rename crate-1");
  auto name = in("commit", value::string("supply crate"));
  if (!name) {
    return std::unexpected(name.error());
  }
  auto renamed =
      in("commit", commit_ops(m, editor_tx("rename crate"),
                              {op_set(id.at("crate-1"), {path_segment::field(1)}, *name)}));
  if (!renamed) {
    return std::unexpected(renamed.error());
  }
  const digest head_before = renamed->revision;
  std::println("  committed revision {}", hex8(head_before));
  undo.reset();
  engine->reset();
  ws->reset();

  // 5. reopen
  std::println("fabric-demo: 5 reopen");
  opts = in("reopen", demo_options(nullptr, t0 + 10'000'000));
  if (!opts) {
    return std::unexpected(opts.error());
  }
  ws = in("reopen", open_project(root, open_mode::write, recovery_policy{}, engine_profile{},
                                 std::move(*opts)));
  if (!ws) {
    return std::unexpected(ws.error());
  }
  auto head = in("reopen", (*ws)->manager().head());
  if (!head) {
    return std::unexpected(head.error());
  }
  if (head->revision_digest() != std::optional<digest>(head_before)) {
    return fail("reopen", "the reopened head is not the last committed revision");
  }
  if (!(*ws)->last_recovery().lossless()) {
    return fail("reopen", "the clean reopen was not lossless: " + (*ws)->last_recovery().summary());
  }
  std::println("  head {} unchanged, recovery lossless", hex8(head->revision_digest()));
  ws->reset();

  // 6. crash at FP4
  const failpoint_info &fp4 = failpoint_table()[static_cast<std::size_t>(failpoint::fp4)];
  std::println("fabric-demo: 6 crash at {} ({}; {})", fp4.name, fp4.at, fp4.outcome);
  auto fs = std::make_shared<crash_vfs>();
  opts = in("crash", demo_options(fs, t0 + 20'000'000));
  if (!opts) {
    return std::unexpected(opts.error());
  }
  ws = in("crash", open_project(root, open_mode::write, recovery_policy{}, engine_profile{},
                                std::move(*opts)));
  if (!ws) {
    return std::unexpected(ws.error());
  }
  auto bright = in("crash", value::f64(0.75));
  if (!bright) {
    return std::unexpected(bright.error());
  }
  const patch_op crash_op = op_set(id.at("iron"), {path_segment::field(2)}, *bright);
  fs->arm();
  auto lost =
      commit_ops((*ws)->manager(), editor_tx("warm iron", "fabric-demo/warm-iron"), {crash_op});
  if (lost) {
    return fail("crash", "the commit was acknowledged although its frame was torn before fsync");
  }
  if (!fs->crashed()) {
    return fail("crash", "the crash vfs never reached FP4: " + lost.error().message);
  }
  std::println("  commit not acknowledged: {} ({})", sl::to_string(lost.error().code),
               lost.error().message);
  std::println("  torn: {} of the commit's unsynced bytes never reached the disk",
               fs->torn_bytes());
  ws->reset();

  // 7. recover
  std::println("fabric-demo: 7 recover");
  opts = in("recover", demo_options(nullptr, t0 + 30'000'000));
  if (!opts) {
    return std::unexpected(opts.error());
  }
  ws = in("recover", open_project(root, open_mode::write, recovery_policy{}, engine_profile{},
                                  std::move(*opts)));
  if (!ws) {
    return std::unexpected(ws.error());
  }
  const recovery_report &r = (*ws)->last_recovery();
  print_recovery(r);
  if (r.lost_bytes == 0 || !r.stale_lock) {
    return fail("recover", "the recovery report shows no torn tail or no stale lock");
  }
  head = in("recover", (*ws)->manager().head());
  if (!head) {
    return std::unexpected(head.error());
  }
  const bool survived = head->revision_digest() != std::optional<digest>(head_before);
  std::println("  head {}: the torn commit {}", hex8(head->revision_digest()),
               survived ? "survived (its frame was intact)"
                        : "is gone (decided by frame intactness)");
  auto retry =
      in("recover",
         commit_ops((*ws)->manager(), editor_tx("warm iron", "fabric-demo/warm-iron"), {crash_op}));
  if (!retry) {
    return std::unexpected(retry.error());
  }
  auto g2 = glow_of(**ws, id.at("iron"));
  if (!g2) {
    return std::unexpected(g2.error());
  }
  auto again =
      in("recover",
         commit_ops((*ws)->manager(), editor_tx("warm iron", "fabric-demo/warm-iron"), {crash_op}));
  if (!again) {
    return std::unexpected(again.error());
  }
  if (*g2 != 0.75 || again->revision != retry->revision) {
    return fail("recover", "the retry by idempotency key did not commit exactly once");
  }
  std::println("  retry by idempotency key: committed once at {}, a second retry returns it",
               hex8(retry->revision));

  // 8. counters
  std::println("fabric-demo: 8 counters");
  const f2_report f2 = (*ws)->domains().f2();
  std::println("  F2 domains with more than one owner: {}/{}", f2.numerator, f2.denominator);
  if (f2.numerator != 0) {
    return fail("counters", "F2 numerator is not 0");
  }
  if constexpr (f5::enabled) {
    const f5::ledger l = f5::read();
    std::println("  F5 store mutations bypassing the transaction manager: {}/{}", l.bypassing,
                 l.mutations);
    if (l.bypassing != 0 || l.mutations == 0) {
      return fail("counters", "F5 numerator is not 0 or nothing was measured");
    }
  } else {
    std::println("  F5 store mutations bypassing the transaction manager: compiled out (release)");
  }
  ws->reset();
  return {};
}

// Creates `dir`, emptying an earlier demo directory (it holds the marker) and refusing any other
// non-empty directory.
step_result<void> prepare(const std::filesystem::path &dir) {
  std::error_code ec;
  if (std::filesystem::exists(dir, ec)) {
    const bool empty = std::filesystem::is_empty(dir, ec);
    if (ec) {
      return fail("prepare", dir.string() + ": " + ec.message());
    }
    if (!empty && !std::filesystem::exists(dir / marker, ec)) {
      return fail("prepare", dir.string() + " is not empty and holds no " + std::string(marker));
    }
    std::filesystem::remove_all(dir, ec);
    if (ec) {
      return fail("prepare", dir.string() + ": " + ec.message());
    }
  }
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    return fail("prepare", dir.string() + ": " + ec.message());
  }
  std::FILE *f = std::fopen((dir / marker).string().c_str(), "wb");
  if (f == nullptr) {
    return fail("prepare", "cannot write " + (dir / marker).string());
  }
  (void)std::fclose(f);
  return {};
}

int usage() {
  std::fputs("usage: sl_fabric_cli demo <dir>\n", stderr);
  return 2;
}

int run(std::span<char *> args) {
  if (args.size() != 3 || std::string_view(args[1]) != "demo") {
    return usage();
  }
  const std::filesystem::path dir = args[2];
  step_result<void> r = prepare(dir);
  if (r) {
    r = run_demo(dir);
  }
  if (!r) {
    std::println("fabric-demo: FAIL {}: {}", r.error().step, r.error().why);
    return 1;
  }
  std::println("fabric-demo: ok");
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  const std::span<char *> args(argv, static_cast<std::size_t>(argc));
  try {
    return run(args);
  } catch (const std::exception &e) {
    std::fputs("sl_fabric_cli: ", stderr);
    std::fputs(e.what(), stderr);
    std::fputs("\n", stderr);
  } catch (...) {
    std::fputs("sl_fabric_cli: unknown error\n", stderr);
  }
  return 2;
}
