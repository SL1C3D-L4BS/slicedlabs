// The crash harness (WO-055). See crash_harness.hpp.

#include "crash_harness.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "faulty_vfs.hpp"
#include "model_store.hpp"
#include "patch_gen.hpp"
#include "test_schemas.hpp"
#include "tx_fixture.hpp"

#include "sl/core/error.hpp"
#include "sl/fabric/clock.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/failpoints.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/journal.hpp"
#include "sl/fabric/log_backend.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/recovery.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/vfs.hpp"
#include "sl/fabric/workspace.hpp"

namespace sl::fabric::testing {

namespace {

std::optional<double> env_number(const char *name) {
  const char *env = std::getenv(name); // NOLINT(concurrency-mt-unsafe): read before threads
  if (env == nullptr || *env == '\0') {
    return std::nullopt;
  }
  const std::string_view s(env);
  double v = 0;
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc{} || end != s.data() + s.size()) {
    return std::nullopt;
  }
  return v;
}

std::shared_ptr<const clock> harness_clock() { return std::make_shared<manual_clock>(1'000'000); }

std::optional<digest> head_of(const workspace &ws) {
  const result<snapshot> s = ws.manager().head();
  return s ? s->revision_digest() : std::nullopt;
}

std::string text_of(const error &e) { return std::string(to_string(e.code)) + ": " + e.message; }

// Commits patch `p` through `ws`, over `base` when given (a client re-sends a patch on the base it
// was authored on, ARCH2-R49).
result<commit_receipt> commit_patch(workspace &ws, const patch &p,
                                    const std::optional<digest> &base = std::nullopt) {
  tx_options o = harness_tx();
  o.base = base;
  result<transaction> t = ws.manager().begin(std::move(o));
  if (!t) {
    return std::unexpected(t.error());
  }
  if (result<void> ok = t->stage(p); !ok) {
    return std::unexpected(ok.error());
  }
  return t->commit();
}

} // namespace

double sweep_scale() noexcept {
  const std::optional<double> v = env_number("SL_FABRIC_SWEEP_SCALE");
  if (!v || !(*v > 0.0)) {
    return 1.0;
  }
  return std::min(*v, 100.0);
}

std::size_t scaled(std::size_t n, std::size_t minimum) noexcept {
  const double v = std::round(static_cast<double>(n) * sweep_scale());
  return std::max(minimum, static_cast<std::size_t>(v));
}

std::optional<std::uint64_t> soak_seed() noexcept {
  const char *env = std::getenv("SL_FABRIC_SOAK_SEED"); // NOLINT(concurrency-mt-unsafe)
  if (env == nullptr || *env == '\0') {
    return std::nullopt;
  }
  const std::string_view s(env);
  std::uint64_t v = 0;
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc{} || end != s.data() + s.size()) {
    return std::nullopt;
  }
  return v;
}

std::size_t sweep_threads() noexcept {
  return std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, 8);
}

void parallel_for(std::size_t count, const std::function<void(std::size_t)> &body) {
  std::atomic<std::size_t> next{0};
  std::vector<std::jthread> pool;
  const std::size_t n = std::min(sweep_threads(), std::max<std::size_t>(count, 1));
  pool.reserve(n);
  for (std::size_t t = 0; t < n; ++t) {
    pool.emplace_back([&next, count, &body] {
      for (std::size_t i = next++; i < count; i = next++) {
        body(i);
      }
    });
  }
}

std::filesystem::path harness_root() { return "/ws"; }
std::filesystem::path harness_store() { return harness_root() / workspace::store_dir_name; }

tx_options harness_tx() {
  tx_options o;
  o.origin = tx_origin_of(tx_editor());
  return o;
}

result<crash_workload> make_workload(std::uint64_t seed, std::size_t commits, std::size_t records,
                                     std::size_t max_ops) {
  crash_workload w;
  w.seed = seed;
  patch_gen_options go;
  go.meta_percent = 0;
  go.max_ops = std::max<std::size_t>(max_ops, 1);
  patch_gen gen(seed, go);
  result<oracle_store> start = gen.state(records);
  if (!start) {
    return std::unexpected(start.error());
  }
  oracle_store oracle = std::move(*start);
  w.states.emplace_back();
  w.heads.emplace_back(std::nullopt);
  patch first = load_patch(oracle);
  first.meta.idempotency_key = "w-0";
  w.patches.push_back(std::move(first));
  w.states.push_back(oracle);
  for (std::size_t i = 1; i < std::max<std::size_t>(commits, 1); ++i) {
    result<patch> p = gen.next(oracle);
    if (!p) {
      return std::unexpected(p.error());
    }
    p->base.reset();
    p->meta = patch_meta{};
    p->meta.idempotency_key = "w-" + std::to_string(i);
    if (result<void> ok = oracle.apply(*p); !ok) {
      return std::unexpected(ok.error());
    }
    w.patches.push_back(std::move(*p));
    w.states.push_back(oracle);
  }
  result<workspace_options> o = gen_options(harness_clock());
  if (!o) {
    return std::unexpected(o.error());
  }
  result<std::unique_ptr<workspace>> ws =
      workspace::create_with(std::make_shared<memory_backend>(), std::move(*o));
  if (!ws) {
    return std::unexpected(ws.error());
  }
  for (std::size_t i = 0; i < w.patches.size(); ++i) {
    result<commit_receipt> r = commit_patch(**ws, w.patches[i]);
    if (!r) {
      return std::unexpected(r.error());
    }
    w.heads.emplace_back(r->revision);
    result<snapshot> head = (*ws)->manager().head();
    if (!head) {
      return std::unexpected(head.error());
    }
    result<oracle_store> state = oracle_of(*head);
    if (!state) {
      return std::unexpected(state.error());
    }
    if (*state != w.states[i + 1]) {
      return make_error(errc::internal, "make_workload: the memory run of commit " +
                                            std::to_string(i + 1) + " differs from the oracle");
    }
  }
  return w;
}

result<workspace_options> harness_options(std::shared_ptr<vfs> fs, const harness_config &cfg) {
  result<workspace_options> o = gen_options(harness_clock());
  if (!o) {
    return o;
  }
  o->fs = std::move(fs);
  o->log = cfg.log;
  return o;
}

void note_failure(const std::vector<std::string> &trace, crash_run &run) {
  std::string appended;
  bool synced = true;
  for (const std::string &line : trace) {
    const std::string_view l(line);
    const std::string_view verb = l.substr(0, l.find(' '));
    if (l.ends_with("!fail")) {
      run.failed_primitive = line;
      run.failed_on_read = verb == "read" || verb == "size" || verb == "list" || verb == "exists";
      break;
    }
    const std::string_view rest = l.substr(std::min(l.size(), verb.size() + 1));
    const std::string file(rest.substr(0, rest.find(' ')));
    if (verb == "append") {
      appended = file;
      synced = false;
    } else if ((verb == "fsync" || verb == "fdatasync") && file == appended) {
      synced = true;
    }
  }
  run.unsynced_append = !appended.empty() && !synced;
}

crash_run run_workload(const std::shared_ptr<faulty_vfs> &fs, const crash_workload &w,
                       const harness_config &cfg, std::optional<std::uint64_t> fail_after,
                       std::optional<armed_failpoint> fp) {
  crash_run run;
  const std::uint64_t start = fs->primitives();
  fs->clear_trace();
  result<workspace_options> o = harness_options(fs, cfg);
  if (!o) {
    run.failure = o.error();
    return run;
  }
  if (fail_after) {
    fs->fail_after(*fail_after);
  }
  {
    result<std::unique_ptr<workspace>> ws = workspace::create(harness_root(), std::move(*o));
    if (!ws) {
      run.failure = ws.error();
    } else {
      run.created = true;
      for (std::size_t i = 0; i < w.patches.size(); ++i) {
        if (fp && fp->commit == i) {
          if (result<void> ok = tx_failpoints::arm((*ws)->manager().failpoints(), fp->fp); !ok) {
            run.failure = ok.error();
            break;
          }
        }
        result<transaction> t = (*ws)->manager().begin(harness_tx());
        if (!t) {
          run.failure = t.error();
          break;
        }
        if (result<void> ok = t->stage(w.patches[i]); !ok) {
          run.failure = ok.error();
          break;
        }
        result<commit_receipt> r = t->commit();
        if (!r) {
          run.failure = r.error();
          run.in_flight = true;
          if (fp && fp->commit == i && (*ws)->manager().failpoints().trips(fp->fp) > 0) {
            run.tripped = fp->fp;
          }
          break;
        }
        ++run.acked;
      }
    }
  }
  run.primitives = fs->primitives() - start;
  note_failure(fs->trace(), run);
  return run;
}

recovery_check check_recovery(const std::shared_ptr<faulty_vfs> &fs, const crash_workload &w,
                              const harness_config &cfg, const crash_run &run) {
  recovery_check out;
  const std::size_t n = w.patches.size();
  const auto fail = [&out](std::string what) -> recovery_check {
    out.violation = "FAB-INV-RCV-09: " + std::move(what);
    return out;
  };
  const auto options = [&] { return harness_options(fs, cfg); };
  result<workspace_options> o = options();
  if (!o) {
    return fail("options: " + text_of(o.error()));
  }
  result<std::unique_ptr<workspace>> ws =
      workspace::open(harness_root(), open_mode::write, cfg.policy, std::move(*o));
  if (!ws && ws.error().code == errc::not_found) {
    if (run.created) {
      return fail("the acknowledged workspace creation is gone: " + text_of(ws.error()));
    }
    out.created_again = true;
    o = options();
    if (!o) {
      return fail("options: " + text_of(o.error()));
    }
    ws = workspace::create(harness_root(), std::move(*o));
  }
  if (!ws) {
    return fail("reopen failed: " + text_of(ws.error()));
  }
  out.first = (*ws)->last_recovery();
  const std::optional<digest> head = head_of(**ws);
  const auto at = std::ranges::find(w.heads, head);
  if (at == w.heads.end()) {
    return fail("the head after recovery is no head of the workload (a partial commit)");
  }
  std::size_t k = static_cast<std::size_t>(at - w.heads.begin());
  out.recovered = k;
  if (k < run.acked) {
    return fail("acknowledged commit lost: recovered " + std::to_string(k) + " of " +
                std::to_string(run.acked) + " acknowledged commits");
  }
  if (k > run.acked + (run.in_flight ? 1U : 0U)) {
    return fail("recovered " + std::to_string(k) + " commits but only " +
                std::to_string(run.acked) + " were acknowledged and none was in flight");
  }
  {
    result<snapshot> s = (*ws)->manager().head();
    result<oracle_store> state =
        s ? oracle_of(*s) : result<oracle_store>(std::unexpected(s.error()));
    if (!state) {
      return fail("reading the recovered head: " + text_of(state.error()));
    }
    if (*state != w.states[k]) {
      return fail("the recovered records differ from the oracle after " + std::to_string(k) +
                  " commits");
    }
  }
  // The keyed retry of the commit in flight converges on the next head.
  // A durable first commit has no base to re-send it on (an empty base reads as the head).
  if (run.in_flight && run.acked < n && (w.heads[run.acked] || k == run.acked)) {
    const std::size_t i = run.acked;
    result<commit_receipt> r = commit_patch(**ws, w.patches[i], w.heads[i]);
    if (!r) {
      return fail("the keyed retry of commit " + std::to_string(i + 1) +
                  " failed: " + text_of(r.error()));
    }
    if (r->revision != w.heads[i + 1]) {
      return fail("the keyed retry of commit " + std::to_string(i + 1) + " made another revision");
    }
    if (k == i + 1 && !r->recovered) {
      return fail("the keyed retry of a durable commit committed again instead of returning "
                  "the original receipt");
    }
    k = i + 1;
  }
  ws->reset();
  // Recovery is idempotent: a second reopen loses nothing and finds the same head.
  o = options();
  if (!o) {
    return fail("options: " + text_of(o.error()));
  }
  ws = workspace::open(harness_root(), open_mode::write, cfg.policy, std::move(*o));
  if (!ws) {
    return fail("the second reopen failed: " + text_of(ws.error()));
  }
  if (!(*ws)->last_recovery().lossless()) {
    return fail("recovery is not idempotent: " + (*ws)->last_recovery().summary());
  }
  if (head_of(**ws) != w.heads[k]) {
    return fail("the second reopen found another head");
  }
  // The workspace is writable: the next commit of the workload lands on the oracle's next head.
  for (std::size_t i = k; i < std::min(n, k + 1); ++i) {
    result<commit_receipt> r = commit_patch(**ws, w.patches[i]);
    if (!r) {
      return fail("commit " + std::to_string(i + 1) + " after recovery: " + text_of(r.error()));
    }
    if (r->revision != w.heads[i + 1]) {
      return fail("commit " + std::to_string(i + 1) + " after recovery made another revision");
    }
  }
  return out;
}

record_id operation_named(std::string_view name) noexcept {
  return test_id(id_class::operation, name);
}

result<std::uint64_t> commit_intents(vfs &fs, const log_options &log,
                                     std::vector<intent_record> intents,
                                     std::vector<object> objects) {
  result<std::unique_ptr<log_backend>> store = log_backend::open(fs, harness_store(), log);
  if (!store) {
    return std::unexpected(store.error());
  }
  commit_batch batch;
  batch.objects = std::move(objects);
  batch.intents = std::move(intents);
  batch.commit_time_us = 2'000'000;
  result<commit_ack> ack = backdoor::commit(**store, std::move(batch));
  if (!ack) {
    return std::unexpected(ack.error());
  }
  return ack->seq;
}

result<disk_image> image_of(vfs_memory &fs, const std::filesystem::path &root) {
  disk_image img;
  std::vector<std::filesystem::path> todo{root};
  while (!todo.empty()) {
    const std::filesystem::path cur = todo.back();
    todo.pop_back();
    result<std::vector<std::string>> names = fs.list(cur);
    if (!names) {
      return std::unexpected(names.error());
    }
    img.dirs.insert(cur.generic_string());
    for (const std::string &name : *names) {
      const std::filesystem::path child = cur / name;
      result<std::vector<std::uint8_t>> bytes = fs.contents(child);
      if (bytes) {
        img.files.insert_or_assign(child.generic_string(), std::move(*bytes));
      } else {
        todo.push_back(child);
      }
    }
  }
  return img;
}

namespace {

// Writes `image` into `fs`; `durable` also syncs every file and directory.
result<void> fill(vfs &fs, const disk_image &image, bool durable) {
  for (const std::string &dir : image.dirs) {
    if (result<void> ok = fs.make_dir(dir); !ok && ok.error().code != errc::already_exists) {
      return ok;
    }
  }
  for (const auto &[path, bytes] : image.files) {
    result<vfs_file> f = fs.open(path, open_create);
    if (!f) {
      return std::unexpected(f.error());
    }
    if (!bytes.empty()) {
      if (result<std::uint64_t> at = f->append(bytes); !at) {
        return std::unexpected(at.error());
      }
    }
    if (!durable) {
      continue;
    }
    if (result<void> ok = f->fsync(); !ok) {
      return ok;
    }
  }
  if (!durable) {
    return {};
  }
  if (result<void> ok = fs.sync_dir("/"); !ok) {
    return ok;
  }
  for (const std::string &dir : image.dirs) {
    if (result<void> ok = fs.sync_dir(dir); !ok) {
      return ok;
    }
  }
  return {};
}

} // namespace

result<std::shared_ptr<faulty_vfs>> restore(const disk_image &image) {
  auto fs = std::make_shared<faulty_vfs>();
  if (result<void> ok = fill(*fs, image, true); !ok) {
    return std::unexpected(ok.error());
  }
  fs->clear_trace();
  return fs;
}

result<std::shared_ptr<vfs_memory>> restore_memory(const disk_image &image) {
  auto fs = std::make_shared<vfs_memory>();
  if (result<void> ok = fill(*fs, image, false); !ok) {
    return std::unexpected(ok.error());
  }
  return fs;
}

result<built_log> build_log(std::uint64_t seed, std::size_t commits, const harness_config &cfg,
                            std::size_t records, std::size_t max_ops) {
  result<crash_workload> w = make_workload(seed, commits, records, max_ops);
  if (!w) {
    return std::unexpected(w.error());
  }
  built_log out{.workload = std::move(*w), .fs = std::make_shared<faulty_vfs>(), .image = {}};
  const crash_run run = run_workload(out.fs, out.workload, cfg, std::nullopt);
  if (run.failure) {
    return std::unexpected(*run.failure);
  }
  result<disk_image> img = image_of(*out.fs->memory(), harness_root());
  if (!img) {
    return std::unexpected(img.error());
  }
  out.image = std::move(*img);
  return out;
}

std::string segment_path(std::uint32_t n) {
  return (harness_store() / "log" / segment_file_name(n)).generic_string();
}

std::vector<frame_entry> frame_table(byte_span segment) {
  std::vector<frame_entry> out;
  if (segment.size() <= segment_header_size) {
    return out;
  }
  frame_reader r(segment.subspan(segment_header_size), segment_header_size);
  while (const std::optional<frame_view> f = r.next()) {
    out.push_back(frame_entry{.offset = f->offset, .end = r.stop().end, .type = f->type});
  }
  return out;
}

scratch_dir::scratch_dir(std::string_view name) {
  std::error_code ec;
  root_ = std::filesystem::current_path(ec) / (std::string(name) + ".scratch");
  if (ec) {
    return;
  }
  std::filesystem::remove_all(root_, ec);
  std::filesystem::create_directories(root_, ec);
  ok_ = !ec;
}

scratch_dir::~scratch_dir() {
  std::error_code ec;
  std::filesystem::remove_all(root_, ec);
}

} // namespace sl::fabric::testing
