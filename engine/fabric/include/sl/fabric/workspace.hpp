#pragma once

// The workspace: the registries, the store and the transaction manager of one project (WO-050,
// WO-055; SL-ARCH-002 ARCH2-R11, ARCH2-R18, ARCH2-R28, ARCH2-R34 and ARCH2-R35; ADR-0015,
// ADR-0016).
//
// create_memory() builds a workspace over a fresh memory_backend; create_with() attaches a
// workspace to a store the caller opened (a log_backend over a vfs, or the same memory_backend
// again, which is how a workspace is "reopened" after an indeterminate commit). Every way in
// freezes the schema and domain registries before anything else (ARCH2-R18): a type or domain
// claim after the workspace opens is refused by the registries themselves. Clock and id source
// are injected (ARCH2-R11); the defaults are the system clock and the operating system's entropy.
//
// The durable workspace (WO-055) lives in a directory `root` whose `.slfabric/` holds the log
// store (log_backend.hpp) through options.fs (default vfs_posix):
//   create(root, options)        makes root (its parent must exist) and `.slfabric/`, takes the
//                                writer lock and creates the store; `already_exists` when a store
//                                is there. A creation that stopped before FORMAT is redone.
//   open(root, mode, policy, o)  write: R1 takes the OFD writer lock on `.slfabric/lock`
//                                (`writer_locked` while another open holds it; a lock file left
//                                with a holder's text but locked by no one is stale: it is taken
//                                over and reported as C11), then recover_log() runs R2-R10
//                                (recovery.hpp). A blocking finding refuses the open with
//                                `journal_corrupt` naming the findings (the WO-059 policies may
//                                open a read-only recovery view instead). read_only: no lock and
//                                no repair; a log that needs one is `recovery_required`, and every
//                                commit is `permission_denied`. `not_found` when root holds no
//                                store. When the store holds a project manifest, its schema_set
//                                must be the schema_set of a revision on the first-parent history
//                                of the ref it was read from (the genesis set until a transaction
//                                changes it), else `manifest_invalid` and nothing opens (WO-064).
// last_recovery() is the report of the open (empty after create). Closing a writer clears the
// holder text and releases the lock; a crashed writer leaves the text behind (the next writer
// reports it). The project locator `project.slp` is WO-054's.
//
// The workspace owns the registries and the manager; the store is shared so that a test can keep
// it across a reopen. Readers get the store only as const: every write goes through the manager.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/authority.hpp"
#include "sl/fabric/clock.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/events.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/log_backend.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/recovery.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/vfs.hpp"

namespace sl::fabric {

struct workspace_options {
  /// Frozen by the workspace when it opens (types added before).
  schema_registry schemas;
  /// Frozen by the workspace when it opens; bind_types() is the caller's (claims first).
  domain_registry domains;
  actor_directory actors;
  /// Null: system_clock_source.
  std::shared_ptr<const fabric::clock> clock;
  /// Null: os_entropy_source (after its self test).
  std::shared_ptr<id_source> ids;
  /// Appended after the built-in validators, then the registry freezes.
  std::vector<std::shared_ptr<const validator>> validators;
  /// Built-in validators to leave out, by name (builtin_validators()).
  std::vector<std::string> omit_builtin;
  std::vector<std::pair<type_id, std::shared_ptr<const merge_driver>>> merge_drivers;
  /// Event subscriptions made before the manager opens (they see its recovered events).
  std::vector<std::pair<std::string, event_handler>> subscribers;
  /// The file system of create() and open(); null: a vfs_posix.
  std::shared_ptr<vfs> fs;
  /// The log store of create() and open(): durability, rotation and the loose threshold.
  log_options log;
};

/// How open() opens a durable workspace.
enum class open_mode : std::uint8_t { write = 1, read_only = 2 };

class workspace {
  struct open_key {
    explicit open_key() = default;
  };

public:
  /// A workspace over a new memory_backend. The registries' freeze errors, the manager's errors
  /// (transaction_manager::create) and `entropy_failure` for a failing default id source.
  [[nodiscard]] static result<std::unique_ptr<workspace>> create_memory(workspace_options options);
  /// The same over `store` (not null: `invalid_argument`), which may already hold commits.
  /// open_mode::read_only refuses every commit with `permission_denied` and mode() reports it
  /// (a lenient recovered view, corruption.hpp).
  [[nodiscard]] static result<std::unique_ptr<workspace>>
  create_with(std::shared_ptr<store_backend> store, workspace_options options,
              open_mode mode = open_mode::write);
  /// A new durable workspace in directory `root` (file comment).
  [[nodiscard]] static result<std::unique_ptr<workspace>> create(const std::filesystem::path &root,
                                                                 workspace_options options);
  /// Opens the durable workspace in `root` with recovery (file comment).
  [[nodiscard]] static result<std::unique_ptr<workspace>> open(const std::filesystem::path &root,
                                                               open_mode mode,
                                                               const recovery_policy &policy,
                                                               workspace_options options);
  /// The same with default options (no types, domains or actors registered).
  [[nodiscard]] static result<std::unique_ptr<workspace>>
  open(const std::filesystem::path &root, open_mode mode, const recovery_policy &policy);

  /// The store directory inside a workspace directory.
  static constexpr std::string_view store_dir_name = ".slfabric";

  workspace(open_key key, std::shared_ptr<store_backend> store,
            workspace_options &options) noexcept;
  workspace(const workspace &) = delete;
  workspace(workspace &&) = delete;
  workspace &operator=(const workspace &) = delete;
  workspace &operator=(workspace &&) = delete;
  ~workspace();

  /// The only way to change the project.
  [[nodiscard]] transaction_manager &manager() noexcept { return *manager_; }
  [[nodiscard]] const transaction_manager &manager() const noexcept { return *manager_; }
  /// The revision `revision`, read-only.
  [[nodiscard]] result<snapshot> at(const digest &revision) const;
  [[nodiscard]] const store_backend &store() const noexcept { return *store_; }
  [[nodiscard]] const schema_registry &schemas() const noexcept { return schemas_; }
  [[nodiscard]] const domain_registry &domains() const noexcept { return domains_; }
  [[nodiscard]] const actor_directory &actors() const noexcept { return actors_; }
  /// The report of the recovery open() ran (empty for other workspaces).
  [[nodiscard]] const recovery_report &last_recovery() const noexcept { return recovery_; }
  /// read_only for a workspace opened or created read_only (every commit is then
  /// `permission_denied`), write otherwise.
  [[nodiscard]] open_mode mode() const noexcept { return mode_; }

private:
  // Freezes the registries and builds the manager over store_ (create_with and open); a
  // read_only workspace wraps the store so that every commit is refused.
  [[nodiscard]] static result<std::unique_ptr<workspace>>
  assemble(std::shared_ptr<store_backend> store, workspace_options &options,
           std::shared_ptr<vfs> fs, vfs_file lock, recovery_report report, open_mode mode);

  // Declared first: the file system outlives the lock file and the store.
  std::shared_ptr<vfs> fs_;
  vfs_file lock_;
  recovery_report recovery_;
  open_mode mode_ = open_mode::write;
  std::shared_ptr<store_backend> store_;
  schema_registry schemas_;
  domain_registry domains_;
  actor_directory actors_;
  std::unique_ptr<transaction_manager> manager_;
};

} // namespace sl::fabric
