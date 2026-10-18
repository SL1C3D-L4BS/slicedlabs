// The durable workspace (WO-055, SL-ARCH-002 ARCH2-R28, ARCH2-R34, ARCH2-R35 and `recovery_steps`
// R1, ADR-0015): create and open over a log store, the OFD writer lock with stale-lock takeover,
// and the read-only open.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#define SL_FABRIC_HAVE_GETPID 1
#else
#define SL_FABRIC_HAVE_GETPID 0
#endif

#include "sl/core/error.hpp"
#include "sl/fabric/clock.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/log_backend.hpp"
#include "sl/fabric/recovery.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/vfs.hpp"
#include "sl/fabric/workspace.hpp"

namespace sl::fabric {

namespace {

namespace stdfs = std::filesystem;

template <class T> std::unexpected<error> pass(const result<T> &r) {
  return std::unexpected(r.error());
}

std::span<const std::uint8_t> text_bytes(std::string_view text) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): chars viewed as their bytes
  return {reinterpret_cast<const std::uint8_t *>(text.data()), text.size()};
}

std::int64_t process_id() noexcept {
#if SL_FABRIC_HAVE_GETPID
  return static_cast<std::int64_t>(::getpid());
#else
  return 0;
#endif
}

// A cleanup whose failure changes nothing a caller can act on: the holder text then stays and the
// next writer reports a stale lock.
void best_effort(const result<void> &r) noexcept { static_cast<void>(r); }

// The writer lock of R1: released with an empty file unless kept by a workspace.
class writer_lock {
public:
  writer_lock() = default;
  writer_lock(const writer_lock &) = delete;
  writer_lock(writer_lock &&) = delete;
  writer_lock &operator=(const writer_lock &) = delete;
  writer_lock &operator=(writer_lock &&) = delete;
  ~writer_lock() {
    if (file_.is_open()) {
      best_effort(file_.truncate(0));
      best_effort(file_.unlock());
    }
  }

  // R1: lock `.slfabric/lock` (creating it); a holder text left by a writer that is gone is a
  // stale lock (C11), returned in `stale`.
  result<void> take(vfs &fs, const stdfs::path &store, std::int64_t now_us,
                    std::optional<std::string> &stale) {
    result<vfs_file> f = fs.open(store / "lock", open_options{.write = true, .create = true});
    if (!f) {
      return pass(f);
    }
    if (result<void> ok = f->lock_ofd(); !ok) {
      if (ok.error().code == errc::writer_locked) {
        return make_error(errc::writer_locked,
                          (store / "lock").string() + ": another writer holds the workspace");
      }
      return ok;
    }
    const result<std::uint64_t> size = f->size();
    if (!size) {
      return pass(size);
    }
    if (*size > 0) {
      std::vector<std::uint8_t> old(static_cast<std::size_t>(*size));
      if (result<void> ok = f->read_exact(0, old); !ok) {
        return ok;
      }
      std::string text(old.begin(), old.end());
      while (!text.empty() && (text.back() == '\n' || text.back() == '\0')) {
        text.pop_back();
      }
      stale = text.empty() ? std::string("(no holder text)") : text;
    }
    if (result<void> ok = f->truncate(0); !ok) {
      return ok;
    }
    const std::string holder = "slfabric writer pid " + std::to_string(process_id()) + " since " +
                               std::to_string(now_us) + "\n";
    if (result<void> ok = f->pwrite(0, text_bytes(holder)); !ok) {
      return ok;
    }
    file_ = std::move(*f);
    return {};
  }

  vfs_file release() noexcept { return std::move(file_); }

private:
  vfs_file file_;
};

// The store of a read-only workspace: every read forwards, every commit is refused.
class read_only_store final : public store_backend {
public:
  explicit read_only_store(std::unique_ptr<log_backend> inner) noexcept
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
  std::unique_ptr<log_backend> inner_;
};

std::shared_ptr<vfs> file_system(const workspace_options &options) {
  if (options.fs) {
    return options.fs;
  }
  return std::make_shared<vfs_posix>();
}

std::int64_t now_of(const workspace_options &options) noexcept {
  if (options.clock) {
    return options.clock->now_us();
  }
  return system_clock_source{}.now_us();
}

// R1's finding for a stale lock that was taken over (C11), first in the report.
void report_stale(recovery_report &report, std::optional<std::string> stale) {
  if (!stale) {
    return;
  }
  recovery_finding f;
  f.kind = corruption_class::c11;
  f.code = errc::writer_locked;
  f.file = "lock";
  f.blocking = false;
  f.message = "C11 stale lock: lock: taken over from a writer that is gone (" + *stale + ")";
  report.findings.insert(report.findings.begin(), std::move(f));
  report.stale_lock = std::move(stale);
}

result<void> ensure_dir(vfs &fs, const stdfs::path &dir, durability d) noexcept {
  const result<bool> present = fs.exists(dir);
  if (!present) {
    return pass(present);
  }
  if (*present) {
    return {};
  }
  if (result<void> ok = fs.make_dir(dir); !ok && ok.error().code != errc::already_exists) {
    return ok;
  }
  if (d == durability::none) {
    return {};
  }
  const stdfs::path parent = dir.parent_path();
  return fs.sync_dir(parent.empty() ? stdfs::path(".") : parent);
}

} // namespace

result<std::unique_ptr<workspace>> workspace::create(const std::filesystem::path &root,
                                                     workspace_options options) {
  std::shared_ptr<vfs> fs = file_system(options);
  const stdfs::path store = root / store_dir_name;
  if (result<void> ok = ensure_dir(*fs, root, options.log.sync); !ok) {
    return pass(ok);
  }
  const result<bool> formatted = fs->exists(store / "FORMAT");
  if (!formatted) {
    return pass(formatted);
  }
  if (*formatted) {
    return make_error(errc::already_exists, store.string() + " already holds a store");
  }
  if (result<void> ok = ensure_dir(*fs, store, options.log.sync); !ok) {
    return pass(ok);
  }
  writer_lock lock;
  std::optional<std::string> stale;
  if (result<void> ok = lock.take(*fs, store, now_of(options), stale); !ok) {
    return pass(ok);
  }
  result<std::unique_ptr<log_backend>> log = log_backend::open(*fs, store, options.log);
  if (!log) {
    return pass(log);
  }
  recovery_report report;
  report_stale(report, std::move(stale));
  std::shared_ptr<store_backend> backend(std::move(*log));
  return assemble(std::move(backend), options, std::move(fs), lock.release(), std::move(report),
                  open_mode::write);
}

result<std::unique_ptr<workspace>> workspace::open(const std::filesystem::path &root,
                                                   open_mode mode, const recovery_policy &policy) {
  return open(root, mode, policy, workspace_options{});
}

result<std::unique_ptr<workspace>> workspace::open(const std::filesystem::path &root,
                                                   open_mode mode, const recovery_policy &policy,
                                                   workspace_options options) {
  std::shared_ptr<vfs> fs = file_system(options);
  const stdfs::path store = root / store_dir_name;
  const result<bool> present = fs->exists(store);
  if (!present) {
    return pass(present);
  }
  if (!*present) {
    return make_error(errc::not_found, root.string() + " holds no workspace (no " +
                                           std::string(store_dir_name) + ")");
  }
  writer_lock lock;
  std::optional<std::string> stale;
  if (mode == open_mode::write) {
    if (result<void> ok = lock.take(*fs, store, now_of(options), stale); !ok) {
      return pass(ok);
    }
  }
  result<recovered_log> recovered =
      recover_log(*fs, store, policy, options.log,
                  mode == open_mode::write ? recovery_mode::repair : recovery_mode::inspect);
  if (!recovered) {
    return pass(recovered);
  }
  recovery_report report = std::move(recovered->report);
  report_stale(report, std::move(stale));
  if (report.blocking()) {
    return make_error(errc::journal_corrupt,
                      "open " + root.string() + " refused: " + report.summary());
  }
  if (!recovered->store) {
    return make_error(errc::recovery_required, "open " + root.string() +
                                                   " read-only: the log needs a repair first (" +
                                                   report.summary() + ")");
  }
  std::shared_ptr<store_backend> backend;
  if (mode == open_mode::read_only) {
    backend = std::make_shared<read_only_store>(std::move(recovered->store));
  } else {
    backend = std::shared_ptr<store_backend>(std::move(recovered->store));
  }
  return assemble(std::move(backend), options, std::move(fs), lock.release(), std::move(report),
                  mode);
}

} // namespace sl::fabric
