// sl_fabric_mkstore (WO-059): builds a deterministic sample store for `just fabric-fsck`,
// `neg-fabric-fsck` and by hand.
//
//   sl_fabric_mkstore [--edits N] <dir>
//
// Creates the project `<dir>` (create_project: project.slp and `.slfabric`) with a fixed clock, a
// fixed id stream and one human creator, then commits N (default 4, at most 1000) manifest edits
// renaming the project, each a transaction of subsystem 02. The same N gives the same bytes. The
// workspace is closed before exit (the lock file is left empty). Exit 0 on success, 1 when the
// store cannot be built (e.g. `<dir>` already holds a project), 2 on bad usage.

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include "sl/core/error.hpp"
#include "sl/fabric/authority.hpp"
#include "sl/fabric/clock.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/manifest.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/workspace.hpp"

namespace {

using namespace sl::fabric;

constexpr std::uint32_t max_edits = 1000;

record_id fixture_id(id_class c, std::string_view key) noexcept {
  return derive(c, well_known(id_class::namespace_, 1), key);
}

actor_id creator() noexcept {
  return actor_id::from(fixture_id(id_class::actor, "mkstore/creator")).value_or(actor_id{});
}

int usage() {
  std::fputs("usage: sl_fabric_mkstore [--edits N] <dir>\n", stderr);
  return 2;
}

int fail(const std::string &what, const sl::error &e) {
  std::println(stderr, "sl_fabric_mkstore: {}: {}", what, e.message);
  return 1;
}

int run(std::span<char *> args) {
  std::uint32_t edits = 4;
  std::string dir;
  for (std::size_t i = 1; i < args.size(); ++i) {
    const std::string_view a = args[i];
    if (a == "--edits" && i + 1 < args.size()) {
      const std::string_view n = args[++i];
      const auto [end, ec] = std::from_chars(n.data(), n.data() + n.size(), edits);
      if (ec != std::errc{} || end != n.data() + n.size() || edits > max_edits) {
        return usage();
      }
    } else if (!a.empty() && a.front() != '-' && dir.empty()) {
      dir = a;
    } else {
      return usage();
    }
  }
  if (dir.empty()) {
    return usage();
  }

  if (const std::filesystem::path parent = std::filesystem::path(dir).parent_path();
      !parent.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      std::println(stderr, "sl_fabric_mkstore: {}: {}", parent.string(), ec.message());
      return 1;
    }
  }

  workspace_options options;
  if (auto ok = options.actors.add(actor{.id = creator(), .kind = actor_kind::human, .grants = {}});
      !ok) {
    return fail("actors", ok.error());
  }
  const auto clock = std::make_shared<manual_clock>(1'700'000'000'000'000);
  options.clock = clock;
  options.ids = std::make_shared<deterministic_source>(59, "sl_fabric_mkstore");

  project_manifest m;
  m.project = fixture_id(id_class::project, "mkstore/project");
  m.name = "fsck sample 0";
  m.default_branch = "main";
  auto ws = create_project(dir, m, engine_profile{}, creator(), std::move(options));
  if (!ws) {
    return fail("create " + dir, ws.error());
  }
  for (std::uint32_t i = 1; i <= edits; ++i) {
    clock->advance_us(1'000'000);
    auto head = (*ws)->manager().head("main");
    if (!head) {
      return fail("head", head.error());
    }
    auto current = read_manifest(*head);
    if (!current) {
      return fail("read manifest", current.error());
    }
    current->name = "fsck sample " + std::to_string(i);
    auto op = manifest_set_op(*current);
    if (!op) {
      return fail("manifest edit", op.error());
    }
    tx_options o;
    o.origin.actor = creator();
    o.origin.subsystem = "02";
    o.meta.label = "rename " + std::to_string(i);
    auto t = (*ws)->manager().begin(o);
    if (!t) {
      return fail("begin", t.error());
    }
    if (auto ok = t->stage(*op); !ok) {
      return fail("stage", ok.error());
    }
    if (auto r = t->commit(); !r) {
      return fail("commit " + std::to_string(i), r.error());
    }
  }
  ws->reset();
  std::println("sl_fabric_mkstore: {}: project with {} manifest edits", dir, edits);
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  const std::span<char *> args(argv, static_cast<std::size_t>(argc));
  try {
    return run(args);
  } catch (const std::exception &e) {
    std::fputs("sl_fabric_mkstore: ", stderr);
    std::fputs(e.what(), stderr);
    std::fputs("\n", stderr);
  } catch (...) {
    std::fputs("sl_fabric_mkstore: unknown error\n", stderr);
  }
  return 2;
}
