// fsck (WO-059, SL-ARCH-002 ARCH2-R41 and ARCH2-R44, `corruption` C1-C12, ADR-0015): the
// read-only check of a durable store, the record checks, the state-closure check and the salvage
// store a lenient open reads through. See fsck.hpp.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/checkpoint.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/corruption.hpp"
#include "sl/fabric/deps.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/fsck.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/journal.hpp"
#include "sl/fabric/recovery.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/tree.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/vfs.hpp"

namespace sl::fabric {

namespace {

namespace stdfs = std::filesystem;

constexpr std::size_t header_crc_at = 20;

template <class T> std::unexpected<error> pass(const result<T> &r) {
  return std::unexpected(r.error());
}

std::uint32_t load_u32(byte_span b) noexcept {
  return static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8U) |
         (static_cast<std::uint32_t>(b[2]) << 16U) | (static_cast<std::uint32_t>(b[3]) << 24U);
}

bool is_object_damage(const error &e) noexcept {
  return e.code == errc::object_missing || e.code == errc::object_corrupt;
}

// Where the SLFO bytes of an intact object are.
struct location {
  std::string file;
  std::uint64_t offset = 0;
  std::uint64_t length = 0;
};

// The read-only store over intact objects (fsck.hpp, open_salvage_store).
class salvage_backend final : public store_backend {
public:
  salvage_backend(vfs &fs, std::shared_ptr<vfs> keep, stdfs::path dir,
                  std::map<digest, location> index, std::vector<ref_entry> refs) noexcept
      : keep_(std::move(keep)), fs_(&fs), dir_(std::move(dir)), index_(std::move(index)),
        refs_(std::move(refs)) {}

  [[nodiscard]] result<object> get(const digest &id) const noexcept override {
    const auto it = index_.find(id);
    if (it == index_.end()) {
      return make_error(errc::object_missing, "object " + id.to_hex() + " is not stored");
    }
    result<vfs_file> f = fs_->open(dir_ / it->second.file, open_read);
    if (!f) {
      return pass(f);
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(it->second.length));
    if (result<void> ok = f->read_exact(it->second.offset, bytes); !ok) {
      return make_error(errc::object_corrupt,
                        "object " + id.to_hex() + " cannot be read: " + ok.error().message);
    }
    result<object> o = parse_object(bytes);
    if (!o || o->id != id) {
      return make_error(errc::object_corrupt, "object " + id.to_hex() + " does not verify");
    }
    return o;
  }
  [[nodiscard]] result<bool> has(const digest &id) const noexcept override {
    return index_.contains(id);
  }
  [[nodiscard]] result<digest> read_ref(std::string_view name) const noexcept override {
    for (const ref_entry &r : refs_) {
      if (r.name == name) {
        return r.target;
      }
    }
    return make_error(errc::not_found, "no ref '" + std::string(name) + "'");
  }
  [[nodiscard]] result<std::vector<ref_entry>> list_refs() const noexcept override { return refs_; }
  [[nodiscard]] result<commit_ack> commit(const mutation_capability & /*cap*/,
                                          commit_batch /*batch*/) noexcept override {
    return make_error(errc::permission_denied, "a recovered view is read-only");
  }
  [[nodiscard]] result<verify_report> verify(const verify_scope &scope) const noexcept override {
    std::vector<digest> ids;
    ids.reserve(index_.size());
    for (const auto &[id, where] : index_) {
      ids.push_back(id);
    }
    return verify_store(*this, scope, ids);
  }

private:
  std::shared_ptr<vfs> keep_;
  vfs *fs_;
  stdfs::path dir_;
  std::map<digest, location> index_;
  std::vector<ref_entry> refs_;
};

// Whether an intact frame's payload parses as its type says (resync candidates; as recovery).
bool payload_parses(const frame_view &f) {
  switch (f.type) {
  case frame_type::object:
    return parse_object(f.payload).has_value();
  case frame_type::commit:
    return decode_commit(f.payload).has_value();
  case frame_type::intent_begin:
  case frame_type::intent_end:
    return decode_intent(f.type, f.payload).has_value();
  case frame_type::note:
    return true;
  }
  return false;
}

// The first offset after `fault` where an intact frame with a parsing payload starts.
std::optional<std::uint64_t> resync(byte_span file, std::uint64_t fault) {
  for (std::uint64_t p = fault + 1; p + frame_prefix_size <= file.size(); ++p) {
    frame_reader r(file.subspan(static_cast<std::size_t>(p)), p);
    const std::optional<frame_view> f = r.next();
    if (f && payload_parses(*f)) {
      return p;
    }
  }
  return std::nullopt;
}

// The path of the first id value equal to `target` in `v` (lists end the path: no path enters a
// list).
bool find_id_path(const value &v, const record_id &target, fabric::path &p) {
  switch (v.kind()) {
  case value_kind::id:
    return v.as_id_bytes() == target.bytes();
  case value_kind::struct_:
    for (const field &f : v.fields()) {
      p.push_back(path_segment::field(f.no));
      if (find_id_path(f.v, target, p)) {
        return true;
      }
      p.pop_back();
    }
    return false;
  case value_kind::variant:
    p.push_back(path_segment::field(v.case_no()));
    if (find_id_path(v.payload(), target, p)) {
      return true;
    }
    p.pop_back();
    return false;
  case value_kind::map:
    for (const map_entry &e : v.entries()) {
      result<path_segment> k = path_segment::key(e.key);
      if (!k) {
        continue;
      }
      p.push_back(std::move(*k));
      if (find_id_path(e.v, target, p)) {
        return true;
      }
      p.pop_back();
    }
    return false;
  case value_kind::oset:
    for (const oset_entry &e : v.elements()) {
      p.push_back(path_segment::element(e.element));
      if (find_id_path(e.v, target, p)) {
        return true;
      }
      p.pop_back();
    }
    return false;
  case value_kind::list:
    return std::ranges::any_of(v.items(), [&](const value &item) {
      fabric::path inner;
      return find_id_path(item, target, inner);
    });
  default:
    return false;
  }
}

finding make_finding(corruption_class cls, errc code, std::string file, std::uint32_t segment,
                     std::uint64_t offset, std::optional<digest> oid, std::string detail) {
  finding f;
  f.cls = cls;
  f.code = code;
  f.where.file = std::move(file);
  f.where.segment = segment;
  f.where.offset = offset;
  f.where.oid = oid;
  f.detail = std::move(detail);
  return f;
}

struct record_stats {
  std::uint64_t checked = 0;
  std::uint64_t unjudged = 0;
};

// check_records with counts.
result<std::vector<finding>> records_of(const store_backend &store, const digest &revision,
                                        const schema_registry *schemas, record_stats &stats) {
  std::vector<finding> out;
  const result<snapshot> s = snapshot::open(store, revision);
  if (!s) {
    if (is_object_damage(s.error())) {
      return out;
    }
    return pass(s);
  }
  const tree_reader reader(store);
  const auto visit = [&](const tree_entry &e) -> result<void> {
    if (e.kind != entry_kind::record) {
      return {};
    }
    ++stats.checked;
    const result<object> o = store.get(e.target);
    if (!o) {
      return {}; // the reachability walk reports it
    }
    const result<record_object> r = decode_record_object(*o);
    if (!r) {
      finding f = make_finding(corruption_class::c6, errc::object_corrupt, "", 0, 0, e.target,
                               "record object: " + r.error().message);
      f.where.record = e.id;
      out.push_back(std::move(f));
      return {};
    }
    if (r->id != e.id) {
      finding f = make_finding(corruption_class::c10, errc::id_class_mismatch, "", 0, 0, e.target,
                               "the trie entry of " + e.id.to_string() +
                                   " names the record object of " + r->id.to_string());
      f.where.record = e.id;
      out.push_back(std::move(f));
      return {};
    }
    const type_desc *t = schemas != nullptr ? schemas->find(r->type) : nullptr;
    if (t == nullptr) {
      ++stats.unjudged;
      return {};
    }
    if (t->is_record() && t->record_class != id_class::none && e.id.cls() != t->record_class) {
      finding f = make_finding(corruption_class::c10, errc::id_class_mismatch, "", 0, 0, e.target,
                               e.id.to_string() + " has class " +
                                   std::string(to_string(e.id.cls())) + ", type " + t->name +
                                   " has record class " + std::string(to_string(t->record_class)));
      f.where.record = e.id;
      out.push_back(std::move(f));
    }
    std::vector<diagnostic_lite> diags;
    const result<void> checked = schemas->check(r->body, r->type, diags);
    if (!checked || !diags.empty()) {
      std::string why;
      for (const diagnostic_lite &d : diags) {
        why += (why.empty() ? "" : "; ") + d.rule + " at " + to_string(d.path) + ": " + d.message;
      }
      if (why.empty()) {
        why = checked ? std::string("diagnostics") : checked.error().message;
      }
      finding f = make_finding(corruption_class::c7, errc::schema_invalid, "", 0, 0, e.target,
                               "type " + t->name + ": " + why);
      f.where.record = e.id;
      out.push_back(std::move(f));
      return {};
    }
    const result<std::vector<edge>> edges = extract_edges(e.id, r->body, *t, *schemas);
    if (!edges) {
      return {};
    }
    std::set<record_id> named;
    for (const edge &ed : *edges) {
      const bool must_live =
          (ed.kind == edge_kind::reference || ed.kind == edge_kind::containment) &&
          (ed.strength == ref_strength::strong || ed.strength == ref_strength::owned);
      if (!must_live || !named.insert(ed.to).second) {
        continue;
      }
      const result<bool> live = s->contains(ed.to);
      if (!live) {
        if (is_object_damage(live.error())) {
          continue;
        }
        return pass(live);
      }
      if (*live) {
        continue;
      }
      fabric::path p;
      const std::string at = find_id_path(r->body, ed.to, p) ? to_string(p) : std::string("?");
      finding f = make_finding(corruption_class::c9, errc::object_missing, "", 0, 0, e.target,
                               "from " + e.id.to_string() + " to " + ed.to.to_string() +
                                   " at path " + at + ": the target is not live");
      f.where.record = e.id;
      f.target = ed.to;
      out.push_back(std::move(f));
    }
    return {};
  };
  if (result<void> ok = reader.for_each(s->records_root(), visit); !ok) {
    if (!is_object_damage(ok.error())) {
      return pass(ok);
    }
  }
  return out;
}

class scanner {
public:
  scanner(vfs &fs, stdfs::path dir, const fsck_options &options)
      : fs_(&fs), dir_(std::move(dir)), options_(options) {}

  result<fsck_report> run();

private:
  void add(finding f) { report_.findings.push_back(std::move(f)); }
  result<bool> check_format();
  result<void> probe_lock();
  result<bool> scan_log();
  result<bool> scan_segment(std::uint32_t number, bool last);
  void on_frame(const frame_view &f, const std::string &file, std::uint32_t segment);
  result<void> on_commit(const frame_view &f, const std::string &file, std::uint32_t segment);
  void index_object(const digest &id, std::string file, std::uint64_t frame, std::uint64_t offset,
                    std::uint64_t length, bool loose);
  result<void> check_checkpoints();
  result<void> read_pins();
  result<void> walk(const store_backend &store);
  result<void> check_heads(const store_backend &store);

  vfs *fs_;
  stdfs::path dir_;
  fsck_options options_;
  fsck_report report_;

  chain_state chain_;
  objects_hasher hasher_;
  std::uint64_t pending_ = 0;
  std::uint64_t pending_first_ = 0;
  bool damaged_ = false; // frame damage since the last commit
  bool log_ok_ = true;   // no damage and no invalid commit so far
  std::map<digest, location> index_;
  std::map<std::string, digest, std::less<>> heads_;
};

result<bool> scanner::check_format() {
  const result<std::vector<std::uint8_t>> bytes = read_file(*fs_, dir_ / "FORMAT");
  if (!bytes) {
    return pass(bytes);
  }
  std::string text(bytes->begin(), bytes->end());
  if (text == format_text) {
    return true;
  }
  if (text.ends_with('\n')) {
    text.pop_back();
  }
  constexpr std::string_view prefix = "slfabric ";
  const std::size_t dot = text.find('.');
  const bool shaped = text.starts_with(prefix) && dot != std::string::npos && dot > prefix.size() &&
                      dot + 1 < text.size() && text.find('.', dot + 1) == std::string::npos &&
                      std::ranges::all_of(text.substr(prefix.size()), [](char c) {
                        return c == '.' || (c >= '0' && c <= '9');
                      });
  if (!shaped) {
    add(make_finding(corruption_class::c8, errc::store_corrupt, "FORMAT", 0, 0, std::nullopt,
                     "unrecognised format text; supported slfabric 1.0"));
    return false;
  }
  const std::string major = text.substr(prefix.size(), dot - prefix.size());
  const std::string minor = text.substr(dot + 1);
  if (major != "1") {
    add(make_finding(corruption_class::c8, errc::format_unsupported, "FORMAT", 0, 0, std::nullopt,
                     "store " + major + "." + minor + ", supported 1.0"));
    return false;
  }
  report_.read_only_newer = true;
  return true;
}

result<void> scanner::probe_lock() {
  const stdfs::path lock = dir_ / "lock";
  const result<bool> present = fs_->exists(lock);
  if (!present) {
    return pass(present);
  }
  if (!*present) {
    return {};
  }
  const result<std::vector<std::uint8_t>> bytes = read_file(*fs_, lock);
  if (!bytes) {
    return pass(bytes);
  }
  std::string text(bytes->begin(), bytes->end());
  while (!text.empty() && (text.back() == '\n' || text.back() == '\0')) {
    text.pop_back();
  }
  if (text.empty()) {
    return {};
  }
  result<vfs_file> f = fs_->open(lock, open_options{.write = true});
  if (!f) {
    return pass(f);
  }
  if (result<void> ok = f->lock_ofd(); !ok) {
    if (ok.error().code == errc::writer_locked) {
      report_.live_writer = true;
      return {};
    }
    return ok;
  }
  if (result<void> ok = f->unlock(); !ok) {
    return ok;
  }
  add(make_finding(corruption_class::c11, errc::writer_locked, "lock", 0, 0, std::nullopt,
                   "held by no live writer; previous holder: " + text));
  return {};
}

void scanner::index_object(const digest &id, std::string file, std::uint64_t frame,
                           std::uint64_t offset, std::uint64_t length, bool loose) {
  if (index_.contains(id)) {
    return;
  }
  index_.emplace(id, location{.file = file, .offset = offset, .length = length});
  report_.objects.push_back(fsck_object{.id = id,
                                        .file = std::move(file),
                                        .frame = frame,
                                        .offset = offset,
                                        .length = length,
                                        .loose = loose});
}

result<void> scanner::on_commit(const frame_view &f, const std::string &file,
                                std::uint32_t segment) {
  const result<commit_record> c = decode_commit(f.payload);
  if (!c) {
    add(make_finding(corruption_class::c6, errc::journal_corrupt, file, segment, f.offset,
                     std::nullopt, "commit frame: " + c.error().message));
    damaged_ = true;
    log_ok_ = false;
    return {};
  }
  bool valid = !damaged_;
  if (!damaged_) {
    if (c->seq != chain_.seq + 1 || c->prev != chain_.last) {
      add(make_finding(corruption_class::c3, errc::journal_corrupt, file, segment, f.offset,
                       std::nullopt,
                       "commit seq " + std::to_string(c->seq) + ", expected seq " +
                           std::to_string(chain_.seq + 1) + " chained to the previous commit"));
      valid = false;
    } else if (c->object_count != hasher_.count() || c->objects_digest != hasher_.finish()) {
      add(make_finding(
          corruption_class::c4, errc::journal_corrupt, file, segment, f.offset, std::nullopt,
          "commit seq " + std::to_string(c->seq) + ": " + std::to_string(c->object_count) +
              " objects recorded, " + std::to_string(hasher_.count()) +
              " object frames found; the objects digest differs"));
      valid = false;
    }
  }
  for (const digest &id : c->loose) {
    const std::string hex = id.to_hex();
    const std::string rel = "loose/" + hex.substr(0, 2) + "/" + hex;
    const result<std::vector<std::uint8_t>> bytes = read_file(*fs_, dir_ / rel);
    if (!bytes) {
      if (bytes.error().code != errc::not_found) {
        return pass(bytes);
      }
      add(make_finding(corruption_class::c5, errc::object_missing, rel, 0, 0, id,
                       "loose blob of commit seq " + std::to_string(c->seq) + " is missing"));
      valid = false;
      continue;
    }
    const result<object> o = parse_object(*bytes);
    if (!o || o->id != id) {
      add(make_finding(corruption_class::c4, errc::object_corrupt, rel, 0, 0, id,
                       o ? "loose blob hashes to " + o->id.to_hex() : "loose blob does not parse"));
      valid = false;
      continue;
    }
    index_object(id, rel, 0, 0, bytes->size(), true);
  }
  fsck_commit fc;
  fc.seq = c->seq;
  fc.segment = segment;
  fc.offset = f.offset;
  fc.payload = commit_digest(f.payload);
  fc.revision = c->revision;
  fc.refs = c->refs;
  fc.valid = valid;
  log_ok_ = log_ok_ && valid;
  fc.intact_prefix = log_ok_;
  for (const ref_update &r : c->refs) {
    heads_.insert_or_assign(r.name, r.desired);
  }
  chain_ = chain_state{.seq = c->seq, .last = fc.payload};
  report_.commits.push_back(std::move(fc));
  hasher_.reset();
  pending_ = 0;
  damaged_ = false;
  return {};
}

void scanner::on_frame(const frame_view &f, const std::string &file, std::uint32_t segment) {
  ++report_.frames;
  if (f.type == frame_type::note || f.type == frame_type::commit) {
    return;
  }
  if (pending_ == 0) {
    pending_first_ = f.offset;
  }
  ++pending_;
  if (f.type == frame_type::object) {
    const result<object> o = parse_object(f.payload);
    if (!o) {
      add(make_finding(corruption_class::c6, errc::journal_corrupt, file, segment, f.offset,
                       std::nullopt, "object frame: " + o.error().message));
      damaged_ = true;
      log_ok_ = false;
      return;
    }
    hasher_.add(o->id);
    index_object(o->id, file, f.offset, f.offset + frame_prefix_size, f.payload.size(), false);
    return;
  }
  if (!decode_intent(f.type, f.payload)) {
    add(make_finding(corruption_class::c6, errc::journal_corrupt, file, segment, f.offset,
                     std::nullopt, "intent frame does not decode"));
    damaged_ = true;
    log_ok_ = false;
  }
}

// One segment; false stops the scan (C8).
result<bool> scanner::scan_segment(std::uint32_t number, bool last) {
  const std::string file = "log/" + segment_file_name(number);
  const result<std::vector<std::uint8_t>> read = read_file(*fs_, dir_ / file);
  if (!read) {
    return pass(read);
  }
  const byte_span bytes = *read;
  if (bytes.size() < segment_header_size) {
    add(make_finding(last ? corruption_class::c1 : corruption_class::c2, errc::journal_corrupt,
                     file, number, 0, std::nullopt,
                     "torn segment header of " + std::to_string(bytes.size()) + " bytes"));
    log_ok_ = log_ok_ && last;
    return true;
  }
  if (load_u32(bytes.subspan(header_crc_at, 4)) != crc32c(bytes.first(header_crc_at))) {
    add(make_finding(corruption_class::c2, errc::journal_corrupt, file, number, 0, std::nullopt,
                     "segment header crc mismatch"));
    damaged_ = true;
    log_ok_ = false;
  } else if (const result<segment_header> h =
                 parse_segment_header(bytes.first(segment_header_size));
             !h) {
    if (h.error().code == errc::format_unsupported) {
      add(make_finding(corruption_class::c8, errc::format_unsupported, file, number, 0,
                       std::nullopt, h.error().message + "; supported 1.0"));
      return false;
    }
    add(make_finding(corruption_class::c2, errc::journal_corrupt, file, number, 0, std::nullopt,
                     h.error().message));
    damaged_ = true;
    log_ok_ = false;
  } else {
    report_.read_only_newer = report_.read_only_newer || h->minor > journal_minor;
    if (log_ok_ && (h->segment != number || h->first_seq != chain_.seq + 1)) {
      add(make_finding(corruption_class::c3, errc::journal_corrupt, file, number, 0, std::nullopt,
                       "header names segment " + std::to_string(h->segment) + " first seq " +
                           std::to_string(h->first_seq) + ", expected segment " +
                           std::to_string(number) + " first seq " +
                           std::to_string(chain_.seq + 1)));
      log_ok_ = false;
    }
  }
  std::uint64_t pos = segment_header_size;
  while (pos < bytes.size()) {
    frame_reader r(bytes.subspan(static_cast<std::size_t>(pos)), pos);
    while (const std::optional<frame_view> f = r.next()) {
      if (f->type == frame_type::commit) {
        ++report_.frames;
        if (result<void> ok = on_commit(*f, file, number); !ok) {
          return pass(ok);
        }
      } else {
        on_frame(*f, file, number);
      }
    }
    const frame_stop &stop = r.stop();
    if (stop.clean()) {
      break;
    }
    const std::optional<std::uint64_t> again = resync(bytes, stop.offset);
    if (!again && last && stop.fault != frame_fault::unknown_type) {
      const std::uint64_t cut = pending_ != 0 ? pending_first_ : stop.offset;
      add(make_finding(corruption_class::c1, errc::journal_corrupt, file, number, stop.offset,
                       std::nullopt,
                       stop.message + "; " + std::to_string(bytes.size() - cut) +
                           " bytes from offset " + std::to_string(cut) + " are lost"));
      pending_ = 0; // the cut frames are part of the tail, not orphans
      return true;
    }
    add(make_finding(corruption_class::c2, errc::journal_corrupt, file, number, stop.offset,
                     std::nullopt, stop.message));
    damaged_ = true;
    log_ok_ = false;
    if (!again) {
      break;
    }
    pos = *again;
  }
  if (pending_ != 0 && !last) {
    add(make_finding(corruption_class::c2, errc::journal_corrupt, file, number, pending_first_,
                     std::nullopt,
                     std::to_string(pending_) + " frames after the last commit end the segment"));
    log_ok_ = false;
    pending_ = 0;
    hasher_.reset();
  }
  return true;
}

result<bool> scanner::scan_log() {
  const result<std::vector<std::string>> names = fs_->list(dir_ / "log");
  if (!names) {
    if (names.error().code != errc::not_found) {
      return pass(names);
    }
    add(make_finding(corruption_class::c2, errc::journal_corrupt, "log", 0, 0, std::nullopt,
                     "no log directory"));
    return true;
  }
  std::vector<std::uint32_t> segments;
  for (const std::string &name : *names) {
    const std::optional<std::uint32_t> n = parse_segment_file_name(name);
    if (!n) {
      add(make_finding(corruption_class::c2, errc::journal_corrupt, "log/" + name, 0, 0,
                       std::nullopt, "not a segment file"));
      continue;
    }
    segments.push_back(*n);
  }
  std::ranges::sort(segments);
  if (segments.empty()) {
    add(make_finding(corruption_class::c2, errc::journal_corrupt, "log", 0, 0, std::nullopt,
                     "no segment files"));
  }
  for (std::size_t i = 0; i < segments.size(); ++i) {
    if (segments[i] != i + 1) {
      add(make_finding(corruption_class::c2, errc::journal_corrupt,
                       "log/" + segment_file_name(static_cast<std::uint32_t>(i + 1)),
                       static_cast<std::uint32_t>(i + 1), 0, std::nullopt, "segment is missing"));
      log_ok_ = false;
      break;
    }
  }
  report_.segments = static_cast<std::uint32_t>(segments.size());
  for (std::size_t i = 0; i < segments.size(); ++i) {
    const result<bool> more = scan_segment(segments[i], i + 1 == segments.size());
    if (!more) {
      return more;
    }
    if (!*more) {
      return false;
    }
  }
  report_.orphan_frames = pending_;
  // C2 evidence: the intact commit frames after each damage.
  for (finding &f : report_.findings) {
    if (f.cls != corruption_class::c2 || f.where.segment == 0) {
      continue;
    }
    f.commits_after = static_cast<std::uint64_t>(
        std::ranges::count_if(report_.commits, [&](const fsck_commit &c) {
          return c.segment > f.where.segment ||
                 (c.segment == f.where.segment && c.offset > f.where.offset);
        }));
    f.detail += "; " + std::to_string(f.commits_after) + " intact commits after";
  }
  for (const auto &[name, target] : heads_) {
    report_.heads.push_back(ref_entry{.name = name, .target = target});
  }
  return true;
}

result<void> scanner::check_checkpoints() {
  const result<std::vector<std::string>> names = list_checkpoints(*fs_, dir_);
  if (!names) {
    return pass(names);
  }
  for (const std::string &name : *names) {
    const std::string file = std::string(checkpoint_dir_name) + "/" + name;
    const result<std::vector<std::uint8_t>> bytes = read_file(*fs_, dir_ / file);
    if (!bytes) {
      return pass(bytes);
    }
    const auto reject = [&](const std::string &why) {
      add(make_finding(corruption_class::c12, errc::checkpoint_invalid, file, 0, 0, std::nullopt,
                       why));
    };
    const result<checkpoint> c = decode_checkpoint(*bytes);
    if (!c) {
      reject(c.error().message);
      continue;
    }
    if (parse_checkpoint_file_name(name) != c->seq) {
      reject("the file name does not match seq " + std::to_string(c->seq));
      continue;
    }
    const bool named = std::ranges::any_of(report_.commits, [&](const fsck_commit &k) {
      return k.seq == c->seq && k.segment == c->segment && k.offset == c->frame_offset &&
             k.payload == c->last_commit;
    });
    if (!named) {
      reject("no intact commit seq " + std::to_string(c->seq) + " at segment " +
             std::to_string(c->segment) + " offset " + std::to_string(c->frame_offset));
    }
  }
  return {};
}

result<void> scanner::read_pins() {
  const result<bool> present = fs_->exists(dir_ / "pins");
  if (!present) {
    return pass(present);
  }
  if (!*present) {
    return {};
  }
  const result<std::vector<std::string>> names = fs_->list(dir_ / "pins");
  if (!names) {
    return pass(names);
  }
  for (const std::string &name : *names) {
    const std::string file = "pins/" + name;
    const result<std::vector<std::uint8_t>> bytes = read_file(*fs_, dir_ / file);
    if (!bytes) {
      return pass(bytes);
    }
    std::string text(bytes->begin(), bytes->end());
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
      text.pop_back();
    }
    const result<std::vector<std::uint8_t>> raw =
        text.size() == 64 ? from_hex(text) : result<std::vector<std::uint8_t>>{};
    if (!raw || raw->size() != digest{}.bytes.size()) {
      add(make_finding(corruption_class::c6, errc::store_corrupt, file, 0, 0, std::nullopt,
                       "a pin must hold a 64-hex revision digest"));
      continue;
    }
    digest d;
    std::ranges::copy(*raw, d.bytes.begin());
    report_.pins.push_back(ref_entry{.name = name, .target = d});
  }
  return {};
}

result<void> scanner::walk(const store_backend &store) {
  std::deque<std::pair<digest, std::string>> queue;
  for (const ref_entry &r : report_.heads) {
    queue.emplace_back(r.target, "ref '" + r.name + "'");
  }
  for (const ref_entry &r : report_.pins) {
    queue.emplace_back(r.target, "pin '" + r.name + "'");
  }
  std::set<digest> seen;
  const revision_links links;
  while (!queue.empty()) {
    auto [id, from] = std::move(queue.front());
    queue.pop_front();
    if (!seen.insert(id).second) {
      continue;
    }
    ++report_.objects_reached;
    const result<object> o = store.get(id);
    if (!o) {
      if (o.error().code == errc::object_missing) {
        add(make_finding(corruption_class::c5, errc::object_missing, "", 0, 0, id,
                         "not stored; reached from " + from));
      } else if (o.error().code == errc::object_corrupt) {
        add(make_finding(corruption_class::c4, errc::object_corrupt, "", 0, 0, id,
                         o.error().message + "; reached from " + from));
      } else {
        return pass(o);
      }
      continue;
    }
    std::vector<digest> out;
    if (result<void> ok = links.append_links(*o, out); !ok) {
      add(make_finding(corruption_class::c6, errc::object_corrupt, "", 0, 0, id,
                       std::string(to_string(o->header.kind)) +
                           " object does not decode: " + ok.error().message));
      continue;
    }
    if (o->header.kind == object_kind::record) {
      if (const result<record_object> r = decode_record_object(*o); !r) {
        add(make_finding(corruption_class::c6, errc::object_corrupt, "", 0, 0, id,
                         "record object: " + r.error().message));
        continue;
      }
    }
    const std::string next = "object " + id.to_hex();
    for (const digest &l : out) {
      if (!seen.contains(l)) {
        queue.emplace_back(l, next);
      }
    }
  }
  return {};
}

result<void> scanner::check_heads(const store_backend &store) {
  std::set<digest> revisions;
  for (const ref_entry &r : report_.heads) {
    revisions.insert(r.target);
  }
  for (const ref_entry &r : report_.pins) {
    revisions.insert(r.target);
  }
  std::set<std::pair<int, digest>> known;
  for (const finding &f : report_.findings) {
    if (f.where.oid) {
      known.emplace(static_cast<int>(f.cls), *f.where.oid);
    }
  }
  std::set<std::tuple<int, record_id, record_id>> named;
  for (const digest &rev : revisions) {
    record_stats stats;
    result<std::vector<finding>> found = records_of(store, rev, options_.schemas, stats);
    if (!found) {
      return pass(found);
    }
    report_.records_checked += stats.checked;
    report_.records_unjudged += stats.unjudged;
    for (finding &f : *found) {
      if (f.where.oid && f.cls == corruption_class::c6 &&
          known.contains({static_cast<int>(f.cls), *f.where.oid})) {
        continue;
      }
      // One finding per (class, record, target) over every head and pin.
      if (!named
               .emplace(static_cast<int>(f.cls), f.where.record.value_or(record_id{}),
                        f.target.value_or(record_id{}))
               .second) {
        continue;
      }
      add(std::move(f));
    }
  }
  return {};
}

result<fsck_report> scanner::run() {
  const result<bool> formatted = fs_->exists(dir_ / "FORMAT");
  if (!formatted) {
    return pass(formatted);
  }
  if (!*formatted) {
    return make_error(errc::not_found, (dir_ / "FORMAT").string() + " is missing: no store here");
  }
  const result<bool> format_ok = check_format();
  if (!format_ok) {
    return pass(format_ok);
  }
  if (!*format_ok) {
    return std::move(report_);
  }
  if (options_.probe_lock) {
    if (result<void> ok = probe_lock(); !ok) {
      return pass(ok);
    }
  }
  const result<bool> log_ok = scan_log();
  if (!log_ok) {
    return pass(log_ok);
  }
  if (!*log_ok) {
    return std::move(report_);
  }
  if (result<void> ok = check_checkpoints(); !ok) {
    return pass(ok);
  }
  if (result<void> ok = read_pins(); !ok) {
    return pass(ok);
  }
  const salvage_backend store(*fs_, nullptr, dir_, index_, {});
  if (result<void> ok = walk(store); !ok) {
    return pass(ok);
  }
  if (options_.records) {
    if (result<void> ok = check_heads(store); !ok) {
      return pass(ok);
    }
  }
  return std::move(report_);
}

// Every trie entry of `root` read; record and tombstone objects (and their last records) read
// and decoded; document entries' member tries walked.
result<void> read_trie(const store_backend &store, const digest &root, bool membership) {
  const tree_reader reader(store);
  return reader.for_each(root, [&](const tree_entry &e) -> result<void> {
    switch (e.kind) {
    case entry_kind::record: {
      const result<object> o = store.get(e.target);
      if (!o) {
        return pass(o);
      }
      if (const result<record_object> r = decode_record_object(*o); !r) {
        return pass(r);
      }
      return {};
    }
    case entry_kind::tombstone: {
      const result<object> o = store.get(e.target);
      if (!o) {
        return pass(o);
      }
      const auto t = decode_tombstone(*o);
      if (!t) {
        return pass(t);
      }
      const result<object> last = store.get(t->last_record);
      if (!last) {
        return pass(last);
      }
      return {};
    }
    case entry_kind::document:
      if (membership) {
        return read_trie(store, e.target, false);
      }
      return {};
    case entry_kind::member:
      return {};
    }
    return {};
  });
}

stdfs::path store_dir_of(vfs &fs, const stdfs::path &root, bool &found) {
  found = false;
  stdfs::path inner = root / ".slfabric";
  if (const result<bool> r = fs.exists(inner / "FORMAT"); r && *r) {
    found = true;
    return inner;
  }
  if (const result<bool> r = fs.exists(root / "FORMAT"); r && *r) {
    found = true;
    return root;
  }
  return inner;
}

} // namespace

result<fsck_report> fsck(vfs &fs, const std::filesystem::path &root, fsck_options options) {
  bool found = false;
  const stdfs::path dir = store_dir_of(fs, root, found);
  if (!found) {
    return make_error(errc::not_found, root.string() + " holds no store");
  }
  scanner s(fs, dir, options);
  return s.run();
}

result<std::vector<finding>> check_records(const store_backend &store, const digest &revision,
                                           const schema_registry *schemas) {
  record_stats stats;
  return records_of(store, revision, schemas, stats);
}

result<bool> state_closure_verifies(const store_backend &store, const digest &revision) {
  const auto damaged = [](const error &e) -> result<bool> {
    if (is_object_damage(e) || e.code == errc::decode_truncated ||
        e.code == errc::decode_noncanonical || e.code == errc::type_mismatch) {
      return false;
    }
    return std::unexpected(e);
  };
  const result<object> o = store.get(revision);
  if (!o) {
    return damaged(o.error());
  }
  const auto rev = decode_revision(*o);
  if (!rev) {
    return false;
  }
  const result<object> tx = store.get(rev->tx);
  if (!tx) {
    return damaged(tx.error());
  }
  if (!decode_tx_record(*tx)) {
    return false;
  }
  const result<object> root = store.get(rev->root);
  if (!root) {
    return damaged(root.error());
  }
  const result<root_object> r = decode_root_object(*root);
  if (!r) {
    return false;
  }
  if (result<void> ok = read_trie(store, r->records, false); !ok) {
    return damaged(ok.error());
  }
  if (result<void> ok = read_trie(store, r->membership, true); !ok) {
    return damaged(ok.error());
  }
  return true;
}

result<std::shared_ptr<store_backend>> open_salvage_store(std::shared_ptr<vfs> fs,
                                                          const std::filesystem::path &store_dir,
                                                          const fsck_report &report,
                                                          std::vector<ref_entry> refs) {
  if (!fs) {
    return make_error(errc::invalid_argument, "open_salvage_store: no file system");
  }
  for (std::size_t i = 1; i < refs.size(); ++i) {
    if (!(refs[i - 1].name < refs[i].name)) {
      return make_error(errc::invalid_argument,
                        "open_salvage_store: refs must be sorted by name and unique");
    }
  }
  std::map<digest, location> index;
  for (const fsck_object &o : report.objects) {
    index.emplace(o.id, location{.file = o.file, .offset = o.offset, .length = o.length});
  }
  vfs &ref = *fs;
  return std::shared_ptr<store_backend>(std::make_shared<salvage_backend>(
      ref, std::move(fs), store_dir, std::move(index), std::move(refs)));
}

} // namespace sl::fabric
