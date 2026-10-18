// FAB-INV-MAN-05 (SL-ARCH-001 ARCH1-R33; master plan section 3, F5): the manifest changes only
// through a transaction; package pins are unique. create_project() writes the manifest as the
// genesis transaction through the manager: the first revision (generation 0) of the default branch,
// the tx record naming the creating actor, subsystem 02 and epistemic class asserted, and the F5
// ledger counting mutations but no bypass. A manifest edit is a transaction (new revision, the old
// manifest still in history); the workspace hands out the store only as const. The manifest
// validator refuses a pin repeating a package id or a mount (R-MAN-PIN), a malformed body
// (R-MAN-SHAPE), a changed project id (R-MAN-PROJECT) and another type at the manifest id
// (R-MAN-TYPE); an agent is denied by the authority stage; nothing is written by a refused edit.

#include "sl/core/error.hpp"
#include "sl/fabric/f5.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/manifest.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/provenance.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/vfs.hpp"
#include "sl/fabric/workspace.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;
using sl::errc;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

[[maybe_unused]] void must_ok(const sl::result<void> &r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
}

std::filesystem::path root() { return "/proj"; }

record_id fixture_id(id_class c, std::string_view key) noexcept {
  return derive(c, well_known(id_class::namespace_, 1), key);
}

actor_id creator() noexcept {
  return actor_id::from(fixture_id(id_class::actor, "manifest/creator")).value_or(actor_id{});
}

[[maybe_unused]] actor_id agent() noexcept {
  return actor_id::from(fixture_id(id_class::actor, "manifest/agent")).value_or(actor_id{});
}

engine_profile profile(format_version format = engine_format) {
  engine_profile p;
  p.format = format;
  p.modules = {{"sl.render", {.major = 2, .minor = 3}}, {"sl.physics", {.major = 1, .minor = 0}}};
  p.capabilities = {"cap.audio.spatial", "cap.gpu.compute"};
  p.target_profiles = {"desktop-linux", "desktop-windows"};
  return p;
}

[[maybe_unused]] version_range range(std::uint32_t lo_major, std::uint32_t lo_minor,
                                     std::uint32_t hi_major, std::uint32_t hi_minor) {
  return version_range{.at_least = {.major = lo_major, .minor = lo_minor},
                       .below = {.major = hi_major, .minor = hi_minor}};
}

project_manifest sample() {
  project_manifest m;
  m.project = fixture_id(id_class::project, "manifest/project");
  m.name = "Valley of Lanterns";
  m.writer_format = engine_format;
  m.min_reader_format = engine_format;
  m.modules = {{"sl.render", range(2, 0, 3, 0)}};
  m.capabilities = {"cap.gpu.compute"};
  m.target_profiles = {"desktop-linux"};
  m.default_branch = "main";
  return m;
}

workspace_options options(std::shared_ptr<vfs> fs) {
  workspace_options o;
  must_ok(o.actors.add(actor{.id = creator(), .kind = actor_kind::human, .grants = {}}));
  must_ok(o.actors.add(actor{.id = agent(), .kind = actor_kind::agent, .grants = {}}));
  o.clock = std::make_shared<manual_clock>(1'000'000);
  o.ids = std::make_shared<deterministic_source>(54, "manifest-test");
  o.fs = std::move(fs);
  return o;
}

[[maybe_unused]] tx_options edit_by(const actor_id &actor) {
  tx_options o;
  o.origin.actor = actor;
  o.origin.subsystem = "02";
  return o;
}

// Stages `ops` as `actor` and commits.
[[maybe_unused]] sl::result<commit_receipt> commit_ops(workspace &ws, const actor_id &actor,
                                                       const std::vector<patch_op> &ops) {
  auto t = ws.manager().begin(edit_by(actor));
  if (!t) {
    return std::unexpected(t.error());
  }
  for (const patch_op &op : ops) {
    if (auto ok = t->stage(op); !ok) {
      return std::unexpected(ok.error());
    }
  }
  auto r = t->commit();
  if (!r) {
    std::string why = r.error().message;
    for (const diagnostic &d : t->diagnostics()) {
      why += "\n" + d.rule + " " + d.message;
    }
    return sl::make_error(r.error().code, why);
  }
  return r;
}

static_assert(
    std::is_const_v<std::remove_reference_t<decltype(std::declval<workspace &>().store())>>,
    "the workspace hands out its store only as const");

digest revision_named(std::string_view text) {
  digest d;
  d.bytes = sha256::hash(text);
  return d;
}

gen::oset_item<package_pin> pin(std::string_view package, std::string_view mount,
                                std::vector<std::uint8_t> key) {
  const record_id id = fixture_id(id_class::package, package);
  return gen::oset_item<package_pin>{.element = derive(id_class::element, id, mount),
                                     .order_key = std::move(key),
                                     .v = package_pin{.package = id,
                                                      .revision = revision_named(package),
                                                      .mount = std::string(mount)}};
}

// insert_elem of `p` into the manifest's package_pins (field 9).
patch_op insert_pin(const gen::oset_item<package_pin> &p) {
  return op_insert(manifest_id(), sl::fabric::path{path_segment::field(9)}, p.element.bytes(),
                   p.order_key, must(p.v.to_value()));
}

void refused_with(const sl::result<commit_receipt> &r, std::string_view rule,
                  std::string_view code) {
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == errc::validation_failed);
  CAPTURE(r.error().message);
  CHECK(r.error().message.contains(std::string(rule) + " " + std::string(code)));
}

} // namespace

TEST_CASE("FAB-INV-MAN-05: the manifest changes only through a transaction; package pins are "
          "unique") {
  const auto fs = std::make_shared<vfs_memory>();
  const f5::ledger at_start = f5::read();
  auto ws = must(create_project(root(), sample(), profile(), creator(), options(fs)));

  // Genesis: one transaction through the manager, asserted by the creator.
  const snapshot genesis = must(ws->manager().head("main"));
  CHECK(genesis.generation() == 0);
  const auto rev =
      must(decode_revision(must(ws->store().get(genesis.revision_digest().value_or(digest{})))));
  const auto tx = must(decode_tx_record(must(ws->store().get(rev.tx))));
  CHECK(tx.actor == creator().id());
  CHECK(tx.origin.subsystem == "02");
  CHECK(tx.origin.epistemic == epistemic_class::asserted);
  CHECK(ws->manager().commits() == 1);
  if constexpr (f5::enabled) {
    const f5::ledger after = f5::read();
    CHECK(after.mutations > at_start.mutations);
    CHECK(after.bypassing == at_start.bypassing);
  }

  // An edit is a transaction; the old manifest stays in history.
  project_manifest renamed = must(find_manifest(ws->store())).manifest;
  const project_manifest original = renamed;
  renamed.name = "Valley of Lanterns II";
  renamed.package_pins = {pin("pkg/a", "lib/a", {0x40})};
  const commit_receipt edit = must(commit_ops(*ws, creator(), {must(manifest_set_op(renamed))}));
  CHECK(must(read_manifest(must(ws->manager().head("main")))) == renamed);
  CHECK(must(read_manifest(must(ws->manager().at(genesis.revision_digest().value_or(digest{}))))) ==
        original);
  CHECK(must(ws->manager().head("main")).generation() == 1);
  if constexpr (f5::enabled) {
    CHECK(f5::read().bypassing == at_start.bypassing);
  }

  // Unique pins: by package id and by mount.
  refused_with(commit_ops(*ws, creator(), {insert_pin(pin("pkg/a", "lib/other", {0x60}))}),
               "R-MAN-PIN", "manifest_invalid");
  refused_with(commit_ops(*ws, creator(), {insert_pin(pin("pkg/b", "lib/a", {0x60}))}), "R-MAN-PIN",
               "manifest_invalid");
  must(commit_ops(*ws, creator(), {insert_pin(pin("pkg/b", "lib/b", {0x60}))}));
  CHECK(must(read_manifest(must(ws->manager().head("main")))).package_pins.size() == 2);

  // Other malformed edits.
  project_manifest bad = renamed;
  bad.default_branch = "has space";
  refused_with(commit_ops(*ws, creator(), {must(manifest_set_op(bad))}), "R-MAN-SHAPE",
               "manifest_invalid");
  bad = renamed;
  bad.project = fixture_id(id_class::project, "manifest/usurper");
  refused_with(commit_ops(*ws, creator(), {must(manifest_set_op(bad))}), "R-MAN-PROJECT",
               "manifest_invalid");
  refused_with(commit_ops(*ws, creator(),
                          {op_retype(manifest_id(), type_id_of("fabric.manifest.format_version"),
                                     must(engine_format.to_value()))}),
               "R-MAN-TYPE", "manifest_invalid");

  // The authority stage: an agent has no grant.
  const auto denied = commit_ops(*ws, agent(), {must(manifest_set_op(renamed))});
  REQUIRE_FALSE(denied.has_value());
  CHECK(denied.error().code == errc::validation_failed);

  // Nothing of the refused edits was written.
  CHECK(ws->manager().commits() == 3);
  CHECK(must(ws->manager().head("main")).generation() == 2);
  CHECK(edit.revision != digest{});
}
