#pragma once

// The project manifest and the project locator (WO-054, SL-ARCH-001 ARCH1-R32 to ARCH1-R35,
// SL-ARCH-002 ARCH2-R28; ADR-0011, ADR-0016).
//
// The manifest is one record of the generated type fabric.manifest.manifest (WO-045) at the
// well-known id (entity, ordinal 1), domain fabric.manifest, owner 02 (ARCH1-R32). It is written
// only by transactions (ARCH1-R33): create_project() commits it as the genesis transaction through
// the manager, asserted by the creating actor, and every later edit is an ordinary transaction
// that the manifest validator ("fabric.manifest", stage invariants) judges:
//   R-MAN-ONE      a manifest record at any other id                       manifest_invalid
//   R-MAN-TYPE     a record of another type at the manifest id             manifest_invalid
//   R-MAN-MISSING  the manifest deleted                                    manifest_missing
//   R-MAN-SHAPE    a body that does not decode, or fails validate_manifest manifest_invalid
//   R-MAN-PIN      two package pins with one package id or one mount       manifest_invalid
//   R-MAN-PROJECT  the project id changed                                  manifest_invalid
// Every diagnostic message starts with the error code name.
//
// check_open() is the open-time gate (ARCH1-R34), judged against an engine_profile the caller
// injects (format version, modules present, the capability catalog of SL-REG-004 ids and the
// SL-GOV-007 target profile names; nothing is read from the registries at run time). In order:
//   reader_too_old      the minimum reader format is above the engine's format
//   manifest_invalid    validate_manifest() fails, or a target profile is not in the profile
//   module_unmet        a required module is absent or its version is outside the range
//   capability_unknown  a required capability is not in the catalog
// Each message names what was required and what was found, listing every offending item of the
// class that failed. Success is open_mode::write, or open_mode::read_only when the writer format
// is newer than the engine (a newer minor: the project opens, but this engine may not write it).
// Version ranges are major.minor, at_least inclusive, below exclusive.
//
// project.slp (ARCH1-R35) is a locator, never a source of facts: a JSON object with exactly the
// keys "format" ("sl-project/1"), "store" (the store directory relative to the project root) and
// "project" (the project id text). Any other key, a missing or non-string one is
// manifest_invalid; another format string is format_unsupported.
//
// create_project() and open_project() (src/workspace_project.cpp) wrap workspace::create and
// workspace::open (WO-055) with the manifest:
//   create_project  check_open() on the requested manifest first (nothing is created on a
//                   refusal), then workspace::create, the genesis transaction on the manifest's
//                   default branch, and project.slp last (written atomically). A store left
//                   without a manifest by an interrupted create (no refs) is resumed; a store
//                   holding the same project's manifest but no locator gets its locator.
//   open_project    reads project.slp, then the manifest from an inspecting (lock-free,
//                   repair-free) look at the store, then check_open(): a refusal opens nothing
//                   (no lock taken, no recovery run). Only then workspace::open runs in the mode
//                   asked for (read_only when check_open says so). A log that needs a repair
//                   before it can be read is repaired by the write open first and judged after;
//                   a refusal then closes the workspace before returning.
// Both register the manifest types, claim fabric.manifest for subsystem 02 and bind the types
// when the caller has not (prepare_manifest_options), and append the manifest validator.

#include <compare>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>

#include "sl/core/error.hpp"
#include "sl/fabric/gen/fabric.manifest.gen.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/log_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/recovery.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/vfs.hpp"
#include "sl/fabric/workspace.hpp"

namespace sl::fabric {

using project_manifest = gen::fabric_manifest::manifest;
using format_version = gen::fabric_manifest::format_version;
using version_range = gen::fabric_manifest::version_range;
using package_pin = gen::fabric_manifest::package_pin;

inline constexpr std::string_view manifest_type_name = "fabric.manifest.manifest";
inline constexpr std::string_view manifest_domain = "fabric.manifest";
inline constexpr std::string_view manifest_validator_name = "fabric.manifest";

/// The project format this engine writes and reads.
inline constexpr format_version engine_format{.major = 1, .minor = 0};

/// well_known(entity, 1) (ADR-0011).
[[nodiscard]] record_id manifest_id() noexcept;
/// type_id_of(manifest_type_name).
[[nodiscard]] type_id manifest_type() noexcept;

/// Lexicographic (major, minor).
[[nodiscard]] std::strong_ordering compare(const format_version &a,
                                           const format_version &b) noexcept;
/// at_least <= v < below.
[[nodiscard]] bool in_range(const format_version &v, const version_range &r) noexcept;
/// "1.2".
[[nodiscard]] std::string to_string(const format_version &v);
/// "[1.2, 2.0)".
[[nodiscard]] std::string to_string(const version_range &r);

/// What the opening engine offers (injected; the catalogs are not read at run time).
struct engine_profile {
  format_version format = engine_format;
  /// Modules present, by id, with their versions.
  std::map<std::string, format_version, std::less<>> modules;
  /// The capability catalog: SL-REG-004 ids.
  std::set<std::string, std::less<>> capabilities;
  /// SL-GOV-007 target profile names.
  std::set<std::string, std::less<>> target_profiles;

  friend bool operator==(const engine_profile &, const engine_profile &) = default;
};

/// The shape rules, with no profile (`manifest_invalid` naming the offending item): a project id
/// of class project, a non-empty name, minimum reader <= writer, non-empty module ids with
/// non-empty ranges, capabilities and target profiles non-empty and unique, package pins with a
/// package id of class package, a non-zero revision, a non-empty mount, unique by package id and
/// by mount, and a default branch that is a valid ref name (1 to 255 bytes of 0x21..0x7E).
[[nodiscard]] result<void> validate_manifest(const project_manifest &m);

/// The open-time gate (file comment).
[[nodiscard]] result<open_mode> check_open(const project_manifest &m, const engine_profile &p);

/// The manifest of `s`: `manifest_missing` when no live record is at manifest_id(),
/// `manifest_invalid` when it has another type, does not decode or another document root is a
/// manifest too (O(documents) reads), the store's errors otherwise.
[[nodiscard]] result<project_manifest> read_manifest(const snapshot &s);

/// The manifest of a store and the ref it was read from: the head of the first ref (by name)
/// holding one, re-read from the default branch it names when that ref exists.
/// `manifest_missing` when no ref holds one.
struct located_manifest {
  std::string ref;
  project_manifest manifest;
};
[[nodiscard]] result<located_manifest> find_manifest(const store_backend &store);

/// A set op replacing the whole manifest body with `m` (an edit for a transaction).
[[nodiscard]] result<patch_op> manifest_set_op(const project_manifest &m);

/// The manifest validator (file comment).
[[nodiscard]] std::shared_ptr<const validator> make_manifest_validator();

/// Registers the fabric.manifest types, claims fabric.manifest for subsystem 02 and binds the
/// types, each only when not done yet; `registry_frozen` when a step is needed and the registry
/// is frozen.
[[nodiscard]] result<void> prepare_manifest_options(workspace_options &options);

// ---- project.slp ----------------------------------------------------------------------------

inline constexpr std::string_view locator_format = "sl-project/1";
inline constexpr std::string_view locator_file_name = "project.slp";

struct project_locator {
  std::string format{locator_format};
  std::string store{workspace::store_dir_name};
  record_id project;

  friend bool operator==(const project_locator &, const project_locator &) = default;
};

/// The locator text (file comment); the store must be a relative path without "..", the project
/// id of class project.
[[nodiscard]] result<project_locator> parse_locator(std::string_view text);
/// Canonical text: {"format": ..., "store": ..., "project": ...} and a newline.
[[nodiscard]] std::string format_locator(const project_locator &l);
/// root/project.slp: `not_found` when absent, parse_locator's errors.
[[nodiscard]] result<project_locator> read_locator(vfs &fs, const std::filesystem::path &root);
/// Writes root/project.slp atomically (a temporary file, fsync, rename, directory sync; no syncs
/// under durability::none).
[[nodiscard]] result<void> write_locator(vfs &fs, const std::filesystem::path &root,
                                         const project_locator &l, durability sync);

// ---- projects (src/workspace_project.cpp) ---------------------------------------------------

/// A new project in `root` (file comment). The manifest's writer format becomes p.format, a zero
/// minimum reader becomes {p.format.major, 0}, a zero schema_set the digest of the registered
/// schemas and a null project id a minted one. `already_exists` when root holds a project or
/// another project's store; check_open's errors; the genesis commit's errors.
[[nodiscard]] result<std::unique_ptr<workspace>>
create_project(const std::filesystem::path &root, project_manifest manifest,
               const engine_profile &p, const actor_id &creator, workspace_options options);

/// Opens the project in `root` (file comment): read_locator's errors, the manifest errors of
/// find_manifest and check_open, `manifest_invalid` when the locator's project id is not the
/// manifest's, `unsupported` for a locator store other than workspace::store_dir_name, and
/// workspace::open's errors.
[[nodiscard]] result<std::unique_ptr<workspace>>
open_project(const std::filesystem::path &root, open_mode mode, const recovery_policy &policy,
             const engine_profile &p, workspace_options options);

} // namespace sl::fabric
