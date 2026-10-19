// FAB-INV-STO-06 (SL-ARCH-002 ARCH2-R29; ARCH2-R30, R33 and R38 at the backend): the WO-042
// store_backend contract suite passes on log_backend over the posix vfs (in a scratch directory
// under the ctest working directory, inside the build tree, removed by RAII) and over the faulty
// vfs; a reopened log serves every acknowledged commit; one commit is one append and one fsync;
// a crash at every storage primitive of a commit (FP3 torn commit write, FP4 after the write
// before the fsync, FP5 after the fsync, and between the steps of a loose blob's tmp-and-rename)
// loses no acknowledged commit and never shows a partial one; a failed sync is
// commit_indeterminate and blocks commits until reopen; open refuses a dirty tail with
// recovery_required (recovery itself is WO-055).

#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/journal.hpp"
#include "sl/fabric/log_backend.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/vfs.hpp"

#include "../../../engine/fabric/testing/faulty_vfs.hpp"
#include "../../../engine/fabric/testing/store_contract.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using sl::errc;
using sl::fabric::commit_ack;
using sl::fabric::commit_batch;
using sl::fabric::digest;
using sl::fabric::durability;
using sl::fabric::intent_outcome;
using sl::fabric::intent_phase;
using sl::fabric::intent_record;
using sl::fabric::log_backend;
using sl::fabric::log_options;
using sl::fabric::object;
using sl::fabric::record_id;
using sl::fabric::ref_entry;
using sl::fabric::ref_update;
using sl::fabric::store_backend;
using sl::fabric::testing::backdoor;
using sl::fabric::testing::crash_policy;
using sl::fabric::testing::faulty_vfs;

constexpr std::string_view store_text = "ws/.slfabric";
constexpr std::string_view segment1 = "ws/.slfabric/log/seg-00000001.slg";

std::filesystem::path store_path() { return store_text; }

// A directory under the ctest working directory (the build tree), removed on both ends.
class scratch_dir {
public:
  explicit scratch_dir(std::string_view name) : root_(root_for(name)) {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
    std::filesystem::create_directories(root_, ec);
    REQUIRE_FALSE(ec);
  }
  scratch_dir(const scratch_dir &) = delete;
  scratch_dir(scratch_dir &&) = delete;
  scratch_dir &operator=(const scratch_dir &) = delete;
  scratch_dir &operator=(scratch_dir &&) = delete;
  ~scratch_dir() {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }
  [[nodiscard]] const std::filesystem::path &path() const noexcept { return root_; }

private:
  static std::filesystem::path root_for(std::string_view name) {
    std::error_code ec;
    std::filesystem::path cwd = std::filesystem::current_path(ec);
    REQUIRE_FALSE(ec);
    return cwd / (std::string(name) + ".scratch");
  }

public:
private:
  std::filesystem::path root_;
};

object blob(std::string_view text) {
  const std::vector<std::uint8_t> bytes(text.begin(), text.end());
  return sl::fabric::make_object(sl::fabric::object_kind::blob, bytes);
}

object big_blob(std::size_t size, std::uint8_t seed) {
  std::vector<std::uint8_t> bytes(size);
  for (std::size_t i = 0; i < size; ++i) {
    bytes[i] = static_cast<std::uint8_t>((i * 131U) + seed);
  }
  return sl::fabric::make_object(sl::fabric::object_kind::blob, bytes);
}

record_id op(std::uint32_t n) { return sl::fabric::well_known(sl::fabric::id_class::operation, n); }

ref_update ref(std::string name, std::optional<digest> expected, digest desired) {
  return ref_update{.name = std::move(name), .expected = expected, .desired = desired};
}

std::unique_ptr<log_backend> open_store(sl::fabric::vfs &fs, const std::filesystem::path &dir,
                                        const log_options &options = {}) {
  auto s = log_backend::open(fs, dir, options);
  if (!s) {
    FAIL("open: " << s.error().message);
  }
  return std::move(*s);
}

struct posix_factory {
  const scratch_dir *dir = nullptr;
  sl::fabric::vfs_posix fs;
  int made = 0;

  std::unique_ptr<store_backend> make() {
    const std::filesystem::path ws = dir->path() / ("store" + std::to_string(made++));
    REQUIRE(fs.make_dir(ws).has_value());
    return open_store(fs, ws / ".slfabric");
  }
};

struct faulty_factory {
  log_options options;
  std::vector<std::unique_ptr<faulty_vfs>> fs;

  std::unique_ptr<store_backend> make() {
    fs.push_back(std::make_unique<faulty_vfs>());
    REQUIRE(fs.back()->make_dir("ws").has_value());
    return open_store(*fs.back(), store_path(), options);
  }
};

// Everything a client can observe about the store, for a fixed list of ids.
struct observed {
  std::uint64_t seq = 0;
  std::vector<ref_entry> refs;
  std::vector<bool> readable;
  std::vector<record_id> open;

  friend bool operator==(const observed &, const observed &) = default;
};

// The same without reading object bytes (`has` instead of `get`): usable while the vfs is dead.
observed observe_index(const log_backend &s, const std::vector<object> &objects) {
  observed o;
  o.seq = s.last_seq();
  o.refs = s.list_refs().value_or(std::vector<ref_entry>{});
  for (const object &x : objects) {
    o.readable.push_back(s.has(x.id).value_or(false));
  }
  o.open = s.open_intents();
  return o;
}

observed observe(const log_backend &s, const std::vector<object> &objects) {
  observed o;
  o.seq = s.last_seq();
  o.refs = s.list_refs().value_or(std::vector<ref_entry>{});
  for (const object &x : objects) {
    const auto got = s.get(x.id);
    o.readable.push_back(got.has_value() && *got == x);
  }
  o.open = s.open_intents();
  return o;
}

// Three commits; the third has a loose blob (with loose_above = 64), a ref move, a ref creation
// and an intent end.
struct workload {
  object a = blob("alpha");
  object b = blob("beta");
  object c = blob("gamma");
  object d = big_blob(300, 7); // loose
  object e = blob("epsilon");

  [[nodiscard]] std::vector<object> all() const { return {a, b, c, d, e}; }

  [[nodiscard]] commit_batch first() const {
    commit_batch x{.objects = {a, b}, .refs = {}, .intents = {}, .commit_time_us = 1};
    x.refs.push_back(ref("main", std::nullopt, a.id));
    return x;
  }
  [[nodiscard]] commit_batch second() const {
    commit_batch x{.objects = {c}, .refs = {}, .intents = {}, .commit_time_us = 2};
    x.refs.push_back(ref("main", a.id, c.id));
    x.intents.push_back(intent_record{.operation = op(1),
                                      .kind = "import",
                                      .phase = intent_phase::begin,
                                      .outcome = intent_outcome::none});
    return x;
  }
  [[nodiscard]] commit_batch third() const {
    commit_batch x{.objects = {d, e, a}, .refs = {}, .intents = {}, .commit_time_us = 3};
    x.refs.push_back(ref("main", c.id, e.id));
    x.refs.push_back(ref("side", std::nullopt, d.id));
    x.intents.push_back(intent_record{.operation = op(1),
                                      .kind = "import",
                                      .phase = intent_phase::end,
                                      .outcome = intent_outcome::completed});
    return x;
  }
};

constexpr log_options crash_options{
    .sync = durability::fsync, .rotate_at = sl::fabric::segment_rotate_size, .loose_above = 64};

// A faulty vfs over memory with the workspace parent made durable.
std::unique_ptr<faulty_vfs> fresh_vfs() {
  auto fs = std::make_unique<faulty_vfs>();
  REQUIRE(fs->make_dir("ws").has_value());
  REQUIRE(fs->sync_dir(".").has_value());
  return fs;
}

} // namespace

TEST_CASE("FAB-INV-STO-06: the backend contract suite passes on log_backend over the posix vfs") {
  const scratch_dir dir("logstore_contract_test");
  posix_factory factory{.dir = &dir, .fs = {}, .made = 0};
  sl::fabric::testing::store_contract(factory);
}

TEST_CASE("FAB-INV-STO-06: the backend contract suite passes on log_backend over the faulty vfs") {
  faulty_factory factory{.options = {}, .fs = {}};
  sl::fabric::testing::store_contract(factory);
}

TEST_CASE("FAB-INV-STO-06: the contract suite passes with loose blobs and 1 KiB segments") {
  faulty_factory factory{
      .options = log_options{.sync = durability::fsync, .rotate_at = 1024, .loose_above = 16},
      .fs = {}};
  sl::fabric::testing::store_contract(factory);
}

TEST_CASE("FAB-INV-STO-06: a reopened posix log serves every acknowledged commit") {
  const scratch_dir dir("logstore_contract_test_reopen");
  sl::fabric::vfs_posix fs;
  const std::filesystem::path slf = dir.path() / ".slfabric";
  const object big = big_blob((std::size_t{3} << 19U), 3); // 1.5 MiB: loose by default
  const object small = blob("small");
  observed before;
  {
    const auto store = open_store(fs, slf);
    commit_batch x{.objects = {big, small}, .refs = {}, .intents = {}, .commit_time_us = 5};
    x.refs.push_back(ref("main", std::nullopt, small.id));
    x.refs.push_back(ref("assets/big", std::nullopt, big.id));
    x.intents.push_back(intent_record{.operation = op(4),
                                      .kind = "bake",
                                      .phase = intent_phase::begin,
                                      .outcome = intent_outcome::none});
    REQUIRE(backdoor::commit(*store, std::move(x)).has_value());
    before = observe(*store, {big, small});
    CHECK(before.readable == std::vector<bool>{true, true});
  }
  const std::string hex = big.id.to_hex();
  CHECK(fs.exists(slf / "loose" / hex.substr(0, 2) / hex).value_or(false));
  CHECK(sl::fabric::read_file(fs, slf / "FORMAT").value_or(std::vector<std::uint8_t>{}) ==
        std::vector<std::uint8_t>(sl::fabric::format_text.begin(), sl::fabric::format_text.end()));
  const auto store = open_store(fs, slf);
  CHECK(observe(*store, {big, small}) == before);
  CHECK(store->open_intents() == std::vector<record_id>{op(4)});
  CHECK(store->object_count() == 2);
  const auto report = store->verify(sl::fabric::verify_scope{});
  REQUIRE(report.has_value());
  CHECK(report->ok());
  CHECK(report->objects_hashed == 2);
}

TEST_CASE("FAB-INV-STO-06: one commit is one append and one fsync (ARCH2-R33)") {
  const object a = blob("a");
  const object b = blob("b");
  const auto trace_of = [&](durability d, std::size_t loose_above) {
    const auto fs = fresh_vfs();
    const auto store =
        open_store(*fs, store_path(),
                   log_options{.sync = d, .rotate_at = 1U << 20U, .loose_above = loose_above});
    fs->clear_trace();
    commit_batch x{.objects = {a, b}, .refs = {}, .intents = {}, .commit_time_us = 0};
    REQUIRE(backdoor::commit(*store, std::move(x)).has_value());
    return fs->trace();
  };
  const auto fsynced = trace_of(durability::fsync, 1U << 20U);
  REQUIRE(fsynced.size() == 2);
  CHECK(fsynced[0].starts_with("append seg-00000001.slg +"));
  CHECK(fsynced[1] == "fsync seg-00000001.slg");
  CHECK(trace_of(durability::flush, 1U << 20U) ==
        std::vector<std::string>{fsynced[0], "fdatasync seg-00000001.slg"});
  CHECK(trace_of(durability::none, 1U << 20U) == std::vector<std::string>{fsynced[0]});

  // A loose blob is written, synced and renamed into place before the frames (ADR-0015).
  const auto loose = trace_of(durability::fsync, 4);
  const std::string ha = a.id.to_hex();
  const std::string hb = b.id.to_hex();
  const std::vector<std::string> expect = {
      "exists " + ha.substr(0, 2), "make_dir " + ha.substr(0, 2), "sync_dir loose",
      "open " + ha + ".tmp wct",   "append " + ha + ".tmp +10",   "fsync " + ha + ".tmp",
      "close " + ha + ".tmp",      "rename " + ha + ".tmp " + ha, "sync_dir " + ha.substr(0, 2)};
  REQUIRE(loose.size() > expect.size());
  for (std::size_t i = 0; i < expect.size(); ++i) {
    CHECK(loose[i] == expect[i]);
  }
  CHECK(loose[loose.size() - 2].starts_with("append seg-00000001.slg +"));
  CHECK(loose.back() == "fsync seg-00000001.slg");
  CHECK(std::ranges::count_if(
            loose, [](const std::string &l) { return l.starts_with("fsync seg"); }) == 1);
  const std::string rename_b = "rename " + hb + ".tmp " + hb;
  CHECK(std::ranges::count(loose, rename_b) == 1);
}

TEST_CASE("FAB-INV-STO-06: a crash at every primitive of a commit loses no acknowledged commit") {
  const workload w;
  const std::vector<object> ids = w.all();

  // Reference states and the primitive count of the third commit, from a clean run.
  observed s2;
  observed s3;
  std::uint64_t primitives = 0;
  {
    const auto fs = fresh_vfs();
    const auto store = open_store(*fs, store_path(), crash_options);
    REQUIRE(backdoor::commit(*store, w.first()).has_value());
    REQUIRE(backdoor::commit(*store, w.second()).has_value());
    s2 = observe(*store, ids);
    const std::uint64_t before = fs->primitives();
    REQUIRE(backdoor::commit(*store, w.third()).has_value());
    primitives = fs->primitives() - before;
    s3 = observe(*store, ids);
  }
  REQUIRE(primitives >= 10); // loose: exists, make_dir, sync_dir, open, append, fsync, rename,
                             // sync_dir; then the segment append and its fsync
  CHECK(s2.seq == 2);
  CHECK(s3.seq == 3);
  CHECK(s3.readable == std::vector<bool>{true, true, true, true, true});
  CHECK(s3.open.empty());
  CHECK(s2.open == std::vector<record_id>{op(1)});

  struct policy_case {
    crash_policy policy;
    std::uint64_t tear = 0;
  };
  const std::vector<policy_case> policies = {
      {.policy = crash_policy::keep_unsynced, .tear = 0},
      {.policy = crash_policy::drop_unsynced, .tear = 0},
      {.policy = crash_policy::tear_last_append, .tear = 0},
      {.policy = crash_policy::tear_last_append, .tear = 1},
      {.policy = crash_policy::tear_last_append, .tear = 13},
      {.policy = crash_policy::tear_last_append, .tear = 100},
      {.policy = crash_policy::tear_last_append, .tear = 1U << 20U},
  };
  const std::uint64_t segment_fsync = primitives - 1; // the last primitive of the commit

  for (std::uint64_t k = 0; k <= primitives; ++k) {
    for (const policy_case &pc : policies) {
      CAPTURE(k);
      CAPTURE(static_cast<int>(pc.policy));
      CAPTURE(pc.tear);
      const auto fs = fresh_vfs();
      auto store = open_store(*fs, store_path(), crash_options);
      REQUIRE(backdoor::commit(*store, w.first()).has_value());
      REQUIRE(backdoor::commit(*store, w.second()).has_value());
      fs->fail_after(k);
      const auto r = backdoor::commit(*store, w.third());
      std::uint64_t torn_length = 0;
      if (k == primitives) { // FP5: the fsync returned, the commit is acknowledged
        REQUIRE(r.has_value());
        CHECK(r->seq == 3);
        CHECK(observe_index(*store, ids) == s3); // the next primitive (a read) would fail
      } else {
        REQUIRE_FALSE(r.has_value());
        CHECK(observe_index(*store, ids) == s2); // nothing is published before the commit point
        if (k == segment_fsync) {                // FP4: written, the fsync failed
          CHECK(r.error().code == errc::commit_indeterminate);
          CHECK(store->needs_recovery());
          CHECK(backdoor::commit(*store, w.third()).error().code == errc::recovery_required);
          const auto trace = fs->trace();
          const std::string &append = trace[trace.size() - 2];
          REQUIRE(append.starts_with("append seg-00000001.slg +"));
          torn_length = std::stoull(append.substr(append.find('+') + 1));
        } else {
          CHECK(r.error().code == errc::store_io);
        }
      }
      REQUIRE(fs->crash(pc.policy, pc.tear).has_value());
      store.reset();

      // What reopening must find, decided by frame intactness alone.
      enum class expect : std::uint8_t { before, after, torn };
      expect want = expect::before;
      if (k == primitives) {
        want = expect::after; // acknowledged: survives every crash
      } else if (k == segment_fsync) {
        if (pc.policy == crash_policy::keep_unsynced) {
          want = expect::after;
        } else if (pc.policy == crash_policy::tear_last_append) {
          // FP3: a commit write torn inside its bytes leaves a torn tail.
          if (pc.tear >= torn_length) {
            want = expect::after;
          } else if (pc.tear > 0) {
            want = expect::torn;
          }
        }
      }
      const auto reopened = log_backend::open(*fs, store_path(), crash_options);
      if (want == expect::torn) {
        REQUIRE_FALSE(reopened.has_value());
        CHECK(reopened.error().code == errc::recovery_required);
        continue;
      }
      if (!reopened) {
        FAIL("reopen: " << reopened.error().message);
      }
      CHECK(observe(**reopened, ids) == (want == expect::after ? s3 : s2));
      if (want == expect::before) { // the retry commits whole
        REQUIRE(backdoor::commit(**reopened, w.third()).value_or(commit_ack{}).seq == 3);
        CHECK(observe(**reopened, ids) == s3);
      }
    }
  }
}

TEST_CASE("FAB-INV-STO-06: commit_indeterminate blocks commits and reopen decides by the frame") {
  const workload w;
  const auto fs = fresh_vfs();
  auto store = open_store(*fs, store_path(), crash_options);
  REQUIRE(backdoor::commit(*store, w.first()).has_value());
  fs->fail_after(1); // the append passes, the fsync fails
  commit_batch x{.objects = {w.c}, .refs = {}, .intents = {}, .commit_time_us = 9};
  const auto r = backdoor::commit(*store, x);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == errc::commit_indeterminate);
  fs->disarm();
  REQUIRE(fs->crash(crash_policy::keep_unsynced).has_value());
  CHECK(backdoor::commit(*store, x).error().code == errc::recovery_required);
  CHECK_FALSE(store->has(w.c.id).value_or(true));
  store.reset();
  const auto reopened = open_store(*fs, store_path(), crash_options);
  CHECK(reopened->last_seq() == 2); // the intact frame decided: committed
  CHECK(reopened->get(w.c.id).value_or(object{}) == w.c);
}

TEST_CASE("FAB-INV-STO-06: open refuses a dirty tail, a newer format and a missing loose blob") {
  const workload w;
  const auto make = [&w] {
    auto fs = fresh_vfs();
    const auto store = open_store(*fs, store_path(), crash_options);
    REQUIRE(backdoor::commit(*store, w.first()).has_value());
    REQUIRE(backdoor::commit(*store, w.second()).has_value());
    REQUIRE(backdoor::commit(*store, w.third()).has_value());
    return fs;
  };
  const auto append_raw = [](sl::fabric::vfs &fs, const std::vector<std::uint8_t> &bytes) {
    auto f = fs.open(segment1, sl::fabric::open_append);
    REQUIRE(f.has_value());
    REQUIRE(f->append(bytes).has_value());
  };
  const auto write_format = [](sl::fabric::vfs &fs, std::string_view text) {
    auto f = fs.open(store_path() / "FORMAT", sl::fabric::open_create);
    REQUIRE(f.has_value());
    REQUIRE(f->append(std::vector<std::uint8_t>(text.begin(), text.end())).has_value());
  };
  const auto code = [](sl::fabric::vfs &fs) {
    const auto r = log_backend::open(fs, store_path(), crash_options);
    return r ? errc::ok : r.error().code;
  };

  SUBCASE("a torn frame") {
    const auto fs = make();
    const object x = blob("torn");
    const auto f = sl::fabric::encode_frame(sl::fabric::frame_type::object,
                                            sl::fabric::frame_object(x.header, x.payload));
    append_raw(*fs, std::vector<std::uint8_t>(f->begin(), f->begin() + 12));
    const auto r = log_backend::open(*fs, store_path(), crash_options);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == errc::recovery_required);
    CHECK(r.error().message.find("torn tail") != std::string::npos);
  }
  SUBCASE("orphan frames after the last commit") {
    const auto fs = make();
    const object x = blob("orphan");
    append_raw(*fs, sl::fabric::encode_frame(sl::fabric::frame_type::object,
                                             sl::fabric::frame_object(x.header, x.payload))
                        .value());
    const auto r = log_backend::open(*fs, store_path(), crash_options);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == errc::recovery_required);
    CHECK(r.error().message.find("follow the last commit (seq 3)") != std::string::npos);
  }
  SUBCASE("FORMAT versions") {
    const auto fs = make();
    write_format(*fs, "slfabric 2.0\n");
    CHECK(code(*fs) == errc::format_unsupported);
    write_format(*fs, "slfabric 1.1\n");
    CHECK(code(*fs) == errc::read_only_newer);
    write_format(*fs, "not a store\n");
    CHECK(code(*fs) == errc::store_corrupt);
    write_format(*fs, "slfabric 1.0");
    CHECK(code(*fs) == errc::ok);
  }
  SUBCASE("a missing or damaged loose blob") {
    const auto fs = make();
    const std::string hex = w.d.id.to_hex();
    const std::filesystem::path p = store_path() / "loose" / hex.substr(0, 2) / hex;
    fs->flip_bit(p, 40, 3);
    CHECK(code(*fs) == errc::journal_corrupt);
    fs->clear_flips();
    CHECK(code(*fs) == errc::ok);
    REQUIRE(fs->remove(p).has_value());
    CHECK(code(*fs) == errc::journal_corrupt);
  }
  SUBCASE("no FORMAT") {
    const auto fs = make();
    REQUIRE(fs->remove(store_path() / "FORMAT").has_value());
    CHECK(code(*fs) == errc::recovery_required); // never re-created over data
  }
  SUBCASE("a creation that stopped before FORMAT is redone") {
    const auto fs = fresh_vfs();
    fs->fail_after(9); // dirs and segment 1 exist, FORMAT does not
    CHECK_FALSE(log_backend::open(*fs, store_path(), crash_options).has_value());
    REQUIRE(fs->crash(crash_policy::keep_unsynced).has_value());
    CHECK_FALSE(fs->exists(store_path() / "FORMAT").value_or(true));
    const auto store = open_store(*fs, store_path(), crash_options);
    CHECK(store->last_seq() == 0);
    REQUIRE(backdoor::commit(*store, w.first()).has_value());
  }
}

TEST_CASE("FAB-INV-STO-06: get verifies stored bytes; a flipped bit is object_corrupt") {
  const auto fs = fresh_vfs();
  const object a = blob("verified on every read");
  const auto store = open_store(*fs, store_path());
  commit_batch x{.objects = {a}, .refs = {}, .intents = {}, .commit_time_us = 0};
  x.refs.push_back(ref("main", std::nullopt, a.id));
  REQUIRE(backdoor::commit(*store, std::move(x)).has_value());
  CHECK(store->get(a.id).value_or(object{}) == a);
  // segment header (24) + frame prefix (9) + SLFO header (9): the first payload byte.
  fs->flip_bit(segment1, 24 + 9 + 9, 0);
  const auto got = store->get(a.id);
  REQUIRE_FALSE(got.has_value());
  CHECK(got.error().code == errc::object_corrupt);
  const auto report = store->verify(sl::fabric::verify_scope{});
  REQUIRE(report.has_value());
  REQUIRE(report->findings.size() == 1);
  CHECK(report->findings[0].code == errc::object_corrupt);
  CHECK(report->findings[0].id == a.id);
}
