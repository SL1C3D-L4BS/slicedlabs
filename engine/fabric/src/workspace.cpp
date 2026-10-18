// The workspace (WO-050, SL-ARCH-002 ARCH2-R11 and ARCH2-R18, ADR-0016): registries frozen when
// it opens, the store, and the transaction manager.

#include <memory>
#include <utility>

#include "sl/core/error.hpp"
#include "sl/fabric/clock.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/workspace.hpp"

namespace sl::fabric {

workspace::workspace(open_key /*key*/, std::shared_ptr<store_backend> store,
                     workspace_options &options) noexcept
    : store_(std::move(store)), schemas_(std::move(options.schemas)),
      domains_(std::move(options.domains)), actors_(std::move(options.actors)) {}

workspace::~workspace() = default;

result<std::unique_ptr<workspace>> workspace::create_memory(workspace_options options) {
  return create_with(std::make_shared<memory_backend>(), std::move(options));
}

result<std::unique_ptr<workspace>> workspace::create_with(std::shared_ptr<store_backend> store,
                                                          workspace_options options) {
  if (!store) {
    return make_error(errc::invalid_argument, "a workspace needs a store");
  }
  std::shared_ptr<const fabric::clock> time = options.clock;
  if (!time) {
    time = std::make_shared<system_clock_source>();
  }
  std::shared_ptr<id_source> ids = options.ids;
  if (!ids) {
    auto os = os_entropy_source::create();
    if (!os) {
      return std::unexpected(os.error());
    }
    ids = std::make_shared<os_entropy_source>(std::move(*os));
  }
  // ARCH2-R18: the registries freeze before the workspace can be used.
  if (auto ok = options.schemas.freeze(); !ok) {
    return std::unexpected(ok.error());
  }
  if (auto ok = options.domains.freeze(); !ok) {
    return std::unexpected(ok.error());
  }
  auto ws = std::make_unique<workspace>(open_key{}, std::move(store), options);
  manager_config config;
  config.store = ws->store_.get();
  config.schemas = &ws->schemas_;
  config.domains = &ws->domains_;
  config.actors = &ws->actors_;
  config.clock = std::move(time);
  config.ids = std::move(ids);
  config.validators = std::move(options.validators);
  config.omit_builtin = std::move(options.omit_builtin);
  config.merge_drivers = std::move(options.merge_drivers);
  config.subscribers = std::move(options.subscribers);
  auto manager = transaction_manager::create(std::move(config));
  if (!manager) {
    return std::unexpected(manager.error());
  }
  ws->manager_ = std::move(*manager);
  return ws;
}

result<snapshot> workspace::at(const digest &revision) const {
  return snapshot::open(*store_, revision);
}

} // namespace sl::fabric
