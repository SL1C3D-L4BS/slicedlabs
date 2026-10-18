// FAB-INV-MAN-01 (SL-ARCH-001 ARCH1-R32, ADR-0011): exactly one manifest at the well-known id
// (entity, ordinal 1); a second is manifest_invalid. create_project() leaves the manifest at
// manifest_id() on the default branch, decoded field for field; a commit creating a manifest at
// any other id is refused by the manifest validator (R-MAN-ONE, manifest_invalid), deleting it is
// manifest_missing (R-MAN-MISSING); a second manifest planted past the validator makes
// open_project() and read_manifest() refuse with manifest_invalid naming it; a store with no
// manifest is manifest_missing.

#include "sl/core/error.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/manifest.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/vfs.hpp"
#include "sl/fabric/workspace.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
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

bool has_rule(const std::string &message, std::string_view rule, std::string_view code) {
  return message.contains(std::string(rule) + " " + std::string(code));
}

} // namespace

TEST_CASE("FAB-INV-MAN-01: exactly one manifest at the well-known id; a second is "
          "manifest_invalid") {
  const auto fs = std::make_shared<vfs_memory>();
  {
    auto ws = must(create_project(root(), sample(), profile(), creator(), options(fs)));
    const located_manifest found = must(find_manifest(ws->store()));
    CHECK(found.ref == "main");
    CHECK(manifest_id() == well_known(id_class::entity, 1));
    project_manifest expect = sample();
    expect.schema_set = found.manifest.schema_set;
    CHECK(found.manifest == expect);
    CHECK(found.manifest.schema_set != digest{});
    const snapshot head = must(ws->manager().head("main"));
    CHECK(must(head.get(manifest_id())).type == manifest_type());

    // A second manifest at another id is refused.
    const record_id second = fixture_id(id_class::entity, "manifest/second");
    const auto refused =
        commit_ops(*ws, creator(), {op_create(second, manifest_type(), must(sample().to_value()))});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == errc::validation_failed);
    CHECK(has_rule(refused.error().message, "R-MAN-ONE", "manifest_invalid"));
    CHECK(refused.error().message.find(second.to_string()) != std::string::npos);

    // The manifest cannot be deleted.
    const auto deleted = commit_ops(*ws, creator(), {op_delete(manifest_id())});
    REQUIRE_FALSE(deleted.has_value());
    CHECK(deleted.error().code == errc::validation_failed);
    CHECK(has_rule(deleted.error().message, "R-MAN-MISSING", "manifest_missing"));
    CHECK(must(ws->manager().head("main")).revision_digest() == head.revision_digest());
  }
  CHECK(must(open_project(root(), open_mode::write, recovery_policy{}, profile(), options(fs)))
            ->mode() == open_mode::write);

  // Planted past the validator (a plain workspace without it), a second manifest refuses open.
  const record_id planted = fixture_id(id_class::entity, "manifest/planted");
  {
    workspace_options o = options(fs);
    must_ok(prepare_manifest_options(o));
    auto plain = must(workspace::open(root(), open_mode::write, recovery_policy{}, std::move(o)));
    must(commit_ops(*plain, creator(),
                    {op_create(planted, manifest_type(), must(sample().to_value()))}));
    const auto read = read_manifest(must(plain->manager().head("main")));
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error().code == errc::manifest_invalid);
  }
  const auto opened =
      open_project(root(), open_mode::write, recovery_policy{}, profile(), options(fs));
  REQUIRE_FALSE(opened.has_value());
  CHECK(opened.error().code == errc::manifest_invalid);
  CHECK(opened.error().message.find(planted.to_string()) != std::string::npos);
  CHECK(opened.error().message.find("second manifest") != std::string::npos);
}

TEST_CASE("FAB-INV-MAN-01: a store with no manifest is manifest_missing") {
  const auto fs = std::make_shared<vfs_memory>();
  {
    auto bare = must(workspace::create(root(), options(fs)));
    CHECK(bare->manager().refs().empty());
  }
  must_ok(write_locator(*fs, root(),
                        project_locator{.format = std::string(locator_format),
                                        .store = std::string(workspace::store_dir_name),
                                        .project = sample().project},
                        durability::none));
  const auto opened =
      open_project(root(), open_mode::write, recovery_policy{}, profile(), options(fs));
  REQUIRE_FALSE(opened.has_value());
  CHECK(opened.error().code == errc::manifest_missing);

  // A snapshot without the record reads as manifest_missing too.
  auto ws = must(workspace::open(root(), open_mode::read_only, recovery_policy{}, options(fs)));
  const auto none = read_manifest(must(ws->manager().head("main")));
  REQUIRE_FALSE(none.has_value());
  CHECK(none.error().code == errc::manifest_missing);
}
