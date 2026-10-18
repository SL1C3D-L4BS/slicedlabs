// sl_fabric_fsck (WO-059, SL-ARCH-002 ARCH2-R44, FAB-INV-COR-06): the read-only check of a
// durable store.
//
//   sl_fabric_fsck [--objects] [--no-lock-probe] [--no-records] <dir>
//
// `<dir>` is a workspace directory or its `.slfabric`. Runs fsck() with the fabric.manifest types
// registered (records of other types are counted as unjudged) and prints one line per finding
// (finding::line(): the class, then `oid <hex>` or `<file> offset <n>`, then the evidence), then a
// summary line. --objects first lists every intact object:
//
//   object <oid> <file> frame <offset> payload <offset> length <n>
//
// Exit 0 when there is no finding, 1 when there is one, 2 on bad usage or when the store cannot
// be read (no store, an I/O error).

#include <cstddef>
#include <cstdio>
#include <exception>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>

#include "sl/core/error.hpp"
#include "sl/fabric/corruption.hpp"
#include "sl/fabric/fsck.hpp"
#include "sl/fabric/manifest.hpp"
#include "sl/fabric/vfs.hpp"
#include "sl/fabric/workspace.hpp"

namespace {

using namespace sl::fabric;

int usage() {
  std::fputs("usage: sl_fabric_fsck [--objects] [--no-lock-probe] [--no-records] <dir>\n", stderr);
  return 2;
}

int run(std::span<char *> args) {
  bool objects = false;
  fsck_options options;
  std::string dir;
  for (std::size_t i = 1; i < args.size(); ++i) {
    const std::string_view a = args[i];
    if (a == "--objects") {
      objects = true;
    } else if (a == "--no-lock-probe") {
      options.probe_lock = false;
    } else if (a == "--no-records") {
      options.records = false;
    } else if (!a.empty() && a.front() != '-' && dir.empty()) {
      dir = a;
    } else {
      return usage();
    }
  }
  if (dir.empty()) {
    return usage();
  }
  workspace_options types;
  if (auto ok = prepare_manifest_options(types); !ok) {
    std::println(stderr, "sl_fabric_fsck: schemas: {}", ok.error().message);
    return 2;
  }
  if (auto ok = types.schemas.freeze(); !ok) {
    std::println(stderr, "sl_fabric_fsck: schemas: {}", ok.error().message);
    return 2;
  }
  options.schemas = &types.schemas;
  vfs_posix fs;
  const auto report = fsck(fs, dir, options);
  if (!report) {
    std::println(stderr, "sl_fabric_fsck: {}: {}", dir, report.error().message);
    return 2;
  }
  if (objects) {
    for (const fsck_object &o : report->objects) {
      std::println("object {} {} frame {} payload {} length {}", o.id.to_hex(), o.file, o.frame,
                   o.offset, o.length);
    }
  }
  for (const finding &f : report->findings) {
    std::println("{}", f.line());
  }
  std::println("sl_fabric_fsck: {}: {} findings; {} segments, {} frames, {} objects, {} commits, "
               "{} objects reached, {} records checked, {} unjudged{}{}",
               dir, report->findings.size(), report->segments, report->frames,
               report->objects.size(), report->commits.size(), report->objects_reached,
               report->records_checked, report->records_unjudged,
               report->read_only_newer ? ", newer minor (read-only)" : "",
               report->live_writer ? ", a live writer holds the lock" : "");
  return report->clean() ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
  const std::span<char *> args(argv, static_cast<std::size_t>(argc));
  try {
    return run(args);
  } catch (const std::exception &e) {
    std::fputs("sl_fabric_fsck: ", stderr);
    std::fputs(e.what(), stderr);
    std::fputs("\n", stderr);
  } catch (...) {
    std::fputs("sl_fabric_fsck: unknown error\n", stderr);
  }
  return 2;
}
