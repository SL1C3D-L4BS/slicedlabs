#pragma once

// The workspace: the registries, the store and the transaction manager of one project (WO-050,
// SL-ARCH-002 ARCH2-R18, ARCH2-R11; ADR-0016).
//
// create_memory() builds a workspace over a fresh memory_backend; create_with() attaches a
// workspace to a store the caller opened (a log_backend over a vfs, or the same memory_backend
// again, which is how a workspace is "reopened" after an indeterminate commit). Both freeze the
// schema and domain registries before anything else (ARCH2-R18): a type or domain claim after the
// workspace opens is refused by the registries themselves. Clock and id source are injected
// (ARCH2-R11); the defaults are the system clock and the operating system's entropy. The durable
// open with recovery (open(path, mode, policy)) is WO-055.
//
// The workspace owns the registries and the manager; the store is shared so that a test can keep
// it across a reopen. Readers get the store only as const: every write goes through the manager.

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/authority.hpp"
#include "sl/fabric/clock.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/events.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/ownership.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/validate.hpp"

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
};

class workspace {
  struct open_key {
    explicit open_key() = default;
  };

public:
  /// A workspace over a new memory_backend. The registries' freeze errors, the manager's errors
  /// (transaction_manager::create) and `entropy_failure` for a failing default id source.
  [[nodiscard]] static result<std::unique_ptr<workspace>> create_memory(workspace_options options);
  /// The same over `store` (not null: `invalid_argument`), which may already hold commits.
  [[nodiscard]] static result<std::unique_ptr<workspace>>
  create_with(std::shared_ptr<store_backend> store, workspace_options options);

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

private:
  std::shared_ptr<store_backend> store_;
  schema_registry schemas_;
  domain_registry domains_;
  actor_directory actors_;
  std::unique_ptr<transaction_manager> manager_;
};

} // namespace sl::fabric
