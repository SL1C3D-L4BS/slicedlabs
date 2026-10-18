// FAB-INV-MAN-02 (SL-ARCH-001 ARCH1-R34): an unmet module, capability or target profile is a
// typed refusal and nothing opens. check_open() against an injected engine_profile: a required
// module absent or outside [at_least, below) is module_unmet naming the module, the range required
// and the version found (bounds: at_least inclusive, below exclusive); a capability outside the
// catalog is capability_unknown; a target profile outside the SL-GOV-007 names is manifest_invalid;
// every offending item of the failing class is named. At the workspace: create_project() with an
// unmet requirement creates nothing, and open_project() refusing leaves the store byte for byte
// as it was, takes no lock (a writer opens right after) and runs no recovery.

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
#include <map>
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

bool names(const sl::error &e, std::string_view text) { return e.message.contains(text); }

// Every file under `dir` with its bytes.
std::map<std::string, std::vector<std::uint8_t>> image(vfs_memory &fs,
                                                       const std::filesystem::path &dir) {
  std::map<std::string, std::vector<std::uint8_t>> out;
  for (const std::string &name : must(fs.list(dir))) {
    const std::filesystem::path p = dir / name;
    auto bytes = fs.contents(p);
    if (bytes) {
      out.emplace(p.string(), std::move(*bytes));
    } else {
      out.merge(image(fs, p));
    }
  }
  return out;
}

} // namespace

TEST_CASE("FAB-INV-MAN-02: unmet module, capability or target profile is a typed refusal "
          "(check_open)") {
  CHECK(must(check_open(sample(), profile())) == open_mode::write);

  SUBCASE("module absent") {
    project_manifest m = sample();
    m.modules.emplace("sl.audio", range(1, 0, 2, 0));
    m.modules.emplace("sl.net", range(1, 0, 2, 0));
    const auto r = check_open(m, profile());
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == errc::module_unmet);
    CHECK(names(r.error(), "module 'sl.audio' required [1.0, 2.0), found none"));
    CHECK(names(r.error(), "module 'sl.net' required [1.0, 2.0), found none"));
  }
  SUBCASE("module version outside the range") {
    project_manifest m = sample();
    m.modules["sl.render"] = range(2, 4, 3, 0); // engine has 2.3
    auto r = check_open(m, profile());
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == errc::module_unmet);
    CHECK(names(r.error(), "module 'sl.render' required [2.4, 3.0), found 2.3"));
    // at_least is inclusive, below exclusive.
    m.modules["sl.render"] = range(2, 3, 2, 4);
    CHECK(must(check_open(m, profile())) == open_mode::write);
    m.modules["sl.render"] = range(2, 0, 2, 3);
    r = check_open(m, profile());
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == errc::module_unmet);
    CHECK(names(r.error(), "found 2.3"));
    // An empty range is a malformed manifest, not an unmet module.
    m.modules["sl.render"] = range(2, 3, 2, 3);
    r = check_open(m, profile());
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == errc::manifest_invalid);
  }
  SUBCASE("capability outside the catalog") {
    project_manifest m = sample();
    m.capabilities.emplace_back("cap.xr.passthrough");
    const auto r = check_open(m, profile());
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == errc::capability_unknown);
    CHECK(names(r.error(), "'cap.xr.passthrough'"));
    CHECK(names(r.error(), "found 2 capabilities"));
    m.capabilities.emplace_back("cap.gpu.compute");
    CHECK(check_open(m, profile()).error().code == errc::manifest_invalid); // listed twice
  }
  SUBCASE("target profile unknown") {
    project_manifest m = sample();
    m.target_profiles.emplace_back("console-x");
    const auto r = check_open(m, profile());
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == errc::manifest_invalid);
    CHECK(names(r.error(), "target profile 'console-x' required"));
  }
}

TEST_CASE("FAB-INV-MAN-02: unmet module, capability or target profile is a typed refusal and "
          "nothing opens") {
  const auto fs = std::make_shared<vfs_memory>();

  // create: nothing is made for a project this engine could not open.
  project_manifest needs = sample();
  needs.capabilities.emplace_back("cap.xr.passthrough");
  const auto refused = create_project(root(), needs, profile(), creator(), options(fs));
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().code == errc::capability_unknown);
  CHECK_FALSE(must(fs->exists(root())));

  {
    auto ws = must(create_project(root(), sample(), profile(), creator(), options(fs)));
  }
  const auto before = image(*fs, root());
  REQUIRE_FALSE(before.empty());

  engine_profile without_module = profile();
  without_module.modules.erase("sl.render");
  engine_profile old_module = profile();
  old_module.modules["sl.render"] = {.major = 1, .minor = 9};
  engine_profile no_caps = profile();
  no_caps.capabilities.clear();
  engine_profile no_targets = profile();
  no_targets.target_profiles = {"desktop-windows"};
  const std::vector<std::pair<engine_profile, errc>> cases = {
      {without_module, errc::module_unmet},
      {old_module, errc::module_unmet},
      {no_caps, errc::capability_unknown},
      {no_targets, errc::manifest_invalid},
  };
  for (const auto &[p, code] : cases) {
    for (const open_mode mode : {open_mode::write, open_mode::read_only}) {
      const auto opened = open_project(root(), mode, recovery_policy{}, p, options(fs));
      REQUIRE_FALSE(opened.has_value());
      CHECK(opened.error().code == code);
    }
  }
  // Nothing opened: the files are as they were and no lock is held.
  CHECK(image(*fs, root()) == before);
  auto writer =
      must(open_project(root(), open_mode::write, recovery_policy{}, profile(), options(fs)));
  CHECK(writer->mode() == open_mode::write);
}
