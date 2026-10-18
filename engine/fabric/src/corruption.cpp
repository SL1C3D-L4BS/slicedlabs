// Corruption policies (WO-059, SL-ARCH-002 `corruption` C1-C12, ARCH2-R41 and ARCH2-R42,
// ADR-0015): the policy table, findings, quarantine, loss reports and repairs. The strict and
// lenient opens live in workspace_corruption.cpp, fsck in fsck.cpp.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/corruption.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/gen/fabric.provenance.gen.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/log_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/recovery.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/tree.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/vfs.hpp"

namespace sl::fabric {

namespace {

namespace stdfs = std::filesystem;

template <class T> std::unexpected<error> pass(const result<T> &r) {
  return std::unexpected(r.error());
}

// SL-ARCH-002 `corruption`, row by row.
constexpr std::array<corruption_policy, 12> policies = {{
    {.cls = corruption_class::c1,
     .name = "torn tail",
     .detect = "R4",
     .policy = "quarantine tail bytes, truncate, open normally",
     .evidence = "lost_bytes",
     .quarantines = true,
     .self_healing = true},
    {.cls = corruption_class::c2,
     .name = "mid-log CRC mismatch",
     .detect = "R4",
     .policy = "quarantine segment copy, read-only view at last intact commit before damage",
     .evidence = "offset, commits after damage",
     .quarantines = true,
     .read_only_view = true},
    {.cls = corruption_class::c3,
     .name = "broken chain or seq gap",
     .detect = "R5",
     .policy = "as C2",
     .evidence = "seq, expected and found prev",
     .quarantines = true,
     .read_only_view = true},
    {.cls = corruption_class::c4,
     .name = "object hash mismatch",
     .detect = "R5, R9, read verify",
     .policy = "quarantine object, open newest revision whose closure verifies",
     .evidence = "oid, records affected",
     .quarantines = true,
     .read_only_view = true},
    {.cls = corruption_class::c5,
     .name = "missing object",
     .detect = "R5, R9",
     .policy = "as C4",
     .evidence = "oid, records affected",
     .quarantines = true,
     .read_only_view = true},
    {.cls = corruption_class::c6,
     .name = "undecodable record",
     .detect = "decode",
     .policy = "quarantine, read-only, repair proposal",
     .evidence = "oid, rule, offset",
     .quarantines = true,
     .read_only_view = true,
     .repair = true},
    {.cls = corruption_class::c7,
     .name = "schema-invalid record",
     .detect = "check under stored schema_set",
     .policy = "flag record read-only, repair proposal",
     .evidence = "record id, diagnostics",
     .read_only_view = true,
     .repair = true},
    {.cls = corruption_class::c8,
     .name = "unknown newer format",
     .detect = "R2, frame header",
     .policy = "refuse format_unsupported, touch nothing",
     .evidence = "found and supported version",
     .refuses = true},
    {.cls = corruption_class::c9,
     .name = "dangling strong reference at rest",
     .detect = "fsck",
     .policy = "report, repair proposal (weaken or restore)",
     .evidence = "from, to, path",
     .repair = true},
    {.cls = corruption_class::c10,
     .name = "id-class confusion",
     .detect = "fsck, decode",
     .policy = "quarantine record",
     .evidence = "record id, type record class",
     .quarantines = true,
     .read_only_view = true},
    {.cls = corruption_class::c11,
     .name = "stale lock",
     .detect = "R1",
     .policy = "take over, report",
     .evidence = "previous holder info",
     .self_healing = true},
    {.cls = corruption_class::c12,
     .name = "corrupt checkpoint",
     .detect = "R3",
     .policy = "ignore, rebuild, report",
     .evidence = "file name",
     .quarantines = true,
     .self_healing = true},
}};

result<void> sync_dir(vfs &fs, const stdfs::path &dir, durability d) noexcept {
  if (d == durability::none) {
    return {};
  }
  return fs.sync_dir(dir.empty() ? stdfs::path(".") : dir);
}

result<void> sync_file(const vfs_file &f, durability d) noexcept {
  switch (d) {
  case durability::none:
    return {};
  case durability::flush:
    return f.fdatasync();
  case durability::fsync:
    return f.fsync();
  }
  return {};
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
  return sync_dir(fs, dir.parent_path(), d);
}

// The bytes quarantine() copies: the whole source or its range.
result<std::vector<std::uint8_t>> source_bytes(vfs &fs, const quarantine_item &what) {
  const stdfs::path from = what.store_dir / what.source;
  if (!what.offset && !what.length) {
    return read_file(fs, from);
  }
  result<vfs_file> f = fs.open(from, open_read);
  if (!f) {
    return pass(f);
  }
  const result<std::uint64_t> size = f->size();
  if (!size) {
    return pass(size);
  }
  const std::uint64_t at = what.offset.value_or(0);
  const std::uint64_t n = what.length.value_or(*size > at ? *size - at : 0);
  std::vector<std::uint8_t> out(static_cast<std::size_t>(n));
  if (result<void> ok = f->read_exact(at, out); !ok) {
    return pass(ok);
  }
  return out;
}

bool is_object_damage(const error &e) noexcept {
  return e.code == errc::object_missing || e.code == errc::object_corrupt;
}

// The document of the record (or of a tombstone's last record) an entry names, when it reads.
std::optional<record_id> document_of(const store_backend &store, const tree_entry &e) {
  digest target = e.target;
  if (e.kind == entry_kind::tombstone) {
    const result<object> t = store.get(target);
    if (!t) {
      return std::nullopt;
    }
    const auto tomb = decode_tombstone(*t);
    if (!tomb) {
      return std::nullopt;
    }
    target = tomb->last_record;
  } else if (e.kind != entry_kind::record) {
    return std::nullopt;
  }
  const result<object> o = store.get(target);
  if (!o) {
    return std::nullopt;
  }
  const result<record_object> r = decode_record_object(*o);
  if (!r) {
    return std::nullopt;
  }
  return r->document;
}

// The first parents of `head`, newest first, `head` excluded (the walk stops at damage).
std::vector<digest> ancestors_of(const store_backend &store, const digest &head) {
  std::vector<digest> out;
  std::set<digest> seen{head};
  digest at = head;
  for (;;) {
    const result<object> o = store.get(at);
    if (!o) {
      return out;
    }
    const auto rev = decode_revision(*o);
    if (!rev || rev->parents.empty()) {
      return out;
    }
    at = rev->parents.front();
    if (!seen.insert(at).second) {
      return out;
    }
    out.push_back(at);
  }
}

// The record `id` of `rev` when live there, its object read, verified and (with schemas) its body
// checking under its type.
std::optional<std::pair<record_object, digest>> verified_record(const store_backend &store,
                                                                const digest &rev,
                                                                const record_id &id,
                                                                const schema_registry *schemas) {
  const result<snapshot> s = snapshot::open(store, rev);
  if (!s) {
    return std::nullopt;
  }
  const result<std::optional<tree_entry>> e = s->entry(id);
  if (!e || !*e || (*e)->kind != entry_kind::record) {
    return std::nullopt;
  }
  const result<object> o = store.get((*e)->target);
  if (!o) {
    return std::nullopt;
  }
  result<record_object> r = decode_record_object(*o);
  if (!r || r->id != id) {
    return std::nullopt;
  }
  if (schemas != nullptr) {
    const type_desc *t = schemas->find(r->type);
    if (t != nullptr) {
      std::vector<diagnostic_lite> diags;
      if (!schemas->check(r->body, r->type, diags) || !diags.empty()) {
        return std::nullopt;
      }
    }
  }
  return std::make_pair(std::move(*r), (*e)->target);
}

std::string short_hex(const digest &d) { return d.to_hex().substr(0, 16); }

} // namespace

std::span<const corruption_policy> corruption_policies() noexcept { return policies; }

const corruption_policy &policy_of(corruption_class c) noexcept {
  const auto n = static_cast<std::size_t>(c);
  if (n < 1 || n > policies.size()) {
    return policies.front();
  }
  return policies[n - 1]; // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index): checked
}

std::string finding::line() const {
  std::string s = std::string(to_string(cls)) + " " + std::string(corruption_name(cls)) + ":";
  if (where.oid) {
    s += " oid " + where.oid->to_hex();
  }
  if (where.record) {
    s += " record " + where.record->to_string();
  }
  if (target) {
    s += " target " + target->to_string();
  }
  if (!where.file.empty()) {
    s += " " + where.file;
    if (where.segment != 0 || where.offset != 0) {
      s += " offset " + std::to_string(where.offset);
    }
  }
  s += ": " + detail;
  return s;
}

finding from_recovery(const recovery_finding &f) {
  finding out;
  out.cls = f.kind;
  out.code = f.code;
  out.where.file = f.file;
  out.where.offset = f.offset;
  if (f.file.starts_with("log/")) {
    out.where.segment = parse_segment_file_name(std::string_view(f.file).substr(4)).value_or(0);
  }
  if (f.kind == corruption_class::c4 || f.kind == corruption_class::c5) {
    out.where.oid = f.expected;
  }
  out.detail = f.message;
  out.seq = f.seq;
  out.commits_after = f.commits_after;
  return out;
}

result<std::string> quarantine(vfs &fs, const quarantine_item &what, durability sync) {
  if (what.name.empty() || what.name.contains('/')) {
    return make_error(errc::invalid_argument,
                      "quarantine: '" + what.name + "' is not a quarantine file name");
  }
  const bool ranged = what.offset.has_value() || what.length.has_value();
  if (ranged && what.method == quarantine_method::move) {
    return make_error(errc::invalid_argument, "quarantine: a byte range can only be copied");
  }
  const stdfs::path q = what.store_dir / "quarantine";
  if (result<void> ok = ensure_dir(fs, q, sync); !ok) {
    return pass(ok);
  }
  std::vector<std::uint8_t> bytes;
  if (what.method == quarantine_method::copy) {
    result<std::vector<std::uint8_t>> b = source_bytes(fs, what);
    if (!b) {
      return pass(b);
    }
    bytes = std::move(*b);
  } else {
    const result<bool> present = fs.exists(what.store_dir / what.source);
    if (!present) {
      return pass(present);
    }
    if (!*present) {
      return make_error(errc::not_found,
                        "quarantine: " + (what.store_dir / what.source).string() + " is absent");
    }
  }
  std::string name;
  for (int n = 1;; ++n) {
    name = n == 1 ? what.name : what.name + "-" + std::to_string(n);
    const result<bool> taken = fs.exists(q / name);
    if (!taken) {
      return pass(taken);
    }
    if (!*taken) {
      break;
    }
    if (what.method == quarantine_method::copy) {
      // The same bytes under the same name: quarantined before (a rerun reuses it).
      const result<std::vector<std::uint8_t>> old = read_file(fs, q / name);
      if (old && *old == bytes) {
        return "quarantine/" + name;
      }
    }
  }
  if (what.method == quarantine_method::move) {
    if (result<void> ok = fs.rename(what.store_dir / what.source, q / name); !ok) {
      return pass(ok);
    }
    if (result<void> ok = sync_dir(fs, (what.store_dir / what.source).parent_path(), sync); !ok) {
      return pass(ok);
    }
  } else {
    result<vfs_file> f =
        fs.open(q / name, open_options{.write = true, .create = true, .exclusive = true});
    if (!f) {
      return pass(f);
    }
    if (!bytes.empty()) {
      if (result<std::uint64_t> at = f->append(bytes); !at) {
        return pass(at);
      }
    }
    if (result<void> ok = sync_file(*f, sync); !ok) {
      return pass(ok);
    }
  }
  if (result<void> ok = sync_dir(fs, q, sync); !ok) {
    return pass(ok);
  }
  return "quarantine/" + name;
}

result<loss_report> compute_loss(const store_backend &store, const std::optional<digest> &recovered,
                                 std::span<const digest> lost) {
  loss_report out;
  for (const digest &d : lost) {
    if (d != recovered) {
      out.revisions.push_back(d);
    }
  }
  std::ranges::sort(out.revisions);
  const auto dup = std::ranges::unique(out.revisions);
  out.revisions.erase(dup.begin(), dup.end());
  if (out.revisions.empty()) {
    return out;
  }
  result<snapshot> base = recovered ? snapshot::open(store, *recovered) : snapshot::empty(store);
  if (!base) {
    return pass(base);
  }
  bool found = false;
  std::set<record_id> records;
  std::set<record_id> documents;
  for (const digest &l : lost) {
    if (l == recovered) {
      continue;
    }
    const result<snapshot> s = snapshot::open(store, l);
    if (!s) {
      if (is_object_damage(s.error())) {
        continue;
      }
      return pass(s);
    }
    const result<std::vector<record_delta>> deltas = base->diff(*s);
    if (!deltas) {
      if (is_object_damage(deltas.error())) {
        continue;
      }
      return pass(deltas);
    }
    found = true;
    for (const record_delta &d : *deltas) {
      records.insert(d.id);
      bool known = false;
      for (const std::optional<tree_entry> &e : {d.before, d.after}) {
        if (!e) {
          continue;
        }
        const std::optional<record_id> doc = document_of(store, *e);
        known = known || doc.has_value();
        if (doc && !doc->is_null()) {
          documents.insert(*doc);
        }
      }
      out.exact = out.exact && known;
    }
    break;
  }
  out.exact = out.exact && found;
  out.records.assign(records.begin(), records.end());
  out.documents.assign(documents.begin(), documents.end());
  return out;
}

result<std::vector<repair_proposal>> propose_repairs(const store_backend &store, const digest &head,
                                                     std::span<const finding> findings,
                                                     const schema_registry *schemas) {
  std::map<record_id, corruption_class> targets;
  for (const finding &f : findings) {
    if (!policy_of(f.cls).repair) {
      continue;
    }
    const std::optional<record_id> id = f.cls == corruption_class::c9 ? f.target : f.where.record;
    if (id) {
      targets.emplace(*id, f.cls);
    }
  }
  std::vector<repair_proposal> out;
  if (targets.empty()) {
    return out;
  }
  const result<snapshot> now = snapshot::open(store, head);
  if (!now) {
    return pass(now);
  }
  const std::vector<digest> ancestors = ancestors_of(store, head);
  for (const auto &[id, cls] : targets) {
    // The type the record has at the head, when its record object still decodes there.
    std::optional<type_id> head_type;
    bool live = false;
    if (const result<std::optional<tree_entry>> e = now->entry(id); e && *e) {
      live = (*e)->kind == entry_kind::record;
      if (live) {
        if (const result<object> o = store.get((*e)->target); o) {
          if (const result<record_object> r = decode_record_object(*o); r) {
            head_type = r->type;
          }
        }
      }
    }
    for (const digest &anc : ancestors) {
      std::optional<std::pair<record_object, digest>> src =
          verified_record(store, anc, id, schemas);
      if (!src || (head_type && src->first.type != *head_type)) {
        continue;
      }
      repair_proposal p;
      p.cls = cls;
      p.record = id;
      p.source = repair_source::ancestor_revision;
      p.source_revision = anc;
      p.source_object = src->second;
      p.op = live ? op_set(id, fabric::path{}, src->first.body)
                  : op_create(id, src->first.type, src->first.body);
      p.summary = std::string(to_string(cls)) + " restore " + id.to_string() + " from revision " +
                  short_hex(anc) + " (record object " + short_hex(src->second) + ")";
      out.push_back(std::move(p));
      break;
    }
  }
  return out;
}

result<commit_receipt> apply_repairs(transaction_manager &manager,
                                     std::span<const repair_proposal> proposals,
                                     const repair_origin &origin, std::string_view ref) {
  if (proposals.empty()) {
    return make_error(errc::invalid_argument, "apply_repairs: no proposals");
  }
  const store_backend &store = manager.store();
  std::string label = "repair:";
  for (const repair_proposal &p : proposals) {
    // ARCH2-R42: the body staged is exactly the verified source's, never invented.
    const result<object> o = store.get(p.source_object);
    if (!o) {
      return make_error(errc::object_corrupt, "apply_repairs: source " + p.source_object.to_hex() +
                                                  " does not verify: " + o.error().message);
    }
    const result<record_object> r = decode_record_object(*o);
    if (!r || r->id != p.record || !p.op.new_value || *p.op.new_value != r->body ||
        p.op.target != p.record) {
      return make_error(errc::object_corrupt, "apply_repairs: the op for " + p.record.to_string() +
                                                  " does not carry the body of " +
                                                  p.source_object.to_hex());
    }
    label += " " + p.summary + ";";
  }
  tx_options o;
  o.origin.actor = origin.actor;
  o.origin.subsystem = origin.subsystem;
  o.origin.epistemic = epistemic_class::inferred;
  o.ref = std::string(ref);
  o.meta.label = std::move(label);
  result<transaction> t = manager.begin(std::move(o));
  if (!t) {
    return pass(t);
  }
  for (const repair_proposal &p : proposals) {
    if (result<void> ok = t->stage(p.op); !ok) {
      return pass(ok);
    }
  }
  result<commit_receipt> receipt = t->commit();
  if (!receipt) {
    std::string why = "apply_repairs: " + receipt.error().message;
    for (const diagnostic &d : t->diagnostics()) {
      why += "; " + d.rule + " " + d.message;
    }
    return make_error(receipt.error().code, why);
  }
  return receipt;
}

} // namespace sl::fabric
