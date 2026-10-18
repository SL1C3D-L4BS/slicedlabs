// FAB-INV-MAN-03 (SL-ARCH-001 ARCH1-R34): a minimum reader above the engine is reader_too_old; a
// newer writer minor opens read-only. check_open(): minimum reader 1.2 against an engine of 1.1 is
// reader_too_old naming both versions (also across a major), equal or older readers pass; a writer
// format newer than the engine (minor or major, the minimum reader admitting it) is
// open_mode::read_only, otherwise write; a minimum reader above the writer is manifest_invalid.
// At the workspace: a project written by an engine of 1.3 opens read-only on an engine of 1.0
// (commits are permission_denied) even when write was asked for, and one requiring a 1.2 reader is
// refused on 1.1 with nothing opened.

#include "sl/core/error.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/manifest.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/vfs.hpp"
#include "sl/fabric/workspace.hpp"

#include <doctest/doctest.h>

#include <compare>
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

format_version v(std::uint32_t major, std::uint32_t minor) {
  return format_version{.major = major, .minor = minor};
}

} // namespace

TEST_CASE("FAB-INV-MAN-03: minimum reader above the engine is reader_too_old; newer writer minor "
          "opens read-only (check_open)") {
  CHECK(std::is_lt(compare(v(1, 2), v(1, 10))));
  CHECK(std::is_gt(compare(v(2, 0), v(1, 10))));
  CHECK(to_string(v(1, 2)) == "1.2");
  CHECK(to_string(range(1, 2, 2, 0)) == "[1.2, 2.0)");

  project_manifest m = sample();
  m.writer_format = v(1, 2);
  m.min_reader_format = v(1, 2);
  auto r = check_open(m, profile(v(1, 1)));
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == errc::reader_too_old);
  CHECK(r.error().message.find("1.2") != std::string::npos);
  CHECK(r.error().message.find("1.1") != std::string::npos);
  CHECK(must(check_open(m, profile(v(1, 2)))) == open_mode::write);
  CHECK(must(check_open(m, profile(v(1, 3)))) == open_mode::write);

  m.writer_format = v(2, 0);
  m.min_reader_format = v(2, 0);
  CHECK(check_open(m, profile(v(1, 9))).error().code == errc::reader_too_old);

  // A newer writer whose minimum reader admits this engine: read-only.
  m.writer_format = v(1, 3);
  m.min_reader_format = v(1, 0);
  CHECK(must(check_open(m, profile(v(1, 0)))) == open_mode::read_only);
  CHECK(must(check_open(m, profile(v(1, 2)))) == open_mode::read_only);
  CHECK(must(check_open(m, profile(v(1, 3)))) == open_mode::write);
  m.writer_format = v(2, 1);
  CHECK(must(check_open(m, profile(v(1, 4)))) == open_mode::read_only);

  // The reader check comes first; a minimum reader above the writer is malformed.
  m.writer_format = v(1, 0);
  m.min_reader_format = v(1, 1);
  CHECK(check_open(m, profile(v(1, 0))).error().code == errc::reader_too_old);
  CHECK(check_open(m, profile(v(1, 1))).error().code == errc::manifest_invalid);
}

TEST_CASE("FAB-INV-MAN-03: minimum reader above the engine is reader_too_old; newer writer minor "
          "opens read-only (workspace)") {
  const auto fs = std::make_shared<vfs_memory>();
  {
    // An engine of 1.3 writes a project any 1.x reader may read.
    project_manifest m = sample();
    m.min_reader_format = format_version{};
    auto ws = must(create_project(root(), m, profile(v(1, 3)), creator(), options(fs)));
    const project_manifest stored = must(find_manifest(ws->store())).manifest;
    CHECK(stored.writer_format == v(1, 3));
    CHECK(stored.min_reader_format == v(1, 0));
  }
  {
    auto ws = must(
        open_project(root(), open_mode::write, recovery_policy{}, profile(v(1, 0)), options(fs)));
    CHECK(ws->mode() == open_mode::read_only);
    const auto edit = commit_ops(*ws, creator(), {must(manifest_set_op(sample()))});
    REQUIRE_FALSE(edit.has_value());
    CHECK(edit.error().code == errc::permission_denied);
  }
  CHECK(
      must(open_project(root(), open_mode::write, recovery_policy{}, profile(v(1, 3)), options(fs)))
          ->mode() == open_mode::write);

  const std::filesystem::path strict = "/strict";
  {
    project_manifest m = sample();
    m.project = fixture_id(id_class::project, "manifest/strict");
    m.min_reader_format = v(1, 2);
    auto ws = must(create_project(strict, m, profile(v(1, 2)), creator(), options(fs)));
  }
  for (const open_mode mode : {open_mode::write, open_mode::read_only}) {
    const auto opened =
        open_project(strict, mode, recovery_policy{}, profile(v(1, 1)), options(fs));
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().code == errc::reader_too_old);
  }
  // No lock was left behind by the refusals.
  CHECK(
      must(open_project(strict, open_mode::write, recovery_policy{}, profile(v(1, 2)), options(fs)))
          ->mode() == open_mode::write);
}
