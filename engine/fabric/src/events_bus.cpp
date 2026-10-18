// The ordered in-process event bus (WO-047, SL-ARCH-001 ARCH1-R46, ARCH2-R13).

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/core/log.hpp"
#include "sl/fabric/events.hpp"

namespace sl::fabric {

namespace detail {

struct subscriber {
  std::string prefix;
  event_handler handler;
  std::atomic<bool> live{true};
};

struct file_closer {
  void operator()(std::FILE *f) const noexcept {
    if (f != nullptr) {
      std::fclose(f);
    }
  }
};

struct bus_core {
  mutable std::mutex mutex;
  std::map<std::uint64_t, std::shared_ptr<subscriber>>
      subscribers; // token order = subscription order
  std::uint64_t next_token = 1;
  std::deque<event_envelope> queue;
  bool draining = false;
  std::uint64_t last_seq = 0;
  std::uint64_t delivered = 0;
  std::shared_ptr<std::FILE> json; // shared: a drain in progress keeps its stream alive
  std::atomic<std::uint64_t> json_failures{0};
  sl::log::logger *log_target = nullptr;

  void unsubscribe(std::uint64_t token) noexcept {
    std::shared_ptr<subscriber> gone;
    {
      const std::scoped_lock lock(mutex);
      const auto it = subscribers.find(token);
      if (it == subscribers.end()) {
        return;
      }
      gone = std::move(it->second);
      subscribers.erase(it);
    }
    gone->live.store(false, std::memory_order_release);
  }

  [[nodiscard]] bool has(std::uint64_t token) const noexcept {
    const std::scoped_lock lock(mutex);
    return subscribers.contains(token);
  }

  // Writes the JSON line of `e` when the dump is on. Called by the draining thread only, without
  // the lock held; `file` is the stream captured under the lock.
  void dump(std::FILE *file, const event_envelope &e) noexcept {
    if (file == nullptr) {
      return;
    }
    auto line = to_json_line(e);
    if (!line) {
      json_failures.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    line->push_back('\n');
    // Unbuffered stream: one write(2) per line, so concurrent appenders do not interleave.
    const std::size_t n = std::fwrite(line->data(), 1, line->size(), file);
    if (n != line->size()) {
      json_failures.fetch_add(1, std::memory_order_relaxed);
    }
  }

  // Delivers queued events until the queue is empty. The caller set `draining` under the lock and
  // holds `lock` on entry and on return.
  void drain(std::unique_lock<std::mutex> &lock) {
    while (!queue.empty()) {
      event_envelope e = std::move(queue.front());
      queue.pop_front();
      std::vector<std::shared_ptr<subscriber>> targets;
      targets.reserve(subscribers.size());
      for (const auto &[token, sub] : subscribers) {
        if (std::string_view{e.type}.starts_with(sub->prefix)) {
          targets.push_back(sub);
        }
      }
      const std::shared_ptr<std::FILE> file = json;
      sl::log::logger *forward = log_target;
      ++delivered;
      lock.unlock();
      dump(file.get(), e);
      if (forward != nullptr) {
        log_event(*forward, e);
      }
      for (const auto &sub : targets) {
        if (sub->live.load(std::memory_order_acquire)) {
          sub->handler(e);
        }
      }
      lock.lock();
    }
    draining = false;
  }

  // Validates, orders and queues `batch`, then drains unless another call is already draining.
  [[nodiscard]] result<void> submit(std::vector<event_envelope> batch) {
    for (const auto &e : batch) {
      if (auto ok = validate(e); !ok) {
        return ok;
      }
    }
    std::ranges::stable_sort(
        batch, [](const event_envelope &a, const event_envelope &b) { return a.seq < b.seq; });
    std::unique_lock lock(mutex);
    std::uint64_t previous = last_seq;
    for (const auto &e : batch) {
      if (e.seq <= previous) {
        return make_error(errc::event_invalid, "seq: " + std::to_string(e.seq) +
                                                   " is not after the last accepted seq " +
                                                   std::to_string(previous));
      }
      previous = e.seq;
    }
    last_seq = previous;
    for (auto &e : batch) {
      queue.push_back(std::move(e));
    }
    if (!draining) {
      draining = true;
      drain(lock);
    }
    return {};
  }
};

} // namespace detail

// --- subscription ---

subscription::subscription(subscription &&other) noexcept
    : bus_(std::move(other.bus_)), token_(std::exchange(other.token_, 0)) {
  other.bus_.reset();
}

subscription &subscription::operator=(subscription &&other) noexcept {
  if (this != &other) {
    reset();
    bus_ = std::move(other.bus_);
    token_ = std::exchange(other.token_, 0);
    other.bus_.reset();
  }
  return *this;
}

subscription::~subscription() { reset(); }

void subscription::reset() noexcept {
  if (const auto core = bus_.lock()) {
    core->unsubscribe(token_);
  }
  bus_.reset();
  token_ = 0;
}

bool subscription::active() const noexcept {
  const auto core = bus_.lock();
  return core && core->has(token_);
}

// --- event_bus ---

event_bus::event_bus() : core_(std::make_shared<detail::bus_core>()) {}

event_bus::~event_bus() {
  // Handlers must not run after the bus is gone: mark every subscriber dead.
  std::map<std::uint64_t, std::shared_ptr<detail::subscriber>> gone;
  {
    const std::scoped_lock lock(core_->mutex);
    gone.swap(core_->subscribers);
  }
  for (const auto &[token, sub] : gone) {
    sub->live.store(false, std::memory_order_release);
  }
}

subscription event_bus::subscribe(std::string_view type_prefix, event_handler handler) {
  auto sub = std::make_shared<detail::subscriber>();
  sub->prefix = std::string{type_prefix};
  sub->handler = std::move(handler);
  const std::scoped_lock lock(core_->mutex);
  const std::uint64_t token = core_->next_token++;
  core_->subscribers.emplace(token, std::move(sub));
  return subscription{core_, token};
}

result<void> event_bus::publish(event_envelope e) {
  std::vector<event_envelope> one;
  one.push_back(std::move(e));
  return core_->submit(std::move(one));
}

result<void> event_bus::publish_batch(std::vector<event_envelope> batch) {
  return core_->submit(std::move(batch));
}

std::uint64_t event_bus::last_seq() const noexcept {
  const std::scoped_lock lock(core_->mutex);
  return core_->last_seq;
}

std::uint64_t event_bus::delivered() const noexcept {
  const std::scoped_lock lock(core_->mutex);
  return core_->delivered;
}

std::size_t event_bus::subscriber_count() const noexcept {
  const std::scoped_lock lock(core_->mutex);
  return core_->subscribers.size();
}

result<void> event_bus::enable_json_lines(const std::string &dump_path) {
  std::shared_ptr<std::FILE> file(std::fopen(dump_path.c_str(), "ab"), detail::file_closer{});
  if (!file) {
    return make_error(errc::io_error, "cannot open the event dump " + dump_path);
  }
  std::setvbuf(file.get(), nullptr, _IONBF, 0);
  const std::scoped_lock lock(core_->mutex);
  core_->json = std::move(file);
  return {};
}

result<bool> event_bus::enable_json_from_env() {
  const char *dump_path = std::getenv(std::string{event_json_env}.c_str());
  if (dump_path == nullptr || *dump_path == '\0') {
    return false;
  }
  if (auto ok = enable_json_lines(dump_path); !ok) {
    return std::unexpected(ok.error());
  }
  return true;
}

void event_bus::disable_json_lines() noexcept {
  const std::scoped_lock lock(core_->mutex);
  core_->json.reset();
}

std::uint64_t event_bus::json_failures() const noexcept {
  return core_->json_failures.load(std::memory_order_relaxed);
}

void event_bus::forward_to_log(sl::log::logger *target) noexcept {
  const std::scoped_lock lock(core_->mutex);
  core_->log_target = target;
}

} // namespace sl::fabric
