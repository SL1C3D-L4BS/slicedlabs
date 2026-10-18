#pragma once

// Helpers for the transaction-manager tests (WO-050): workspaces over the test schemas (with the
// domains claimed and a small actor directory) or over the patch generator's record types, test
// actors and origins, sample records, a snapshot-to-oracle conversion, a byte image of a store and
// a log backend over the faulty vfs that can be crashed and reopened.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "faulty_vfs.hpp"
#include "model_store.hpp"

#include "sl/core/error.hpp"
#include "sl/fabric/clock.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/log_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/provenance.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/workspace.hpp"

namespace sl::fabric::testing {

/// A human actor with no grants: writes content.* through subsystem "content" and asset.* through
/// "asset" (the owner subsystems).
[[nodiscard]] actor_id tx_editor() noexcept;
/// An agent with no grants: denied by default (ARCH1-R40).
[[nodiscard]] actor_id tx_agent() noexcept;
/// A second human actor (idempotency keys are per actor).
[[nodiscard]] actor_id tx_other() noexcept;

/// {actor, subsystem} with no provenance.
[[nodiscard]] tx_origin tx_origin_of(const actor_id &actor, std::string_view subsystem = "content");

/// Options over the test schemas (test_schemas.hpp): domains content.scene, content.mesh (owner
/// content) and asset.cooked (owner asset) claimed and bound, the three actors above, `clock` and a
/// deterministic id source seeded with `seed`.
[[nodiscard]] result<workspace_options> scene_options(std::shared_ptr<const clock> clock,
                                                      std::uint64_t seed = 1);
/// Options over the patch generator's types "gen.rec0".."gen.rec3": the schema, authority,
/// references and invariants validators are left out (generated bodies conform to no schema).
[[nodiscard]] result<workspace_options> gen_options(std::shared_ptr<const clock> clock,
                                                    std::uint64_t seed = 1);

/// A begin over `ref` by `origin`, staging `ops`, then commit.
[[nodiscard]] result<commit_receipt> commit_ops(transaction_manager &m, const tx_origin &origin,
                                                const std::vector<patch_op> &ops,
                                                patch_meta meta = {},
                                                std::string_view ref = "main");

/// Stable test ids: test_id(entity | asset | derived | element, name).
[[nodiscard]] record_id entity_named(std::string_view name) noexcept;
[[nodiscard]] record_id asset_named(std::string_view name) noexcept;
[[nodiscard]] record_id derived_named(std::string_view name) noexcept;

[[nodiscard]] type_id entity_type() noexcept;
[[nodiscard]] type_id scene_type() noexcept;
[[nodiscard]] type_id mesh_type() noexcept;
[[nodiscard]] type_id cooked_type() noexcept;

/// An Entity body (sample_entity) and the create op for it.
[[nodiscard]] result<patch_op> create_entity(std::string_view name);
/// A Scene owning `members` (an owned EntityList).
[[nodiscard]] result<patch_op> create_scene(const record_id &id, std::string_view name,
                                            const std::vector<record_id> &members);
/// A Mesh {source external_ref, vertex_count}.
[[nodiscard]] result<patch_op> create_mesh(std::string_view name, std::uint32_t vertices);
/// A CookedMesh (state class derived) of mesh `source`.
[[nodiscard]] result<patch_op> create_cooked(std::string_view name, const record_id &source);
/// set Entity.name (field 1).
[[nodiscard]] result<patch_op> set_name(const record_id &id, std::string_view name);
/// A complete F3 provenance record over `input`.
[[nodiscard]] provenance complete_provenance(const record_id &input);

/// Every live record of `s` as an oracle_store.
[[nodiscard]] result<oracle_store> oracle_of(const snapshot &s);

/// A patch creating every record of `state` (to load a generated state into a workspace).
[[nodiscard]] patch load_patch(const oracle_store &state);

/// Every file under `dir` of `fs` with its bytes (a byte image of a log store).
[[nodiscard]] result<std::map<std::string, std::vector<std::uint8_t>>>
file_image(vfs_memory &fs, const std::filesystem::path &dir);

/// A log backend over a faulty vfs over a vfs_memory, crashable and reopenable.
class tx_log_store {
public:
  tx_log_store();

  /// Opens (or reopens) the store; the previous backend object must be released first.
  [[nodiscard]] result<std::shared_ptr<store_backend>> open();
  [[nodiscard]] faulty_vfs &fs() noexcept { return *fs_; }
  [[nodiscard]] vfs_memory &memory() noexcept { return *fs_->memory(); }
  [[nodiscard]] const std::filesystem::path &dir() const noexcept { return dir_; }

private:
  std::unique_ptr<faulty_vfs> fs_;
  std::filesystem::path dir_;
  bool made_ = false;
};

} // namespace sl::fabric::testing
