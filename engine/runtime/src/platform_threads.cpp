// Threads of the platform (WO-078, SL-ARCH-003 ARCH3-R037, R040, RFP1). pthread_create directly,
// so an OS refusal is a thread_spawn_failed value and not an exception; the start block carries
// the body and the OS name, and the new thread sets its own name (pthread_setname_np, 15 bytes)
// before the body runs. One of the src/platform_*.cpp files allowed to create threads.

#include "sl/core/error.hpp"
#include "sl/runtime/error.hpp"
#include "sl/runtime/faults.hpp"
#include "sl/runtime/headless_platform.hpp"
#include "sl/runtime/platform.hpp"

#include <pthread.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sl::runtime {

struct thread_handle::state {
  pthread_t tid{};
  std::string name;
  bool joinable = false;
};

namespace {

// Linux limits a thread name to 16 bytes with the terminator.
constexpr std::size_t os_name_bytes = 15;

struct start_block {
  std::function<void()> body;
  std::string os_name;
};

void *thread_main(void *arg) noexcept {
  const std::unique_ptr<start_block> block(static_cast<start_block *>(arg));
  static_cast<void>(pthread_setname_np(pthread_self(), block->os_name.c_str()));
  block->body();
  return nullptr;
}

} // namespace

thread_handle::thread_handle() noexcept = default;

thread_handle::thread_handle(std::unique_ptr<state> s) noexcept : state_(std::move(s)) {}

thread_handle::thread_handle(thread_handle &&other) noexcept = default;

thread_handle &thread_handle::operator=(thread_handle &&other) noexcept {
  if (this != &other) {
    join();
    state_ = std::move(other.state_);
  }
  return *this;
}

thread_handle::~thread_handle() { join(); }

bool thread_handle::joinable() const noexcept { return state_ != nullptr && state_->joinable; }

std::string_view thread_handle::name() const noexcept {
  return state_ != nullptr ? std::string_view{state_->name} : std::string_view{};
}

void thread_handle::join() noexcept {
  if (!joinable()) {
    return;
  }
  static_cast<void>(pthread_join(state_->tid, nullptr));
  state_->joinable = false;
}

result<thread_handle> os_thread_factory::spawn(std::string name, std::function<void()> body,
                                               affinity_hint /*hint*/) noexcept {
#if SL_RUNTIME_FAULTS
  // compiled out of Release, where visit() never fires (-Wunreachable-code-return)
  if (visit(failpoint::rfp1).fire) {
    return make_error(errc::thread_spawn_failed, "thread_factory::spawn: RFP1 thread.spawn");
  }
#endif
  if (name.empty() || !body) {
    return make_error(errc::invalid_argument, "thread_factory::spawn: empty name or body");
  }
  std::string full = "slrt" + std::to_string(instance_id_) + "-" + name;
  auto block = std::make_unique<start_block>(
      start_block{.body = std::move(body), .os_name = full.substr(0, os_name_bytes)});
  auto st = std::make_unique<thread_handle::state>();
  pthread_t tid{};
  const int rc = pthread_create(&tid, nullptr, &thread_main, block.get());
  if (rc != 0) {
    return make_error(errc::thread_spawn_failed, "thread_factory::spawn: pthread_create " +
                                                     std::to_string(rc) + " for " + full);
  }
  [[maybe_unused]] start_block *const owned_by_thread = block.release(); // thread_main frees it
  st->tid = tid;
  st->name = std::move(full);
  st->joinable = true;
  return thread_handle(std::move(st));
}

thread_group &thread_group::operator=(thread_group &&other) noexcept {
  if (this != &other) {
    static_cast<void>(release());
    handles_ = std::move(other.handles_);
  }
  return *this;
}

thread_group::~thread_group() { static_cast<void>(release()); }

result<void> thread_group::spawn(thread_factory &factory, std::string name,
                                 std::function<void()> body, affinity_hint hint) noexcept {
  auto handle = factory.spawn(std::move(name), std::move(body), hint);
  if (!handle) {
    return std::unexpected(std::move(handle.error()));
  }
  handles_.push_back(std::move(*handle));
  return {};
}

std::vector<std::string> thread_group::release() noexcept {
  std::vector<std::string> order;
  order.reserve(handles_.size());
  while (!handles_.empty()) {
    thread_handle &last = handles_.back();
    last.join();
    order.emplace_back(last.name());
    handles_.pop_back();
  }
  return order;
}

} // namespace sl::runtime
