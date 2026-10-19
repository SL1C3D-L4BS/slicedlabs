// The workspace (WO-050, WO-055; SL-ARCH-002 ARCH2-R11 and ARCH2-R18, ADR-0016): registries
// frozen when it opens, the store, and the transaction manager. The durable create and open with
// recovery live in workspace_open.cpp.

#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/clock.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/recovery.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/vfs.hpp"
#include "sl/fabric/workspace.hpp"

namespace sl::fabric {

namespace {

// A cleanup whose failure changes nothing a caller can act on.
void best_effort(const result<void> &r) noexcept { static_cast<void>(r); }

// The store of a read-only workspace: every read forwards, every commit is refused.
class read_only_store final : public store_backend {
public:
  explicit read_only_store(std::shared_ptr<store_backend> inner) noexcept
      : inner_(std::move(inner)) {}

  [[nodiscard]] result<object> get(const digest &id) const noexcept override {
    return inner_->get(id);
  }
  [[nodiscard]] result<bool> has(const digest &id) const noexcept override {
    return inner_->has(id);
  }
  [[nodiscard]] result<digest> read_ref(std::string_view name) const noexcept override {
    return inner_->read_ref(name);
  }
  [[nodiscard]] result<std::vector<ref_entry>> list_refs() const noexcept override {
    return inner_->list_refs();
  }
  [[nodiscard]] result<commit_ack> commit(const mutation_capability & /*cap*/,
                                          commit_batch /*batch*/) noexcept override {
    return make_error(errc::permission_denied, "the workspace is open read-only");
  }
  [[nodiscard]] result<verify_report> verify(const verify_scope &scope) const noexcept override {
    return inner_->verify(scope);
  }

private:
  std::shared_ptr<store_backend> inner_;
};

} // namespace

workspace::workspace(open_key /*key*/, std::shared_ptr<store_backend> store,
                     workspace_options &options) noexcept
    : store_(std::move(store)), schemas_(std::move(options.schemas)),
      domains_(std::move(options.domains)), actors_(std::move(options.actors)) {}

workspace::~workspace() {
  manager_.reset();
  store_.reset();
  if (lock_.is_open()) {
    // A clean close leaves an empty lock file: the next writer finds no stale holder.
    best_effort(lock_.truncate(0));
    best_effort(lock_.unlock());
    lock_.close();
  }
}

result<std::unique_ptr<workspace>> workspace::create_memory(workspace_options options) {
  return create_with(std::make_shared<memory_backend>(), std::move(options));
}

result<std::unique_ptr<workspace>> workspace::create_with(std::shared_ptr<store_backend> store,
                                                          workspace_options options,
                                                          open_mode mode) {
  if (!store) {
    return make_error(errc::invalid_argument, "a workspace needs a store");
  }
  return assemble(std::move(store), options, nullptr, vfs_file{}, recovery_report{}, mode);
}

result<std::unique_ptr<workspace>> workspace::assemble(std::shared_ptr<store_backend> store,
                                                       workspace_options &options,
                                                       std::shared_ptr<vfs> fs, vfs_file lock,
                                                       recovery_report report, open_mode mode) {
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
  if (mode == open_mode::read_only) {
    store = std::make_shared<read_only_store>(std::move(store));
  }
  auto ws = std::make_unique<workspace>(open_key{}, std::move(store), options);
  ws->fs_ = std::move(fs);
  ws->lock_ = std::move(lock);
  ws->recovery_ = std::move(report);
  ws->mode_ = mode;
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
