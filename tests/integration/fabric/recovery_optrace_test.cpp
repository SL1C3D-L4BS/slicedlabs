// FAB-INV-RCV-11 (SL-ARCH-002 ARCH2-R07 commit steps, ARCH2-R33, ADR-0015): one commit's storage
// primitive trace on the posix vfs equals the golden protocol order. A durable workspace lives in a
// scratch directory under the ctest working directory, on a faulty vfs (which only records the
// trace) over vfs_posix. A commit is one append of its object, intent and commit frames to the
// last segment followed by exactly one fsync of that segment (the commit point, ARCH2-R33); a
// commit with an object above the loose threshold first writes the blob to loose/ab/<oid>.tmp,
// syncs it, renames it into place and syncs its directory (after making and syncing the directory
// the first time); a commit that would grow the segment past rotate_at first creates the next
// segment, writes and syncs its header and syncs log/. The traces are compared primitive by
// primitive with file names (byte counts, offsets and object ids dropped); reads, sizes,
// listings, read-only opens and closes change nothing and are left out.

#include "sl/core/error.hpp"
#include "sl/fabric/vfs.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/crash_harness.hpp"
#include "../../../engine/fabric/testing/faulty_vfs.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;
namespace tg = sl::fabric::testing;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

// "<primitive> <file>" of every mutating primitive (reads, sizes, listings and closes dropped).
std::vector<std::string> protocol(const std::vector<std::string> &trace) {
  std::vector<std::string> out;
  for (const std::string &line : trace) {
    const std::string_view l(line);
    const std::string_view verb = l.substr(0, l.find(' '));
    if (verb == "read" || verb == "size" || verb == "list" || verb == "exists" || verb == "close" ||
        (verb == "open" && l.ends_with(" r"))) {
      continue;
    }
    const std::string_view rest = l.substr(std::min(l.size(), verb.size() + 1));
    std::string file(rest.substr(0, rest.find(' ')));
    if (file.size() == 68 && file.ends_with(".tmp")) {
      file = "<oid>.tmp";
    } else if (file.size() == 64) {
      file = "<oid>";
    } else if (file.size() == 2 && verb != "open") {
      file = "<ab>";
    }
    std::string entry(verb);
    entry += ' ';
    entry += file;
    if (verb == "rename") {
      const std::string_view to = rest.substr(rest.find(' ') + 1);
      entry += ' ';
      entry += to.size() == 64 ? std::string("<oid>") : std::string(to);
    }
    out.push_back(std::move(entry));
  }
  return out;
}

// The golden order of one commit (log_backend::commit): every loose blob (its directory made and
// synced in loose/ the first time, then tmp, sync, rename, directory sync), then the next segment
// when rotating (create, header, sync, log/ sync), then the one append of the frames and the one
// sync of that segment. Returns "" when `p` follows it, else what broke the order.
std::string check_order(const std::vector<std::string> &p, std::size_t &loose, bool &rotated) {
  std::size_t i = 0;
  const auto at = [&p, &i](std::string_view want) { return i < p.size() && p[i] == want; };
  while (i < p.size() && (p[i] == "make_dir <ab>" || p[i] == "open <oid>.tmp")) {
    if (at("make_dir <ab>")) {
      ++i;
      if (!at("sync_dir loose")) {
        return "a new loose directory is not synced in loose/";
      }
      ++i;
    }
    for (const std::string_view step : {"open <oid>.tmp", "append <oid>.tmp", "fsync <oid>.tmp",
                                        "rename <oid>.tmp <oid>", "sync_dir <ab>"}) {
      if (!at(step)) {
        return "loose blob step " + std::string(step) + " missing at " + std::to_string(i);
      }
      ++i;
    }
    ++loose;
  }
  if (i < p.size() && p[i].starts_with("open seg-")) {
    const std::string seg = p[i].substr(5);
    for (const std::string &step :
         {"open " + seg, "append " + seg, "fsync " + seg, std::string("sync_dir log")}) {
      if (!at(step)) {
        return "rotation step " + step + " missing at " + std::to_string(i);
      }
      ++i;
    }
    rotated = true;
  }
  if (i + 2 != p.size() || !p[i].starts_with("append seg-") ||
      p[i + 1] != "fsync " + p[i].substr(7)) {
    return "the commit does not end with one append and one fsync of the segment";
  }
  return {};
}

std::string joined(const std::vector<std::string> &v) {
  std::string s;
  for (const std::string &x : v) {
    s += x;
    s += '\n';
  }
  return s;
}

} // namespace

TEST_CASE("FAB-INV-RCV-11: one commit's storage primitive trace on the posix vfs equals the "
          "golden protocol order") {
  tg::scratch_dir scratch("fabric_recovery_optrace");
  REQUIRE(scratch.ok());
  auto fs = std::make_shared<tg::faulty_vfs>(std::make_shared<vfs_posix>());
  tg::harness_config cfg;
  cfg.log.loose_above = 1U << 20U;
  const tg::crash_workload w = must(tg::make_workload(3, 3, 2));
  const auto root = scratch.path() / "ws";
  auto ws = must(workspace::create(root, must(tg::harness_options(fs, cfg))));
  for (std::size_t i = 0; i < w.patches.size(); ++i) {
    fs->clear_trace();
    auto t = must(ws->manager().begin(tg::harness_tx()));
    REQUIRE(t.stage(w.patches[i]).has_value());
    REQUIRE(must(t.commit()).revision == w.heads[i + 1]);
    CHECK(joined(protocol(fs->trace())) ==
          joined({"append seg-00000001.slg", "fsync seg-00000001.slg"}));
  }
  ws.reset();
  // Loose blobs and rotation.
  auto fs2 = std::make_shared<tg::faulty_vfs>(std::make_shared<vfs_posix>());
  cfg.log.loose_above = 300;
  cfg.log.rotate_at = 2048;
  const tg::crash_workload w2 = must(tg::make_workload(9, 6, 3));
  std::size_t loose = 0;
  std::size_t rotations = 0;
  bool rotated = false;
  auto ws2 = must(workspace::create(scratch.path() / "ws2", must(tg::harness_options(fs2, cfg))));
  for (std::size_t i = 0; i < w2.patches.size(); ++i) {
    fs2->clear_trace();
    auto t = must(ws2->manager().begin(tg::harness_tx()));
    REQUIRE(t.stage(w2.patches[i]).has_value());
    REQUIRE(must(t.commit()).revision == w2.heads[i + 1]);
    CAPTURE(joined(fs2->trace()));
    const std::string broken = check_order(protocol(fs2->trace()), loose, rotated);
    CHECK_MESSAGE(broken.empty(), broken);
    ++rotations;
    rotations -= rotated ? 0U : 1U;
    rotated = false;
  }
  CHECK(loose > 0);
  CHECK(rotations > 0);
}
