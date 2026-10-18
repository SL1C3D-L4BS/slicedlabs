// The recovery driver (WO-055, SL-ARCH-002 `recovery_steps` R2-R10, ARCH2-R35 to R40,
// `corruption` C1-C6, C8 and C12, ADR-0015). See recovery.hpp for the steps; the report helpers
// live in recovery_report.cpp.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
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
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/journal.hpp"
#include "sl/fabric/log_backend.hpp"
#include "sl/fabric/recovery.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/store.hpp"
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

result<void> sync_dir(vfs &fs, const stdfs::path &dir, durability d) noexcept {
  if (d == durability::none) {
    return {};
  }
  return fs.sync_dir(dir);
}

// make_dir that accepts an existing directory; a new one is made durable in its parent.
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

std::string seg_file(std::string_view name) { return "log/" + std::string(name); }

// R2: FORMAT must read "slfabric 1.0" (C8 for a newer major, read_only_newer for a newer minor).
result<void> check_format(vfs &fs, const stdfs::path &dir) {
  const result<std::vector<std::uint8_t>> bytes = read_file(fs, dir / "FORMAT");
  if (!bytes) {
    return pass(bytes);
  }
  std::string text(bytes->begin(), bytes->end());
  if (text == format_text) {
    return {};
  }
  const std::string where = (dir / "FORMAT").string();
  if (text.ends_with('\n')) {
    text.pop_back();
  }
  constexpr std::string_view prefix = "slfabric ";
  const std::size_t dot = text.find('.');
  const bool shaped = text.starts_with(prefix) && dot != std::string::npos && dot > prefix.size() &&
                      dot + 1 < text.size() &&
                      std::ranges::all_of(text.substr(prefix.size()), [](char c) {
                        return c == '.' || (c >= '0' && c <= '9');
                      });
  if (!shaped || text.find('.', dot + 1) != std::string::npos) {
    return make_error(errc::store_corrupt, where + ": unrecognised text");
  }
  const std::string major = text.substr(prefix.size(), dot - prefix.size());
  const std::string minor = text.substr(dot + 1);
  if (major != "1") {
    return make_error(errc::format_unsupported, "C8 unknown newer format: " + where + ": store " +
                                                    major + "." + minor + ", supported 1.0");
  }
  return make_error(errc::read_only_newer,
                    where + ": store 1." + minor + " is newer: readable, never written by 1.0");
}

// The SLFO object of an object frame plus the links it declares.
struct parsed_object {
  object obj;
  std::vector<digest> links;
};

result<parsed_object> parse_with_links(byte_span frame, bool links) {
  result<object> o = parse_object(frame);
  if (!o) {
    return pass(o);
  }
  parsed_object out{.obj = std::move(*o), .links = {}};
  if (links) {
    const revision_links reader;
    if (result<void> ok = reader.append_links(out.obj, out.links); !ok) {
      return pass(ok);
    }
  }
  return out;
}

// Whether an intact frame's payload parses as its type says (resync candidates).
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

struct resync_result {
  bool found = false;
  std::uint64_t offset = 0;
  std::uint64_t commits = 0;
};

// R4: the first offset after `fault` where an intact frame with a parsing payload starts, and the
// intact commit frames from there on. `body` holds file bytes from file offset `base`.
resync_result resync(byte_span body, std::uint64_t base, std::uint64_t fault) {
  resync_result out;
  for (std::uint64_t p = fault + 1; p + frame_prefix_size <= base + body.size(); ++p) {
    frame_reader r(body.subspan(static_cast<std::size_t>(p - base)), p);
    std::optional<frame_view> f = r.next();
    if (!f || !payload_parses(*f)) {
      continue;
    }
    out.found = true;
    out.offset = p;
    for (; f; f = r.next()) {
      out.commits += f->type == frame_type::commit ? 1U : 0U;
    }
    return out;
  }
  return out;
}

// What recovery has seen since the last valid commit.
struct pending_frames {
  objects_hasher hasher;
  std::vector<intent_record> intents;
  std::vector<digest> ids;
  std::vector<digest> links;
  std::map<digest, digest> revision_schema; // revision object id -> schema_set
  std::uint64_t frames = 0;
  std::uint64_t objects = 0;
  std::uint64_t first_offset = 0;

  void clear() noexcept {
    hasher.reset();
    intents.clear();
    ids.clear();
    links.clear();
    revision_schema.clear();
    frames = 0;
    objects = 0;
  }
};

// R8: what to cut from the last segment.
struct tail_plan {
  bool dirty = false;
  bool torn_header = false;
  std::uint32_t segment = 0;
  std::string name;
  std::uint64_t cut = 0;
  std::uint64_t size = 0;
  std::vector<std::uint8_t> bytes; // the bytes cut, [cut, size)
};

class driver {
public:
  driver(vfs &fs, stdfs::path dir, const recovery_policy &policy, const log_options &log,
         recovery_mode mode)
      : fs_(&fs), dir_(std::move(dir)), policy_(policy), log_(log), mode_(mode) {}

  result<recovered_log> run();

private:
  [[nodiscard]] bool blocked() const noexcept { return out_.report.blocking(); }
  void finding(corruption_class kind, errc code, std::string file, std::uint64_t offset,
               bool blocking, const std::string &detail,
               std::optional<digest> expected = std::nullopt,
               std::optional<digest> found = std::nullopt, std::uint64_t commits_after = 0);

  result<void> list_segments();
  result<void> choose_checkpoint();
  result<std::optional<checkpoint>> verify_checkpoint(const std::string &name);
  result<void> scan();
  result<bool> scan_segment_file(std::size_t index);
  result<void> on_frame(const frame_view &f, const std::string &name, std::uint32_t segment);
  result<void> on_commit(const frame_view &f, const std::string &name, std::uint32_t segment);
  result<std::uint64_t> commits_in_later(std::size_t index);
  result<void> repair();
  result<std::string> quarantine_bytes(const std::string &base, byte_span bytes);
  result<std::string> quarantine_move(const stdfs::path &from, const std::string &base);
  result<std::string> free_name(const std::string &base, byte_span same_as, bool compare);
  result<void> collect_loose_tmp();
  result<void> verify_after_open(const log_backend &store);
  result<void> write_new_checkpoint();

  vfs *fs_;
  stdfs::path dir_;
  recovery_policy policy_;
  log_options log_;
  recovery_mode mode_;
  recovered_log out_;

  std::vector<std::uint32_t> segments_;
  std::optional<checkpoint> checkpoint_;
  std::vector<std::string> invalid_checkpoints_;
  std::vector<stdfs::path> loose_tmp_;

  // The state after the last valid commit.
  chain_state chain_;
  std::map<std::string, digest, std::less<>> refs_;
  std::map<record_id, std::string> open_;
  std::optional<digest> schema_set_;
  std::uint32_t commit_segment_ = 1;
  std::uint64_t commit_frame_ = 0;
  std::uint64_t commit_end_ = segment_header_size;
  std::set<digest> scanned_;
  std::vector<std::pair<digest, std::uint64_t>> links_; // (target, seq that needs it)

  pending_frames pending_;
  tail_plan tail_;
};

void driver::finding(corruption_class kind, errc code, std::string file, std::uint64_t offset,
                     bool blocking, const std::string &detail, std::optional<digest> expected,
                     std::optional<digest> found, std::uint64_t commits_after) {
  recovery_finding f;
  f.kind = kind;
  f.code = code;
  f.offset = offset;
  f.seq = chain_.seq;
  f.commits_after = commits_after;
  f.expected = expected;
  f.found = found;
  f.blocking = blocking;
  f.message = std::string(to_string(kind)) + " " + std::string(corruption_name(kind)) + ": " +
              file + " offset " + std::to_string(offset) + ": " + detail + " (after commit seq " +
              std::to_string(chain_.seq) + ")";
  f.file = std::move(file);
  out_.report.findings.push_back(std::move(f));
}

result<void> driver::list_segments() {
  const result<std::vector<std::string>> names = fs_->list(dir_ / "log");
  if (!names) {
    return pass(names);
  }
  for (const std::string &name : *names) {
    const std::optional<std::uint32_t> n = parse_segment_file_name(name);
    if (!n) {
      finding(corruption_class::c2, errc::journal_corrupt, "log/" + name, 0, true,
              "not a segment file");
      continue;
    }
    segments_.push_back(*n);
  }
  std::ranges::sort(segments_);
  if (segments_.empty()) {
    finding(corruption_class::c2, errc::journal_corrupt, "log", 0, true, "no segment files");
  }
  for (std::size_t i = 0; i < segments_.size(); ++i) {
    if (segments_[i] != i + 1) {
      finding(corruption_class::c2, errc::journal_corrupt,
              seg_file(segment_file_name(static_cast<std::uint32_t>(i + 1))), 0, true,
              "segment is missing");
      break;
    }
  }
  return {};
}

// R3: the commit frame a checkpoint names must re-read with its seq and payload digest.
result<std::optional<checkpoint>> driver::verify_checkpoint(const std::string &name) {
  const std::string file = std::string(checkpoint_dir_name) + "/" + name;
  const result<std::vector<std::uint8_t>> bytes =
      read_file(*fs_, dir_ / checkpoint_dir_name / name);
  if (!bytes) {
    return pass(bytes);
  }
  const auto reject = [&](std::string_view why) -> std::optional<checkpoint> {
    finding(corruption_class::c12, errc::checkpoint_invalid, file, 0, false,
            std::string(why) + "; ignored");
    invalid_checkpoints_.push_back(name);
    return std::nullopt;
  };
  result<checkpoint> c = decode_checkpoint(*bytes);
  if (!c) {
    return reject(c.error().message);
  }
  if (parse_checkpoint_file_name(name) != c->seq) {
    return reject("the file name does not match seq " + std::to_string(c->seq));
  }
  if (c->segment > segments_.size()) {
    return reject("segment " + std::to_string(c->segment) + " does not exist");
  }
  result<vfs_file> f = fs_->open(dir_ / "log" / segment_file_name(c->segment), open_read);
  if (!f) {
    return pass(f);
  }
  const result<std::uint64_t> size = f->size();
  if (!size) {
    return pass(size);
  }
  if (c->frame_offset < segment_header_size || c->offset > *size) {
    return reject("the commit frame lies outside " + segment_file_name(c->segment));
  }
  std::vector<std::uint8_t> frame(static_cast<std::size_t>(c->offset - c->frame_offset));
  if (result<void> ok = f->read_exact(c->frame_offset, frame); !ok) {
    return pass(ok);
  }
  frame_reader r(frame, c->frame_offset);
  const std::optional<frame_view> v = r.next();
  if (!v || v->type != frame_type::commit || r.stop().end != c->offset) {
    return reject("no intact commit frame at offset " + std::to_string(c->frame_offset));
  }
  const result<commit_record> rec = decode_commit(v->payload);
  if (!rec || rec->seq != c->seq || commit_digest(v->payload) != c->last_commit) {
    return reject("the commit frame at offset " + std::to_string(c->frame_offset) +
                  " is not commit seq " + std::to_string(c->seq));
  }
  return std::optional<checkpoint>(std::move(*c));
}

result<void> driver::choose_checkpoint() {
  if (!policy_.use_checkpoint) {
    return {};
  }
  const result<std::vector<std::string>> names = list_checkpoints(*fs_, dir_);
  if (!names) {
    return pass(names);
  }
  for (const std::string &name : *names) {
    result<std::optional<checkpoint>> c = verify_checkpoint(name);
    if (!c) {
      return pass(c);
    }
    if (*c) {
      checkpoint_ = std::move(**c);
      return {};
    }
  }
  return {};
}

result<void> driver::on_commit(const frame_view &f, const std::string &name,
                               std::uint32_t segment) {
  const std::string file = seg_file(name);
  const result<commit_record> c = decode_commit(f.payload);
  if (!c) {
    finding(corruption_class::c6, errc::journal_corrupt, file, f.offset, true,
            "commit frame: " + c.error().message);
    return {};
  }
  if (c->seq != chain_.seq + 1 || c->prev != chain_.last) {
    finding(corruption_class::c3, errc::journal_corrupt, file, f.offset, true,
            "commit seq " + std::to_string(c->seq) + ", expected seq " +
                std::to_string(chain_.seq + 1) + " chained to the previous commit",
            chain_.last, c->prev);
    return {};
  }
  if (c->object_count != pending_.hasher.count() || c->objects_digest != pending_.hasher.finish()) {
    finding(corruption_class::c4, errc::journal_corrupt, file, f.offset, true,
            "commit seq " + std::to_string(c->seq) + ": " + std::to_string(c->object_count) +
                " objects recorded, " + std::to_string(pending_.hasher.count()) +
                " object frames found, objects digest " +
                (c->objects_digest == pending_.hasher.finish() ? "matches" : "differs"),
            c->objects_digest, pending_.hasher.finish());
    return {};
  }
  std::vector<digest> incoming = pending_.ids;
  std::vector<digest> links = pending_.links;
  for (const digest &id : c->loose) {
    const result<std::vector<std::uint8_t>> bytes =
        read_file(*fs_, dir_ / "loose" / id.to_hex().substr(0, 2) / id.to_hex());
    if (!bytes) {
      if (bytes.error().code != errc::not_found) {
        return pass(bytes);
      }
      finding(corruption_class::c5, errc::journal_corrupt, file, f.offset, true,
              "commit seq " + std::to_string(c->seq) + ": loose blob " + id.to_hex() +
                  " is missing",
              id);
      return {};
    }
    result<parsed_object> o = parse_with_links(*bytes, policy_.verify_links);
    if (!o || o->obj.id != id) {
      finding(corruption_class::c4, errc::journal_corrupt, file, f.offset, true,
              "commit seq " + std::to_string(c->seq) + ": loose blob " + id.to_hex() +
                  (o ? " hashes to " + o->obj.id.to_hex() : " does not parse"),
              id, o ? std::optional<digest>(o->obj.id) : std::nullopt);
      return {};
    }
    incoming.push_back(id);
    links.insert(links.end(), o->links.begin(), o->links.end());
  }
  for (const ref_update &r : c->refs) {
    const auto it = refs_.find(r.name);
    const std::optional<digest> current =
        it == refs_.end() ? std::nullopt : std::optional<digest>(it->second);
    if (current != r.expected) {
      finding(corruption_class::c3, errc::journal_corrupt, file, f.offset, true,
              "commit seq " + std::to_string(c->seq) + " moves ref '" + r.name +
                  "' from an unexpected head",
              current, r.expected);
      return {};
    }
    // Without a checkpoint every stored object has been seen: a target must be one of them.
    const bool known =
        std::ranges::find(incoming, r.desired) != incoming.end() || scanned_.contains(r.desired);
    if (!checkpoint_ && !known) {
      finding(corruption_class::c5, errc::journal_corrupt, file, f.offset, true,
              "commit seq " + std::to_string(c->seq) + ": ref '" + r.name + "' target " +
                  r.desired.to_hex() + " is not stored",
              r.desired);
      return {};
    }
  }
  std::map<record_id, std::string> open = open_;
  for (const intent_record &in : pending_.intents) {
    const bool ok = in.phase == intent_phase::begin ? open.emplace(in.operation, in.kind).second
                                                    : open.erase(in.operation) != 0;
    if (!ok) {
      finding(corruption_class::c3, errc::journal_corrupt, file, f.offset, true,
              "commit seq " + std::to_string(c->seq) + ": intent " + in.operation.to_string() +
                  " does not bracket");
      return {};
    }
  }
  // R9 in the scan: without a checkpoint every stored object has been seen, so every link of
  // this commit's objects must be among them (objects link backwards or within their commit).
  if (!checkpoint_ && policy_.verify_links) {
    const std::set<digest> mine(incoming.begin(), incoming.end());
    for (const digest &l : links) {
      if (!mine.contains(l) && !scanned_.contains(l)) {
        finding(corruption_class::c5, errc::journal_corrupt, file, f.offset, true,
                "commit seq " + std::to_string(c->seq) + ": object " + l.to_hex() +
                    " is linked but not stored",
                l);
        return {};
      }
    }
  }
  // The commit is valid: apply it.
  for (const ref_update &r : c->refs) {
    refs_.insert_or_assign(r.name, r.desired);
  }
  open_ = std::move(open);
  if (const auto it = pending_.revision_schema.find(c->revision);
      it != pending_.revision_schema.end()) {
    schema_set_ = it->second;
  }
  if (checkpoint_ && policy_.verify_links) {
    for (const digest &l : links) {
      links_.emplace_back(l, c->seq); // checked against the store after it opens
    }
  }
  scanned_.insert(incoming.begin(), incoming.end());
  chain_ = chain_state{.seq = c->seq, .last = commit_digest(f.payload)};
  commit_segment_ = segment;
  commit_frame_ = f.offset;
  commit_end_ = f.offset + frame_prefix_size + f.payload.size();
  ++out_.report.commits_scanned;
  pending_.clear();
  return {};
}

result<void> driver::on_frame(const frame_view &f, const std::string &name, std::uint32_t segment) {
  ++out_.report.frames_scanned;
  if (f.type == frame_type::note) {
    return {};
  }
  if (pending_.frames == 0) {
    pending_.first_offset = f.offset;
  }
  switch (f.type) {
  case frame_type::object: {
    result<parsed_object> o = parse_with_links(f.payload, policy_.verify_links);
    if (!o) {
      finding(corruption_class::c6, errc::journal_corrupt, seg_file(name), f.offset, true,
              "object frame: " + o.error().message);
      return {};
    }
    if (o->obj.header.kind == object_kind::revision) {
      if (result<gen::fabric_history::revision> rev = decode_revision(o->obj)) {
        pending_.revision_schema.insert_or_assign(o->obj.id, rev->schema_set);
      }
    }
    pending_.hasher.add(o->obj.id);
    pending_.ids.push_back(o->obj.id);
    pending_.links.insert(pending_.links.end(), o->links.begin(), o->links.end());
    ++pending_.objects;
    ++pending_.frames;
    return {};
  }
  case frame_type::intent_begin:
  case frame_type::intent_end: {
    result<intent_record> in = decode_intent(f.type, f.payload);
    if (!in) {
      finding(corruption_class::c6, errc::journal_corrupt, seg_file(name), f.offset, true,
              in.error().message);
      return {};
    }
    pending_.intents.push_back(std::move(*in));
    ++pending_.frames;
    return {};
  }
  case frame_type::commit:
    return on_commit(f, name, segment);
  case frame_type::note:
    return {};
  }
  return {};
}

// Intact commit frames in the segments after `index` (the evidence of a C2 finding).
result<std::uint64_t> driver::commits_in_later(std::size_t index) {
  std::uint64_t n = 0;
  for (std::size_t i = index + 1; i < segments_.size(); ++i) {
    const result<std::vector<std::uint8_t>> bytes =
        read_file(*fs_, dir_ / "log" / segment_file_name(segments_[i]));
    if (!bytes) {
      return pass(bytes);
    }
    if (bytes->size() <= segment_header_size) {
      continue;
    }
    const byte_span body = std::span(*bytes).subspan(segment_header_size);
    frame_reader r(body, segment_header_size);
    while (const std::optional<frame_view> f = r.next()) {
      n += f->type == frame_type::commit ? 1U : 0U;
    }
  }
  return n;
}

// R4-R7 over one segment; false stops the scan (a finding, or the torn tail).
result<bool> driver::scan_segment_file(std::size_t index) {
  const std::uint32_t number = segments_[index];
  const bool last = index + 1 == segments_.size();
  const std::string name = segment_file_name(number);
  const std::string file = seg_file(name);
  const bool from_checkpoint = checkpoint_ && checkpoint_->segment == number;
  result<vfs_file> f = fs_->open(dir_ / "log" / name, open_read);
  if (!f) {
    return pass(f);
  }
  const result<std::uint64_t> size = f->size();
  if (!size) {
    return pass(size);
  }
  if (*size < segment_header_size) {
    if (!last) {
      finding(corruption_class::c2, errc::journal_corrupt, file, 0, true,
              "segment of " + std::to_string(*size) + " bytes has no complete header");
      return false;
    }
    // A rotation that did not finish: the header is torn (C1).
    tail_.dirty = true;
    tail_.torn_header = true;
    tail_.segment = number;
    tail_.name = name;
    tail_.cut = 0;
    tail_.size = *size;
    tail_.bytes.resize(static_cast<std::size_t>(*size));
    if (result<void> ok = f->read_exact(0, tail_.bytes); !ok) {
      return pass(ok);
    }
    finding(corruption_class::c1, errc::journal_corrupt, file, 0, false,
            "torn segment header of " + std::to_string(*size) + " bytes");
    out_.report.lost_bytes += *size;
    return false;
  }
  std::array<std::uint8_t, segment_header_size> head{};
  if (result<void> ok = f->read_exact(0, head); !ok) {
    return pass(ok);
  }
  if (load_u32(std::span(head).subspan(header_crc_at, 4)) !=
      crc32c(std::span(head).first(header_crc_at))) {
    // The evidence: the intact commits the damaged header hides, here and in later segments.
    std::vector<std::uint8_t> rest(static_cast<std::size_t>(*size - segment_header_size));
    if (result<void> ok = f->read_exact(segment_header_size, rest); !ok) {
      return pass(ok);
    }
    std::uint64_t after = 0;
    frame_reader r(rest, segment_header_size);
    while (const std::optional<frame_view> fr = r.next()) {
      after += fr->type == frame_type::commit ? 1U : 0U;
    }
    const result<std::uint64_t> later = commits_in_later(index);
    if (!later) {
      return pass(later);
    }
    after += *later;
    finding(corruption_class::c2, errc::journal_corrupt, file, 0, true,
            "segment header crc mismatch; " + std::to_string(after) + " intact commits after",
            std::nullopt, std::nullopt, after);
    return false;
  }
  const result<segment_header> h = parse_segment_header(head);
  if (!h) {
    if (h.error().code == errc::format_unsupported) {
      return make_error(errc::format_unsupported,
                        "C8 unknown newer format: " + file + ": " + h.error().message);
    }
    finding(corruption_class::c2, errc::journal_corrupt, file, 0, true, h.error().message);
    return false;
  }
  if (h->minor > journal_minor) {
    return make_error(errc::read_only_newer, file + ": segment minor " + std::to_string(h->minor) +
                                                 " is newer: readable, never written by 1.0");
  }
  const bool seq_ok =
      from_checkpoint ? h->first_seq <= chain_.seq + 1 : h->first_seq == chain_.seq + 1;
  if (h->segment != number || !seq_ok) {
    finding(corruption_class::c3, errc::journal_corrupt, file, 0, true,
            "header names segment " + std::to_string(h->segment) + " first seq " +
                std::to_string(h->first_seq) + ", expected segment " + std::to_string(number) +
                " first seq " + std::to_string(chain_.seq + 1));
    return false;
  }
  const std::uint64_t start = from_checkpoint ? checkpoint_->offset : segment_header_size;
  std::vector<std::uint8_t> body(static_cast<std::size_t>(*size - start));
  if (result<void> ok = f->read_exact(start, body); !ok) {
    return pass(ok);
  }
  out_.report.bytes_scanned += body.size();
  frame_reader r(body, start);
  while (const std::optional<frame_view> fr = r.next()) {
    if (result<void> ok = on_frame(*fr, name, number); !ok) {
      return pass(ok);
    }
    if (blocked()) {
      return false;
    }
  }
  const frame_stop &stop = r.stop();
  if (!stop.clean()) {
    if (stop.fault == frame_fault::unknown_type) {
      finding(corruption_class::c2, errc::journal_corrupt, file, stop.offset, true,
              stop.message + " (an intact frame of a reserved type)");
      return false;
    }
    const resync_result again = resync(body, start, stop.offset);
    if (!last || again.found) {
      const result<std::uint64_t> later = commits_in_later(index);
      if (!later) {
        return pass(later);
      }
      const std::uint64_t after = again.commits + *later;
      finding(corruption_class::c2, errc::journal_corrupt, file, stop.offset, true,
              stop.message + "; " + std::to_string(after) + " intact commits after", std::nullopt,
              std::nullopt, after);
      return false;
    }
    // C1: nothing valid follows the failure in the last segment.
    const std::uint64_t cut = pending_.frames != 0 ? pending_.first_offset : stop.offset;
    tail_.dirty = true;
    tail_.segment = number;
    tail_.name = name;
    tail_.cut = cut;
    tail_.size = *size;
    tail_.bytes.assign(body.begin() + static_cast<std::ptrdiff_t>(cut - start), body.end());
    out_.report.lost_bytes += *size - cut;
    out_.report.truncated_frames += pending_.frames + 1;
    out_.report.orphan_objects += pending_.objects;
    finding(corruption_class::c1, errc::journal_corrupt, file, stop.offset, false,
            stop.message + "; " + std::to_string(*size - cut) + " bytes from offset " +
                std::to_string(cut) + " are cut");
    return false;
  }
  if (pending_.frames != 0) {
    if (!last) {
      finding(corruption_class::c2, errc::journal_corrupt, file, pending_.first_offset, true,
              std::to_string(pending_.frames) + " frames after the last commit end the segment");
      return false;
    }
    // R6: complete frames after the last valid commit are orphans.
    tail_.dirty = true;
    tail_.segment = number;
    tail_.name = name;
    tail_.cut = pending_.first_offset;
    tail_.size = *size;
    tail_.bytes.assign(body.begin() + static_cast<std::ptrdiff_t>(pending_.first_offset - start),
                       body.end());
    out_.report.lost_bytes += *size - pending_.first_offset;
    out_.report.truncated_frames += pending_.frames;
    out_.report.orphan_objects += pending_.objects;
  }
  return true;
}

result<void> driver::scan() {
  std::size_t first = 0;
  if (checkpoint_) {
    first = checkpoint_->segment - 1;
    chain_ = chain_state{.seq = checkpoint_->seq, .last = checkpoint_->last_commit};
    for (const ref_entry &r : checkpoint_->refs) {
      refs_.insert_or_assign(r.name, r.target);
    }
    for (const checkpoint_intent &in : checkpoint_->open_intents) {
      open_.emplace(in.operation, in.kind);
    }
    schema_set_ = checkpoint_->schema_set;
    commit_segment_ = checkpoint_->segment;
    commit_frame_ = checkpoint_->frame_offset;
    commit_end_ = checkpoint_->offset;
    out_.report.used_checkpoint = true;
    out_.report.checkpoint_seq = checkpoint_->seq;
  }
  for (std::size_t i = first; i < segments_.size(); ++i) {
    const result<bool> more = scan_segment_file(i);
    if (!more) {
      return pass(more);
    }
    if (!*more) {
      break;
    }
  }
  return {};
}

result<std::string> driver::free_name(const std::string &base, byte_span same_as, bool compare) {
  const stdfs::path q = dir_ / "quarantine";
  for (int n = 1;; ++n) {
    std::string name = n == 1 ? base : base + "-" + std::to_string(n);
    const result<bool> present = fs_->exists(q / name);
    if (!present) {
      return pass(present);
    }
    if (!*present) {
      return name;
    }
    if (compare) {
      // A run that stopped after its copy: the same bytes under the same name are reused.
      const result<std::vector<std::uint8_t>> old = read_file(*fs_, q / name);
      if (old && std::ranges::equal(*old, same_as)) {
        return name;
      }
    }
  }
}

result<std::string> driver::quarantine_bytes(const std::string &base, byte_span bytes) {
  const stdfs::path q = dir_ / "quarantine";
  if (result<void> ok = ensure_dir(*fs_, q, log_.sync); !ok) {
    return pass(ok);
  }
  const result<std::string> name = free_name(base, bytes, true);
  if (!name) {
    return name;
  }
  {
    result<vfs_file> f = fs_->open(q / *name, open_create);
    if (!f) {
      return pass(f);
    }
    if (!bytes.empty()) {
      if (result<std::uint64_t> at = f->append(bytes); !at) {
        return pass(at);
      }
    }
    if (result<void> ok = sync_file(*f, log_.sync); !ok) {
      return pass(ok);
    }
  }
  if (result<void> ok = sync_dir(*fs_, q, log_.sync); !ok) {
    return pass(ok);
  }
  out_.report.quarantined.push_back("quarantine/" + *name);
  return name;
}

result<std::string> driver::quarantine_move(const stdfs::path &from, const std::string &base) {
  const stdfs::path q = dir_ / "quarantine";
  if (result<void> ok = ensure_dir(*fs_, q, log_.sync); !ok) {
    return pass(ok);
  }
  const result<std::string> name = free_name(base, {}, false);
  if (!name) {
    return name;
  }
  if (result<void> ok = fs_->rename(from, q / *name); !ok) {
    return pass(ok);
  }
  if (result<void> ok = sync_dir(*fs_, q, log_.sync); !ok) {
    return pass(ok);
  }
  if (result<void> ok = sync_dir(*fs_, from.parent_path(), log_.sync); !ok) {
    return pass(ok);
  }
  out_.report.quarantined.push_back("quarantine/" + *name);
  return name;
}

// Loose `.tmp` files are blobs of commits that never reached their commit frame.
result<void> driver::collect_loose_tmp() {
  const stdfs::path loose = dir_ / "loose";
  const result<bool> present = fs_->exists(loose);
  if (!present) {
    return pass(present);
  }
  if (!*present) {
    return {};
  }
  const result<std::vector<std::string>> subs = fs_->list(loose);
  if (!subs) {
    return pass(subs);
  }
  for (const std::string &sub : *subs) {
    const result<std::vector<std::string>> names = fs_->list(loose / sub);
    if (!names) {
      continue; // not a directory
    }
    for (const std::string &name : *names) {
      if (name.ends_with(".tmp")) {
        loose_tmp_.push_back(loose / sub / name);
      }
    }
  }
  return {};
}

// R8.
result<void> driver::repair() {
  const std::string seq = std::to_string(chain_.seq);
  if (tail_.dirty) {
    const std::string base = tail_.torn_header
                                 ? seq + "-header-" + tail_.name
                                 : seq + "-tail-" + tail_.name + "-" + std::to_string(tail_.cut);
    if (!tail_.bytes.empty()) {
      if (result<std::string> q = quarantine_bytes(base, tail_.bytes); !q) {
        return pass(q);
      }
    }
    result<vfs_file> f = fs_->open(dir_ / "log" / tail_.name, open_options{.write = true});
    if (!f) {
      return pass(f);
    }
    if (result<void> ok = f->truncate(tail_.cut); !ok) {
      return ok;
    }
    if (tail_.torn_header) {
      const auto header = encode_segment_header(segment_header{.major = journal_major,
                                                               .minor = journal_minor,
                                                               .segment = tail_.segment,
                                                               .first_seq = chain_.seq + 1});
      if (result<void> ok = f->pwrite(0, header); !ok) {
        return ok;
      }
    }
    if (result<void> ok = sync_file(*f, log_.sync); !ok) {
      return ok;
    }
  }
  for (const stdfs::path &tmp : loose_tmp_) {
    if (result<std::string> q = quarantine_move(tmp, tmp.stem().string() + "-loose-tmp"); !q) {
      return pass(q);
    }
  }
  for (const std::string &name : invalid_checkpoints_) {
    std::string base = seq;
    base += "-checkpoint-";
    base += name;
    if (result<std::string> q = quarantine_move(dir_ / checkpoint_dir_name / name, base); !q) {
      return pass(q);
    }
  }
  // Whatever the scan accepted is made durable before anything builds on it.
  if (!segments_.empty()) {
    result<vfs_file> f =
        fs_->open(dir_ / "log" / segment_file_name(segments_.back()), open_options{.write = true});
    if (!f) {
      return pass(f);
    }
    if (result<void> ok = sync_file(*f, log_.sync); !ok) {
      return ok;
    }
  }
  return sync_dir(*fs_, dir_ / "log", log_.sync);
}

// R9 against the opened store.
result<void> driver::verify_after_open(const log_backend &store) {
  const result<std::vector<ref_entry>> refs = store.list_refs();
  if (!refs) {
    return pass(refs);
  }
  std::vector<ref_entry> mine;
  mine.reserve(refs_.size());
  for (const auto &[name, target] : refs_) {
    mine.push_back(ref_entry{.name = name, .target = target});
  }
  if (*refs != mine) {
    finding(corruption_class::c3, errc::journal_corrupt, "log", 0, true,
            "the refs rebuilt by recovery differ from the store's");
    return {};
  }
  out_.report.heads = mine;
  if (!policy_.verify_links) {
    return {};
  }
  std::set<digest> missing;
  const auto check = [&](const digest &id, std::uint64_t seq,
                         std::string_view what) -> result<void> {
    if (scanned_.contains(id) || missing.contains(id)) {
      return {};
    }
    const result<bool> has = store.has(id);
    if (!has) {
      return pass(has);
    }
    if (!*has) {
      missing.insert(id);
      finding(corruption_class::c5, errc::journal_corrupt, "log", 0, true,
              std::string(what) + " " + id.to_hex() + " named by commit seq " +
                  std::to_string(seq) + " is not stored",
              id);
    }
    return {};
  };
  for (const auto &[id, seq] : links_) {
    if (result<void> ok = check(id, seq, "object"); !ok) {
      return ok;
    }
  }
  for (const ref_entry &r : mine) {
    if (result<void> ok = check(r.target, chain_.seq, "head of ref '" + r.name + "'"); !ok) {
      return ok;
    }
  }
  return {};
}

// R10.
result<void> driver::write_new_checkpoint() {
  if (mode_ != recovery_mode::repair || !policy_.write_checkpoint || chain_.seq == 0 ||
      (checkpoint_ && checkpoint_->seq == chain_.seq && invalid_checkpoints_.empty())) {
    return {};
  }
  checkpoint c;
  c.seq = chain_.seq;
  c.segment = commit_segment_;
  c.offset = commit_end_;
  c.frame_offset = commit_frame_;
  c.last_commit = chain_.last.value_or(digest{});
  for (const auto &[name, target] : refs_) {
    c.refs.push_back(ref_entry{.name = name, .target = target});
  }
  c.schema_set = schema_set_;
  for (const auto &[op, kind] : open_) {
    c.open_intents.push_back(checkpoint_intent{.operation = op, .kind = kind});
  }
  if (result<void> ok = write_checkpoint(*fs_, dir_, c, log_.sync); !ok) {
    return ok;
  }
  out_.report.checkpoint_written = c.seq;
  const result<std::vector<std::string>> names = list_checkpoints(*fs_, dir_);
  if (!names) {
    return pass(names);
  }
  const std::size_t keep = std::max<std::size_t>(policy_.keep_checkpoints, 1);
  for (std::size_t i = keep; i < names->size(); ++i) {
    if (result<void> ok = fs_->remove(dir_ / checkpoint_dir_name / (*names)[i]); !ok) {
      return ok;
    }
  }
  return {};
}

result<recovered_log> driver::run() {
  const result<bool> formatted = fs_->exists(dir_ / "FORMAT");
  if (!formatted) {
    return pass(formatted);
  }
  if (!*formatted) {
    return make_error(errc::not_found,
                      (dir_ / "FORMAT").string() +
                          " is missing: no store here, or its creation did not finish");
  }
  if (result<void> ok = check_format(*fs_, dir_); !ok) {
    return pass(ok);
  }
  if (result<void> ok = list_segments(); !ok) {
    return pass(ok);
  }
  if (blocked()) {
    return std::move(out_);
  }
  if (result<void> ok = choose_checkpoint(); !ok) {
    return pass(ok);
  }
  if (result<void> ok = scan(); !ok) {
    return pass(ok);
  }
  if (result<void> ok = collect_loose_tmp(); !ok) {
    return pass(ok);
  }
  for (const auto &[op, kind] : open_) {
    out_.report.open_intents.push_back(
        open_intent{.operation = op, .kind = kind, .options = {"continue", "compensate"}});
  }
  out_.report.last_seq = chain_.seq;
  if (blocked()) {
    return std::move(out_);
  }
  if (mode_ == recovery_mode::inspect) {
    if (tail_.dirty) {
      return std::move(out_); // the store cannot open before a repair
    }
  } else if (result<void> ok = repair(); !ok) {
    return pass(ok);
  }
  if (!policy_.open_store) {
    for (const auto &[name, target] : refs_) {
      out_.report.heads.push_back(ref_entry{.name = name, .target = target});
    }
    if (links_.empty()) {
      if (result<void> ok = write_new_checkpoint(); !ok) {
        return pass(ok);
      }
    }
    return std::move(out_);
  }
  result<std::unique_ptr<log_backend>> store = log_backend::open(*fs_, dir_, log_);
  if (!store) {
    if (store.error().code != errc::journal_corrupt) {
      return pass(store);
    }
    finding(corruption_class::c2, errc::journal_corrupt, "log", 0, true,
            "the store refused to open: " + store.error().message);
    return std::move(out_);
  }
  if (result<void> ok = verify_after_open(**store); !ok) {
    return pass(ok);
  }
  if (blocked()) {
    return std::move(out_);
  }
  if (result<void> ok = write_new_checkpoint(); !ok) {
    return pass(ok);
  }
  out_.store = std::move(*store);
  return std::move(out_);
}

} // namespace

result<recovered_log> recover_log(vfs &fs, const std::filesystem::path &store_dir,
                                  const recovery_policy &policy, const log_options &log,
                                  recovery_mode mode) {
  driver d(fs, store_dir, policy, log, mode);
  return d.run();
}

} // namespace sl::fabric
