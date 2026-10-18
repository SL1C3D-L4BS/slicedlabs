// FAB-INV-MAN-04 (SL-ARCH-001 ARCH1-R35, SL-ARCH-002 ARCH2-R28): project.slp accepts only format,
// store and project id. parse_locator() round-trips format_locator(); any other key (a project
// fact such as a name or a module list), a missing key, a non-string value, malformed JSON, a
// store path that is absolute or leaves the root, or a project id of another class is
// manifest_invalid naming the problem; another format string is format_unsupported.
// create_project() writes exactly the three keys and no fact of the manifest (on vfs_memory and on
// the posix vfs); open_project() refuses a locator naming another project (manifest_invalid), a
// store other than .slfabric (unsupported) and a missing locator (not_found).

#include "sl/core/error.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/manifest.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/vfs.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/crash_harness.hpp"

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

std::string read_text(vfs &fs, const std::filesystem::path &p) {
  const std::vector<std::uint8_t> b = must(read_file(fs, p));
  return {b.begin(), b.end()};
}

void write_text(vfs &fs, const std::filesystem::path &p, std::string_view text) {
  auto f = must(fs.open(p, open_create));
  const std::vector<std::uint8_t> b(text.begin(), text.end());
  REQUIRE(f.append(b).has_value());
}

std::string locator_text(std::string_view extra) {
  return std::string(R"({"format": "sl-project/1", "store": ".slfabric", "project": ")") +
         sample().project.to_string() + "\"" + std::string(extra) + "}";
}

void refused(std::string_view text, errc code, std::string_view mentions) {
  CAPTURE(text);
  const auto r = parse_locator(text);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == code);
  CHECK(r.error().message.find(mentions) != std::string::npos);
}

// create_project() writes a locator with exactly format, store and project, and no fact.
void created_locator(const std::shared_ptr<vfs> &fs, const std::filesystem::path &dir) {
  {
    auto ws = must(create_project(dir, sample(), profile(), creator(), options(fs)));
  }
  const std::string text = read_text(*fs, dir / locator_file_name);
  const json_value doc = must(parse_json(text));
  REQUIRE(doc.kind == json_value::kind_type::object);
  REQUIRE(doc.members.size() == 3);
  CHECK(doc.members[0].key == "format");
  CHECK(doc.members[1].key == "store");
  CHECK(doc.members[2].key == "project");
  CHECK(text.find(sample().name) == std::string::npos);
  CHECK(text.find("sl.render") == std::string::npos);
  CHECK(text.find("desktop-linux") == std::string::npos);
  const project_locator loc = must(read_locator(*fs, dir));
  CHECK(loc.format == locator_format);
  CHECK(loc.store == workspace::store_dir_name);
  CHECK(loc.project == sample().project);
  CHECK_FALSE(must(fs->exists(dir / (std::string(locator_file_name) + ".tmp"))));
  CHECK(must(open_project(dir, open_mode::write, recovery_policy{}, profile(), options(fs)))
            ->mode() == open_mode::write);
  // A second create over the project is refused.
  CHECK(create_project(dir, sample(), profile(), creator(), options(fs)).error().code ==
        errc::already_exists);
}

} // namespace

TEST_CASE("FAB-INV-MAN-04: project.slp accepts only format, store and project id (parse)") {
  const project_locator loc{.format = std::string(locator_format),
                            .store = "data/.slfabric",
                            .project = sample().project};
  CHECK(must(parse_locator(format_locator(loc))) == loc);
  CHECK(must(parse_locator(locator_text(""))).store == ".slfabric");

  refused(locator_text(R"(, "name": "Valley")"), errc::manifest_invalid, "key 'name'");
  refused(locator_text(R"(, "modules": [])"), errc::manifest_invalid, "key 'modules'");
  refused(R"({"format": "sl-project/1", "store": ".slfabric"})", errc::manifest_invalid,
          "key 'project' is missing");
  refused(R"({"format": "sl-project/1", "store": 7, "project": "x"})", errc::manifest_invalid,
          "key 'store' is not a string");
  refused(R"({"format": "sl-project/2", "store": ".slfabric", "project": ")" +
              sample().project.to_string() + "\"}",
          errc::format_unsupported, "sl-project/2");
  refused(R"({"format": "sl-project/1", "store": "/abs", "project": ")" +
              sample().project.to_string() + "\"}",
          errc::manifest_invalid, "not relative");
  refused(R"({"format": "sl-project/1", "store": "../up", "project": ")" +
              sample().project.to_string() + "\"}",
          errc::manifest_invalid, "leaves the project root");
  refused(R"({"format": "sl-project/1", "store": ".slfabric", "project": ")" +
              fixture_id(id_class::entity, "x").to_string() + "\"}",
          errc::manifest_invalid, "not a project id");
  refused("[1, 2]", errc::manifest_invalid, "not a JSON object");
  refused("format = sl-project/1", errc::manifest_invalid, "not JSON");
  refused(R"({"format": "sl-project/1", "format": "sl-project/1"})", errc::manifest_invalid,
          "not JSON");
}

TEST_CASE("FAB-INV-MAN-04: project.slp accepts only format, store and project id (memory vfs)") {
  const auto fs = std::make_shared<vfs_memory>();
  created_locator(fs, root());

  // A locator naming another project, another store, or none at all.
  const std::filesystem::path file = root() / locator_file_name;
  const std::string original = read_text(*fs, file);
  const record_id other = fixture_id(id_class::project, "manifest/other");
  write_text(*fs, file,
             format_locator(project_locator{.format = std::string(locator_format),
                                            .store = std::string(workspace::store_dir_name),
                                            .project = other}));
  auto r = open_project(root(), open_mode::write, recovery_policy{}, profile(), options(fs));
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == errc::manifest_invalid);
  CHECK(r.error().message.find(other.to_string()) != std::string::npos);

  write_text(*fs, file,
             format_locator(project_locator{.format = std::string(locator_format),
                                            .store = "elsewhere",
                                            .project = sample().project}));
  r = open_project(root(), open_mode::write, recovery_policy{}, profile(), options(fs));
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == errc::unsupported);

  write_text(*fs, file, locator_text(R"(, "default_branch": "main")"));
  r = open_project(root(), open_mode::read_only, recovery_policy{}, profile(), options(fs));
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == errc::manifest_invalid);

  must_ok(fs->remove(file));
  r = open_project(root(), open_mode::write, recovery_policy{}, profile(), options(fs));
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == errc::not_found);

  write_text(*fs, file, original);
  CHECK(must(open_project(root(), open_mode::write, recovery_policy{}, profile(), options(fs)))
            ->mode() == open_mode::write);

  // An interrupted create resumes: a store holding this project's manifest but no locator gets
  // its locator; another project's store is refused; a store left before its genesis gets it.
  must_ok(fs->remove(file));
  project_manifest other_project = sample();
  other_project.project = other;
  CHECK(create_project(root(), other_project, profile(), creator(), options(fs)).error().code ==
        errc::already_exists);
  CHECK_FALSE(must(fs->exists(file)));
  {
    auto ws = must(create_project(root(), sample(), profile(), creator(), options(fs)));
  }
  CHECK(read_text(*fs, file) == original);

  const std::filesystem::path bare = "/bare";
  {
    auto ws = must(workspace::create(bare, options(fs)));
  }
  {
    auto ws = must(create_project(bare, sample(), profile(), creator(), options(fs)));
    CHECK(must(find_manifest(ws->store())).manifest.project == sample().project);
  }
  CHECK(must(read_locator(*fs, bare)).project == sample().project);
}

TEST_CASE("FAB-INV-MAN-04: project.slp accepts only format, store and project id (posix vfs)") {
  sl::fabric::testing::scratch_dir scratch("fabric_manifest_locator");
  REQUIRE(scratch.ok());
  created_locator(std::make_shared<vfs_posix>(), scratch.path() / "proj");
}
