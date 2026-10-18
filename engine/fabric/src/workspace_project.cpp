// Projects: the workspace with its manifest and locator (WO-054, SL-ARCH-001 ARCH1-R32 to
// ARCH1-R35, SL-ARCH-002 ARCH2-R28). create_project() commits the manifest as the genesis
// transaction through the manager and writes project.slp last; open_project() judges the manifest
// with check_open() before the workspace opens (manifest.hpp).

#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "sl/core/error.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/log_backend.hpp"
#include "sl/fabric/manifest.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/provenance.hpp"
#include "sl/fabric/recovery.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/vfs.hpp"
#include "sl/fabric/workspace.hpp"

namespace sl::fabric {

namespace {

namespace stdfs = std::filesystem;

template <class T> std::unexpected<error> pass(const result<T> &r) {
  return std::unexpected(r.error());
}

std::shared_ptr<vfs> file_system(workspace_options &options) {
  if (!options.fs) {
    options.fs = std::make_shared<vfs_posix>();
  }
  return options.fs;
}

// The locator and the manifest name the same project, and the manifest admits this engine.
result<open_mode> judge(const project_manifest &m, const project_locator &loc,
                        const engine_profile &p) {
  if (m.project != loc.project) {
    return make_error(errc::manifest_invalid,
                      std::format("{} names project {}; the manifest is project {}",
                                  locator_file_name, loc.project.to_string(),
                                  m.project.to_string()));
  }
  return check_open(m, p);
}

result<record_id> mint_project(const workspace_options &options) {
  if (options.ids) {
    return mint(id_class::project, *options.ids);
  }
  auto os = os_entropy_source::create();
  if (!os) {
    return pass(os);
  }
  return mint(id_class::project, *os);
}

// The genesis transaction: the manifest created by `creator` through subsystem 02, asserted.
result<void> commit_genesis(workspace &ws, const project_manifest &m, const actor_id &creator) {
  auto body = m.to_value();
  if (!body) {
    return pass(body);
  }
  tx_options o;
  o.origin.actor = creator;
  o.origin.subsystem = "02";
  o.origin.epistemic = epistemic_class::asserted;
  o.ref = m.default_branch;
  o.meta.label = "project genesis: manifest";
  auto t = ws.manager().begin(std::move(o));
  if (!t) {
    return pass(t);
  }
  if (auto ok = t->stage(op_create(manifest_id(), manifest_type(), std::move(*body))); !ok) {
    return ok;
  }
  auto receipt = t->commit();
  if (!receipt) {
    std::string why = receipt.error().message;
    for (const diagnostic &d : t->diagnostics()) {
      why += "; " + d.rule + " " + d.message;
    }
    return make_error(receipt.error().code, "project genesis: " + why);
  }
  return {};
}

} // namespace

result<std::unique_ptr<workspace>> create_project(const std::filesystem::path &root,
                                                  project_manifest manifest,
                                                  const engine_profile &p, const actor_id &creator,
                                                  workspace_options options) {
  const std::shared_ptr<vfs> fs = file_system(options);
  const durability sync = options.log.sync;
  if (auto ok = prepare_manifest_options(options); !ok) {
    return pass(ok);
  }
  if (manifest.project.is_null()) {
    auto id = mint_project(options);
    if (!id) {
      return pass(id);
    }
    manifest.project = *id;
  }
  manifest.writer_format = p.format;
  if (manifest.min_reader_format == format_version{}) {
    manifest.min_reader_format = format_version{.major = p.format.major, .minor = 0};
  }
  if (manifest.schema_set == digest{}) {
    auto set = schema_set_digest(options.schemas);
    if (!set) {
      return pass(set);
    }
    manifest.schema_set = *set;
  }
  // ARCH1-R34 at create: a project this engine could not open is never made.
  if (auto ok = check_open(manifest, p); !ok) {
    return pass(ok);
  }
  const result<bool> located = fs->exists(root / locator_file_name);
  if (!located) {
    return pass(located);
  }
  if (*located) {
    return make_error(errc::already_exists,
                      (root / locator_file_name).string() + " already locates a project");
  }
  const result<bool> stored = fs->exists(root / workspace::store_dir_name / "FORMAT");
  if (!stored) {
    return pass(stored);
  }
  options.validators.push_back(make_manifest_validator());
  auto ws = *stored ? workspace::open(root, open_mode::write, recovery_policy{}, std::move(options))
                    : workspace::create(root, std::move(options));
  if (!ws) {
    return pass(ws);
  }
  if ((*ws)->manager().refs().empty()) {
    // A new store, or one an interrupted create left without its genesis.
    if (auto ok = commit_genesis(**ws, manifest, creator); !ok) {
      return pass(ok);
    }
  } else {
    // A store with history: only the same project's, interrupted before its locator, resumes.
    auto found = find_manifest((*ws)->store());
    if (!found || found->manifest.project != manifest.project) {
      return make_error(errc::already_exists, (root / workspace::store_dir_name).string() +
                                                  " already holds another project's store");
    }
    if (auto ok = check_open(found->manifest, p); !ok) {
      return pass(ok);
    }
  }
  const project_locator loc{.format = std::string(locator_format),
                            .store = std::string(workspace::store_dir_name),
                            .project = manifest.project};
  if (auto ok = write_locator(*fs, root, loc, sync); !ok) {
    return pass(ok);
  }
  return std::move(*ws);
}

result<std::unique_ptr<workspace>> open_project(const std::filesystem::path &root, open_mode mode,
                                                const recovery_policy &policy,
                                                const engine_profile &p,
                                                workspace_options options) {
  const std::shared_ptr<vfs> fs = file_system(options);
  auto loc = read_locator(*fs, root);
  if (!loc) {
    return pass(loc);
  }
  if (loc->store != workspace::store_dir_name) {
    return make_error(errc::unsupported,
                      std::format("{}: store '{}'; this engine opens only '{}'", locator_file_name,
                                  loc->store, workspace::store_dir_name));
  }
  if (auto ok = prepare_manifest_options(options); !ok) {
    return pass(ok);
  }
  options.validators.push_back(make_manifest_validator());

  // Read the manifest before anything opens: an inspecting look takes no lock and repairs
  // nothing, so a refusal leaves the project untouched.
  bool judged = false;
  open_mode effective = mode;
  {
    auto probe = recover_log(*fs, root / workspace::store_dir_name, policy, options.log,
                             recovery_mode::inspect);
    if (probe && probe->store && !probe->report.blocking()) {
      auto found = find_manifest(*probe->store);
      probe->store.reset();
      if (!found) {
        return pass(found);
      }
      auto verdict = judge(found->manifest, *loc, p);
      if (!verdict) {
        return pass(verdict);
      }
      if (*verdict == open_mode::read_only) {
        effective = open_mode::read_only;
      }
      judged = true;
    }
  }
  auto ws = workspace::open(root, effective, policy, std::move(options));
  if (!ws) {
    return pass(ws);
  }
  if (!judged) {
    // The log needed a repair before it could be read: judged after the write open repaired it;
    // a refusal closes the workspace here.
    auto found = find_manifest((*ws)->store());
    if (!found) {
      return pass(found);
    }
    auto verdict = judge(found->manifest, *loc, p);
    if (!verdict) {
      return pass(verdict);
    }
    if (*verdict == open_mode::read_only && effective == open_mode::write) {
      return make_error(errc::read_only_newer,
                        std::format("manifest: writer format {} is newer than this engine's {}; "
                                    "open the project read-only",
                                    to_string(found->manifest.writer_format), to_string(p.format)));
    }
  }
  return std::move(*ws);
}

} // namespace sl::fabric
