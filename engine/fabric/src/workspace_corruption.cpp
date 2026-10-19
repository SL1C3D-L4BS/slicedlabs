// Strict and lenient opens (WO-059, SL-ARCH-002 ARCH2-R43, `corruption` C1-C12): open_checked
// runs fsck first, then opens the head through workspace::open or a read-only recovered view over
// the intact objects. See corruption.hpp.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/corruption.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/fsck.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/recovery.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/vfs.hpp"
#include "sl/fabric/workspace.hpp"

namespace sl::fabric {

namespace {

namespace stdfs = std::filesystem;

template <class T> std::unexpected<error> pass(const result<T> &r) {
  return std::unexpected(r.error());
}

constexpr std::size_t refusal_lines = 8;

std::string refusal(const std::vector<finding> &findings) {
  std::string text = std::to_string(findings.size()) + " corruption findings";
  for (std::size_t i = 0; i < findings.size() && i < refusal_lines; ++i) {
    text += "; " + findings[i].line();
  }
  if (findings.size() > refusal_lines) {
    text += "; ...";
  }
  return text;
}

bool flags_record(corruption_class c) noexcept {
  return c == corruption_class::c7 || c == corruption_class::c9 || c == corruption_class::c10;
}

// What a lenient open can open for write: recovery heals C1, C11 and C12; C9 is flagged.
bool opens_for_write(corruption_class c) noexcept {
  return c == corruption_class::c1 || c == corruption_class::c9 || c == corruption_class::c11 ||
         c == corruption_class::c12;
}

std::vector<record_id> flagged_of(const std::vector<finding> &findings) {
  std::set<record_id> out;
  for (const finding &f : findings) {
    if (flags_record(f.cls) && f.where.record) {
      out.insert(*f.where.record);
    }
  }
  return {out.begin(), out.end()};
}

std::string file_name_of(const std::string &rel) {
  const std::size_t slash = rel.rfind('/');
  return slash == std::string::npos ? rel : rel.substr(slash + 1);
}

// Copies the damaged bytes of each quarantining finding into quarantine/ (never deletes).
result<std::vector<std::string>> quarantine_findings(vfs &fs, const stdfs::path &store,
                                                     const fsck_report &report, std::uint64_t seq) {
  std::vector<std::string> out;
  std::set<std::string> done;
  const auto copy = [&](const quarantine_item &item) -> result<void> {
    if (!done.insert(item.name).second) {
      return {};
    }
    result<std::string> q = quarantine(fs, item);
    if (!q) {
      if (q.error().code == errc::not_found) {
        return {};
      }
      return pass(q);
    }
    out.push_back(std::move(*q));
    return {};
  };
  for (const finding &f : report.findings) {
    if (!policy_of(f.cls).quarantines && f.cls != corruption_class::c1) {
      continue;
    }
    quarantine_item item;
    item.store_dir = store;
    if ((f.where.file.starts_with("log/") && f.where.segment != 0) ||
        f.where.file.starts_with("checkpoint/")) {
      item.source = f.where.file;
      item.name = std::to_string(seq) + "-" + file_name_of(f.where.file);
    } else if (f.where.file.starts_with("loose/") && f.where.oid) {
      item.source = f.where.file;
      item.name = f.where.oid->to_hex() + "-loose";
    } else if (f.where.file.empty() && f.where.oid) {
      const auto it = std::ranges::find(report.objects, *f.where.oid, &fsck_object::id);
      if (it == report.objects.end()) {
        continue; // missing: nothing to keep
      }
      item.source = it->file;
      item.name = f.where.oid->to_hex() + "-object";
      if (!it->loose) {
        item.offset = it->offset;
        item.length = it->length;
      }
    } else {
      continue;
    }
    if (result<void> ok = copy(item); !ok) {
      return pass(ok);
    }
  }
  return out;
}

result<checked_open> open_view(const std::shared_ptr<vfs> &fs, const stdfs::path &root,
                               const fsck_report &report, const schema_registry *schemas,
                               workspace_options options, const checked_open_options &how) {
  const stdfs::path store = root / workspace::store_dir_name;
  const auto names_ref = [&](const fsck_commit &c) {
    return std::ranges::any_of(c.refs, [&](const ref_update &r) { return r.name == how.ref; });
  };
  result<std::shared_ptr<store_backend>> all = open_salvage_store(fs, store, report, {});
  if (!all) {
    return pass(all);
  }
  const fsck_commit *chosen = nullptr;
  for (const fsck_commit &c : std::views::reverse(report.commits)) {
    if (!c.intact_prefix || !names_ref(c)) {
      continue;
    }
    const result<bool> ok = state_closure_verifies(**all, c.revision);
    if (!ok) {
      return pass(ok);
    }
    if (*ok) {
      chosen = &c;
      break;
    }
  }
  if (chosen == nullptr) {
    return make_error(errc::store_corrupt, "lenient open " + root.string() + ": no revision of '" +
                                               how.ref + "' verifies; " + refusal(report.findings));
  }
  const digest revision = chosen->revision;
  checked_open out;
  out.findings = report.findings;
  out.recovered_view = true;
  out.recovered_ref = "recovered/" + std::to_string(chosen->seq);
  out.recovered_revision = revision;

  result<std::vector<std::string>> q = quarantine_findings(*fs, store, report, chosen->seq);
  if (!q) {
    return pass(q);
  }
  out.quarantined = std::move(*q);

  std::vector<digest> lost;
  for (auto it = report.commits.rbegin(); it != report.commits.rend() && &*it != chosen; ++it) {
    for (const ref_update &r : it->refs) {
      if (r.name == how.ref && r.desired != revision) {
        lost.push_back(r.desired);
      }
    }
  }
  result<loss_report> loss = compute_loss(**all, revision, lost);
  if (!loss) {
    return pass(loss);
  }
  out.loss = std::move(*loss);

  result<std::vector<finding>> view_findings = check_records(**all, revision, schemas);
  if (!view_findings) {
    return pass(view_findings);
  }
  out.flagged = flagged_of(*view_findings);

  result<std::shared_ptr<store_backend>> view = open_salvage_store(
      fs, store, report, {ref_entry{.name = out.recovered_ref, .target = revision}});
  if (!view) {
    return pass(view);
  }
  result<std::unique_ptr<workspace>> ws =
      workspace::create_with(std::move(*view), std::move(options), open_mode::read_only);
  if (!ws) {
    return pass(ws);
  }
  out.ws = std::move(*ws);
  return out;
}

} // namespace

result<checked_open> open_checked(const std::filesystem::path &root, corruption_mode mode,
                                  const recovery_policy &policy, workspace_options options,
                                  const checked_open_options &how) {
  std::shared_ptr<vfs> fs = options.fs ? options.fs : std::make_shared<vfs_posix>();
  schema_registry schemas = options.schemas;
  if (result<void> ok = schemas.freeze(); !ok) {
    return pass(ok);
  }
  const result<fsck_report> report =
      fsck(*fs, root / workspace::store_dir_name,
           fsck_options{.schemas = &schemas, .probe_lock = true, .records = true});
  if (!report) {
    return pass(report);
  }
  if (report->live_writer) {
    return make_error(errc::writer_locked, root.string() + ": another writer holds the workspace");
  }
  const auto c8 = std::ranges::find(report->findings, corruption_class::c8, &finding::cls);
  if (c8 != report->findings.end()) {
    return make_error(errc::format_unsupported,
                      "open " + root.string() + " refused: " + c8->line());
  }
  const bool for_write = mode == corruption_mode::strict
                             ? report->findings.empty()
                             : std::ranges::all_of(report->findings, [](const finding &f) {
                                 return opens_for_write(f.cls);
                               });
  if (!for_write) {
    if (mode == corruption_mode::strict) {
      return make_error(errc::store_corrupt,
                        "strict open " + root.string() + " refused: " + refusal(report->findings));
    }
    return open_view(fs, root, *report, &schemas, std::move(options), how);
  }
  checked_open out;
  out.findings = report->findings;
  out.flagged = flagged_of(report->findings);
  options.fs = fs;
  result<std::unique_ptr<workspace>> ws =
      workspace::open(root, open_mode::write, policy, std::move(options));
  if (!ws) {
    return pass(ws);
  }
  for (const recovery_finding &f : (*ws)->last_recovery().findings) {
    out.findings.push_back(from_recovery(f));
  }
  out.quarantined = (*ws)->last_recovery().quarantined;
  out.ws = std::move(*ws);
  return out;
}

} // namespace sl::fabric
