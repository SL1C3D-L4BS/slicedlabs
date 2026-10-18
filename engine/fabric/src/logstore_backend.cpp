// The durable log backend (WO-053, SL-ARCH-002 ARCH2-R28 to R31 and R33, ADR-0014, ADR-0015).
// See log_backend.hpp for the layout and the protocol.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/f5.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/journal.hpp"
#include "sl/fabric/log_backend.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/vfs.hpp"

namespace sl::fabric {

namespace {

// Where an object's SLFO frame lives.
struct location {
  std::uint32_t segment = 0; // index into state::segments (framed objects)
  std::uint64_t offset = 0;  // file offset of the SLFO frame
  std::uint64_t length = 0;  // SLFO frame bytes
  bool loose = false;
};

struct segment_file {
  std::uint32_t number = 0;
  vfs_file file;
  std::uint64_t size = 0; // bytes up to the end of the last frame
};

std::filesystem::path log_dir(const std::filesystem::path &dir) { return dir / "log"; }
std::filesystem::path loose_dir(const std::filesystem::path &dir) { return dir / "loose"; }

std::filesystem::path loose_path(const std::filesystem::path &dir, const digest &id) {
  const std::string hex = id.to_hex();
  return loose_dir(dir) / hex.substr(0, 2) / hex;
}

std::filesystem::path parent_or_dot(const std::filesystem::path &p) {
  const std::filesystem::path parent = p.parent_path();
  return parent.empty() ? std::filesystem::path(".") : parent;
}

std::unexpected<error> with_context(const error &e, std::string_view context) {
  std::string m(context);
  m += ": ";
  m += e.message;
  return make_error(e.code, std::move(m), e.where);
}

std::unexpected<error> fail(errc code, std::string_view context, std::string_view detail) {
  std::string m(context);
  m += ": ";
  m += detail;
  return make_error(code, std::move(m));
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

result<void> sync_dir(vfs &fs, const std::filesystem::path &dir, durability d) noexcept {
  if (d == durability::none) {
    return {};
  }
  return fs.sync_dir(dir);
}

// make_dir that accepts an existing directory.
result<void> ensure_dir(vfs &fs, const std::filesystem::path &dir) noexcept {
  result<void> made = fs.make_dir(dir);
  if (!made && made.error().code != errc::already_exists) {
    return made;
  }
  return {};
}

// Writes `bytes` to a new file and syncs it.
result<void> write_synced(vfs &fs, const std::filesystem::path &path, byte_span bytes,
                          durability d) noexcept {
  result<vfs_file> f = fs.open(path, open_create);
  if (!f) {
    return std::unexpected(std::move(f.error()));
  }
  if (result<std::uint64_t> at = f->append(bytes); !at) {
    return std::unexpected(std::move(at.error()));
  }
  return sync_file(*f, d);
}

// A loose blob must exist and verify as `id`.
result<void> verify_loose(vfs &fs, const std::filesystem::path &dir, const digest &id) noexcept {
  const result<std::vector<std::uint8_t>> bytes = read_file(fs, loose_path(dir, id));
  if (!bytes) {
    return fail(errc::journal_corrupt, "loose blob " + id.to_hex(),
                bytes.error().code == errc::not_found ? "missing" : bytes.error().message);
  }
  const result<object> o = parse_object(*bytes);
  if (!o) {
    return fail(errc::journal_corrupt, "loose blob " + id.to_hex(), o.error().message);
  }
  if (o->id != id) {
    return fail(errc::journal_corrupt, "loose blob " + id.to_hex(), "hashes to " + o->id.to_hex());
  }
  return {};
}

// The id of the first object of `kind` in the batch, or the zero digest.
digest first_of_kind(const std::vector<object> &objects, object_kind kind) noexcept {
  for (const object &o : objects) {
    if (o.header.kind == kind) {
      return o.id;
    }
  }
  return digest{};
}

std::string ref_text(std::string_view name) {
  std::string m("ref '");
  m += name;
  m += '\'';
  return m;
}

// Creates a fresh `.slfabric` directory: log/ with segment 1, loose/, and FORMAT last.
result<void> create_layout(vfs &fs, const std::filesystem::path &dir, durability d) noexcept {
  const result<bool> present = fs.exists(dir);
  if (!present) {
    return std::unexpected(present.error());
  }
  if (*present) {
    // Only a creation that stopped before FORMAT may be redone: at most an empty segment 1.
    const result<bool> has_log = fs.exists(log_dir(dir));
    if (!has_log) {
      return std::unexpected(has_log.error());
    }
    if (*has_log) {
      const result<std::vector<std::string>> names = fs.list(log_dir(dir));
      if (!names) {
        return std::unexpected(names.error());
      }
      for (const std::string &name : *names) {
        const std::filesystem::path p = log_dir(dir) / name;
        const result<std::vector<std::uint8_t>> bytes = read_file(fs, p);
        if (name != segment_file_name(1) || !bytes || bytes->size() > segment_header_size) {
          return fail(errc::recovery_required, dir.string(),
                      "no FORMAT file but log/ holds " + name +
                          " (an interrupted creation "
                          "with data; run recovery)");
        }
      }
    }
  } else {
    if (result<void> ok = fs.make_dir(dir); !ok) {
      return with_context(ok.error(), "create " + dir.string());
    }
    if (result<void> ok = sync_dir(fs, parent_or_dot(dir), d); !ok) {
      return std::unexpected(std::move(ok.error()));
    }
  }
  if (result<void> ok = ensure_dir(fs, log_dir(dir)); !ok) {
    return std::unexpected(std::move(ok.error()));
  }
  if (result<void> ok = ensure_dir(fs, loose_dir(dir)); !ok) {
    return std::unexpected(std::move(ok.error()));
  }
  const auto header = encode_segment_header(segment_header{});
  if (result<void> ok = write_synced(fs, log_dir(dir) / segment_file_name(1), header, d); !ok) {
    return std::unexpected(std::move(ok.error()));
  }
  if (result<void> ok = sync_dir(fs, log_dir(dir), d); !ok) {
    return std::unexpected(std::move(ok.error()));
  }
  const std::span<const std::uint8_t> text(
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): chars viewed as their bytes
      reinterpret_cast<const std::uint8_t *>(format_text.data()), format_text.size());
  if (result<void> ok = write_synced(fs, dir / "FORMAT.tmp", text, d); !ok) {
    return std::unexpected(std::move(ok.error()));
  }
  if (result<void> ok = fs.rename(dir / "FORMAT.tmp", dir / "FORMAT"); !ok) {
    return std::unexpected(std::move(ok.error()));
  }
  return sync_dir(fs, dir, d);
}

// FORMAT must read "slfabric 1.0" (ARCH2-R28, C8).
result<void> check_format(vfs &fs, const std::filesystem::path &dir) noexcept {
  const result<std::vector<std::uint8_t>> bytes = read_file(fs, dir / "FORMAT");
  if (!bytes) {
    return with_context(bytes.error(), "read FORMAT");
  }
  std::string text(bytes->begin(), bytes->end());
  if (text == format_text) {
    return {};
  }
  if (text.ends_with('\n')) {
    text.pop_back();
  }
  constexpr std::string_view prefix = "slfabric ";
  const std::size_t dot = text.find('.');
  if (!text.starts_with(prefix) || dot == std::string::npos || dot <= prefix.size() ||
      dot + 1 >= text.size()) {
    return fail(errc::store_corrupt, (dir / "FORMAT").string(), "unrecognised text");
  }
  const std::string major = text.substr(prefix.size(), dot - prefix.size());
  const std::string minor = text.substr(dot + 1);
  for (const std::string *part : {&major, &minor}) {
    for (const char c : *part) {
      if (c < '0' || c > '9') {
        return fail(errc::store_corrupt, (dir / "FORMAT").string(), "unrecognised text");
      }
    }
  }
  if (major != "1") {
    return fail(errc::format_unsupported, (dir / "FORMAT").string(),
                "store format " + major + "." + minor + ", supported 1.0");
  }
  if (minor != "0") {
    return fail(errc::read_only_newer, (dir / "FORMAT").string(),
                "store format 1." + minor + " is newer: readable, never written by 1.0");
  }
  return {};
}

} // namespace

struct log_backend::state {
  vfs *fs = nullptr;
  std::filesystem::path dir;
  log_options options;
  std::shared_mutex mutex;
  std::vector<segment_file> segments;
  std::map<digest, location> index;
  std::map<std::string, digest, std::less<>> refs;
  std::vector<intent_record> intents;
  std::set<record_id> open;
  chain_state chain;
  bool broken = false;
  std::string broken_reason;
  std::set<std::string> durable_loose_dirs; // loose/ab entries this backend has synced
};

namespace {

// What a segment scan has seen since the last commit.
struct pending_frames {
  std::vector<std::pair<digest, location>> objects;
  std::vector<intent_record> intents;
  objects_hasher hasher;
  std::uint64_t frames = 0;
  std::uint64_t first_offset = 0;

  void clear() noexcept {
    objects.clear();
    intents.clear();
    hasher.reset();
    frames = 0;
  }
};

// Applies one commit frame found by the scan (ARCH2-R30), or refuses it.
template <class State>
result<void> replay_commit(State &s, pending_frames &pending, byte_span payload,
                           std::uint64_t offset, std::string_view where) {
  const std::string context = std::string(where) + " offset " + std::to_string(offset);
  result<commit_record> c = decode_commit(payload);
  if (!c) {
    return with_context(c.error(), context);
  }
  if (result<void> ok = check_commit(*c, s.chain, pending.hasher); !ok) {
    return with_context(ok.error(), context);
  }
  for (const digest &id : c->loose) {
    if (result<void> ok = verify_loose(*s.fs, s.dir, id); !ok) {
      return with_context(ok.error(), context);
    }
  }
  std::set<digest> incoming(c->loose.begin(), c->loose.end());
  for (const auto &entry : pending.objects) {
    incoming.insert(entry.first);
  }
  for (const ref_update &r : c->refs) {
    const auto it = s.refs.find(r.name);
    const std::optional<digest> current =
        it == s.refs.end() ? std::nullopt : std::optional<digest>(it->second);
    if (current != r.expected) {
      return fail(errc::journal_corrupt, context,
                  "commit seq " + std::to_string(c->seq) + " moves " + ref_text(r.name) +
                      " from an unexpected head");
    }
    if (!incoming.contains(r.desired) && !s.index.contains(r.desired)) {
      return fail(errc::journal_corrupt, context,
                  "commit seq " + std::to_string(c->seq) + ": " + ref_text(r.name) + " target " +
                      r.desired.to_hex() + " is not stored");
    }
  }
  std::set<record_id> open = s.open;
  for (const intent_record &in : pending.intents) {
    const bool ok = in.phase == intent_phase::begin ? open.insert(in.operation).second
                                                    : open.erase(in.operation) != 0;
    if (!ok) {
      return fail(errc::journal_corrupt, context,
                  "intent " + in.operation.to_string() + " does not bracket correctly");
    }
  }
  for (const auto &[id, loc] : pending.objects) {
    s.index.try_emplace(id, loc);
  }
  for (const digest &id : c->loose) {
    s.index.try_emplace(id, location{.segment = 0, .offset = 0, .length = 0, .loose = true});
  }
  for (const ref_update &r : c->refs) {
    s.refs.insert_or_assign(r.name, r.desired);
  }
  s.intents.insert(s.intents.end(), pending.intents.begin(), pending.intents.end());
  s.open = std::move(open);
  s.chain = chain_state{.seq = c->seq, .last = commit_digest(payload)};
  pending.clear();
  return {};
}

} // namespace

log_backend::log_backend(open_key /*key*/, std::unique_ptr<state> s) noexcept
    : state_(std::move(s)) {}

log_backend::~log_backend() = default;

result<std::unique_ptr<log_backend>> log_backend::open(vfs &fs, const std::filesystem::path &dir,
                                                       durability sync) noexcept {
  return open(fs, dir, log_options{.sync = sync});
}

result<std::unique_ptr<log_backend>> log_backend::open(vfs &fs, const std::filesystem::path &dir,
                                                       const log_options &options) noexcept {
  const result<bool> formatted = fs.exists(dir / "FORMAT");
  if (!formatted) {
    return std::unexpected(formatted.error());
  }
  if (!*formatted) {
    if (result<void> ok = create_layout(fs, dir, options.sync); !ok) {
      return std::unexpected(std::move(ok.error()));
    }
  }
  if (result<void> ok = check_format(fs, dir); !ok) {
    return std::unexpected(std::move(ok.error()));
  }
  if (result<void> ok = ensure_dir(fs, loose_dir(dir)); !ok) {
    return std::unexpected(std::move(ok.error()));
  }
  const result<std::vector<std::string>> names = fs.list(log_dir(dir));
  if (!names) {
    return with_context(names.error(), "list " + log_dir(dir).string());
  }
  std::vector<std::uint32_t> numbers;
  for (const std::string &name : *names) {
    const std::optional<std::uint32_t> n = parse_segment_file_name(name);
    if (!n) {
      return fail(errc::journal_corrupt, log_dir(dir).string(), "unexpected file " + name);
    }
    numbers.push_back(*n);
  }
  std::ranges::sort(numbers);
  if (numbers.empty()) {
    return fail(errc::journal_corrupt, log_dir(dir).string(), "no segment files");
  }
  for (std::size_t i = 0; i < numbers.size(); ++i) {
    if (numbers[i] != i + 1) {
      return fail(errc::journal_corrupt, log_dir(dir).string(),
                  "segment " + std::to_string(i + 1) + " is missing");
    }
  }

  auto s = std::make_unique<state>();
  s->fs = &fs;
  s->dir = dir;
  s->options = options;
  pending_frames pending;
  for (std::size_t i = 0; i < numbers.size(); ++i) {
    const bool last = i + 1 == numbers.size();
    const std::uint32_t number = numbers[i];
    const std::string name = segment_file_name(number);
    const std::filesystem::path path = log_dir(dir) / name;
    result<vfs_file> file = fs.open(path, last ? open_append : open_read);
    if (!file) {
      return with_context(file.error(), "open " + path.string());
    }
    const result<std::uint64_t> size = file->size();
    if (!size) {
      return std::unexpected(size.error());
    }
    if (*size < segment_header_size) {
      return fail(last ? errc::recovery_required : errc::journal_corrupt, name,
                  "torn segment header (" + std::to_string(*size) + " bytes)");
    }
    std::array<std::uint8_t, segment_header_size> head{};
    if (result<void> ok = file->read_exact(0, head); !ok) {
      return std::unexpected(std::move(ok.error()));
    }
    const result<segment_header> h = parse_segment_header(head);
    if (!h) {
      return with_context(h.error(), name);
    }
    if (h->minor > journal_minor) {
      return fail(errc::read_only_newer, name,
                  "segment minor " + std::to_string(h->minor) +
                      " is newer: readable, never written by 1.0");
    }
    if (h->segment != number) {
      return fail(errc::journal_corrupt, name,
                  "header names segment " + std::to_string(h->segment));
    }
    if (h->first_seq != s->chain.seq + 1) {
      return fail(errc::journal_corrupt, name,
                  "header first seq " + std::to_string(h->first_seq) + ", expected " +
                      std::to_string(s->chain.seq + 1) + " (ARCH2-R30)");
    }
    const auto index = static_cast<std::uint32_t>(i);
    state &st = *s;
    const frame_visitor visit = [&st, &pending, &name, index](const frame_view &f) -> result<void> {
      if (pending.frames == 0) {
        pending.first_offset = f.offset;
      }
      switch (f.type) {
      case frame_type::object: {
        const result<object> o = parse_object(f.payload);
        if (!o) {
          return fail(errc::journal_corrupt, name + " offset " + std::to_string(f.offset),
                      "object frame: " + o.error().message);
        }
        pending.objects.emplace_back(o->id, location{.segment = index,
                                                     .offset = f.offset + frame_prefix_size,
                                                     .length = f.payload.size(),
                                                     .loose = false});
        pending.hasher.add(o->id);
        ++pending.frames;
        return {};
      }
      case frame_type::intent_begin:
      case frame_type::intent_end: {
        result<intent_record> in = decode_intent(f.type, f.payload);
        if (!in) {
          return with_context(in.error(), name + " offset " + std::to_string(f.offset));
        }
        pending.intents.push_back(std::move(*in));
        ++pending.frames;
        return {};
      }
      case frame_type::commit:
        return replay_commit(st, pending, f.payload, f.offset, name);
      case frame_type::note:
        return {};
      }
      return {};
    };
    const result<frame_stop> stop = scan_segment(*file, segment_header_size, visit);
    if (!stop) {
      return std::unexpected(stop.error());
    }
    if (!stop->clean()) {
      const bool torn = last && stop->fault != frame_fault::unknown_type;
      return fail(torn ? errc::recovery_required : errc::journal_corrupt, name,
                  (torn ? "torn tail: " : "") + stop->message + " after commit seq " +
                      std::to_string(s->chain.seq));
    }
    if (pending.frames != 0) {
      return fail(last ? errc::recovery_required : errc::journal_corrupt, name,
                  std::to_string(pending.frames) + " frames from offset " +
                      std::to_string(pending.first_offset) + " follow the last commit (seq " +
                      std::to_string(s->chain.seq) + ")");
    }
    s->segments.push_back(
        segment_file{.number = number, .file = std::move(*file), .size = stop->end});
  }
  return std::make_unique<log_backend>(open_key{}, std::move(s));
}

result<object> log_backend::get(const digest &id) const noexcept {
  location loc;
  std::uint64_t handle = 0;
  {
    const std::shared_lock lock(state_->mutex);
    const auto it = state_->index.find(id);
    if (it == state_->index.end()) {
      return make_error(errc::object_missing, "object " + id.to_hex() + " is not stored");
    }
    loc = it->second;
    if (!loc.loose) {
      handle = state_->segments[loc.segment].file.handle();
    }
  }
  std::vector<std::uint8_t> frame;
  if (loc.loose) {
    result<std::vector<std::uint8_t>> bytes = read_file(*state_->fs, loose_path(state_->dir, id));
    if (!bytes) {
      return make_error(bytes.error().code == errc::not_found ? errc::object_missing
                                                              : errc::store_io,
                        "object " + id.to_hex() + ": loose file: " + bytes.error().message);
    }
    frame = std::move(*bytes);
  } else {
    frame.resize(static_cast<std::size_t>(loc.length));
    std::size_t done = 0;
    while (done < frame.size()) {
      const result<std::size_t> n =
          state_->fs->read(handle, loc.offset + done, std::span(frame).subspan(done));
      if (!n) {
        return make_error(errc::store_io, "object " + id.to_hex() + ": " + n.error().message);
      }
      if (*n == 0) {
        return make_error(errc::object_corrupt,
                          "object " + id.to_hex() + ": the segment ends inside its frame");
      }
      done += *n;
    }
  }
  result<object> parsed = parse_object(frame);
  if (!parsed) {
    return make_error(errc::object_corrupt,
                      "object " + id.to_hex() +
                          ": stored frame does not parse: " + parsed.error().message);
  }
  if (parsed->id != id) {
    return make_error(errc::object_corrupt,
                      "object " + id.to_hex() + ": stored bytes hash to " + parsed->id.to_hex());
  }
  return parsed;
}

result<bool> log_backend::has(const digest &id) const noexcept {
  const std::shared_lock lock(state_->mutex);
  return state_->index.contains(id);
}

result<digest> log_backend::read_ref(std::string_view name) const noexcept {
  const std::shared_lock lock(state_->mutex);
  const auto it = state_->refs.find(name);
  if (it == state_->refs.end()) {
    return make_error(errc::not_found, "no " + ref_text(name));
  }
  return it->second;
}

result<std::vector<ref_entry>> log_backend::list_refs() const noexcept {
  const std::shared_lock lock(state_->mutex);
  std::vector<ref_entry> out;
  out.reserve(state_->refs.size());
  for (const auto &[name, target] : state_->refs) {
    out.push_back(ref_entry{.name = name, .target = target});
  }
  return out;
}

result<commit_ack> log_backend::commit(const mutation_capability & /*cap*/,
                                       commit_batch batch) noexcept {
  // F5 ledger (WO-051): every call is a backend mutation; outside the manager it is a bypass.
  f5::note_mutation("log_backend::commit", this);
  if (!batch.intents.empty()) {
    f5::note_mutation("log_backend::commit intents", this);
  }
  if (result<void> checked = check_batch(batch); !checked) {
    return std::unexpected(checked.error());
  }
  const std::unique_lock lock(state_->mutex);
  state &s = *state_;
  if (s.broken) {
    return fail(errc::recovery_required, "log_backend", s.broken_reason);
  }

  // Everything that can refuse the batch runs before the first write.
  std::set<digest> incoming;
  for (const object &o : batch.objects) {
    incoming.insert(o.id);
  }
  for (const ref_update &r : batch.refs) {
    const auto it = s.refs.find(r.name);
    if (r.expected) {
      if (it == s.refs.end()) {
        return make_error(errc::ref_cas_failed,
                          ref_text(r.name) + " does not exist, expected " + r.expected->to_hex());
      }
      if (it->second != *r.expected) {
        return make_error(errc::ref_cas_failed, ref_text(r.name) + " is at " + it->second.to_hex() +
                                                    ", expected " + r.expected->to_hex());
      }
    } else if (it != s.refs.end()) {
      return make_error(errc::ref_cas_failed, ref_text(r.name) + " already exists at " +
                                                  it->second.to_hex() + " (create-if-absent)");
    }
    if (!incoming.contains(r.desired) && !s.index.contains(r.desired)) {
      return make_error(errc::object_missing, ref_text(r.name) + " target " + r.desired.to_hex() +
                                                  " is neither stored nor in the batch");
    }
  }
  std::set<record_id> open = s.open;
  for (const intent_record &in : batch.intents) {
    if (in.phase == intent_phase::begin && !open.insert(in.operation).second) {
      return make_error(errc::invalid_argument,
                        "intent " + in.operation.to_string() + " is already open");
    }
    if (in.phase == intent_phase::end && open.erase(in.operation) == 0) {
      return make_error(errc::invalid_argument,
                        "intent " + in.operation.to_string() + " ends without a begin");
    }
  }

  // New objects in batch order: framed, or loose above the threshold.
  struct placed {
    digest id;
    std::vector<std::uint8_t> frame;
    std::uint64_t at = 0; // offset of the SLFO frame inside `buffer`
  };
  std::vector<placed> framed;
  std::vector<placed> loose;
  std::set<digest> seen;
  for (const object &o : batch.objects) {
    if (s.index.contains(o.id) || !seen.insert(o.id).second) {
      continue; // already stored: idempotent
    }
    std::vector<std::uint8_t> frame = frame_object(o.header, o.payload);
    (frame.size() > s.options.loose_above ? loose : framed)
        .push_back(placed{.id = o.id, .frame = std::move(frame), .at = 0});
  }
  const std::uint64_t seq = s.chain.seq + 1;
  const std::string context = "commit seq " + std::to_string(seq);

  // Loose blobs are durable before the frames that name them (ADR-0015).
  for (const placed &p : loose) {
    const std::filesystem::path final_path = loose_path(s.dir, p.id);
    const std::filesystem::path sub = final_path.parent_path();
    // The loose/ab entry must be durable before a commit names a blob in it: synced once per
    // backend, even when an earlier failed commit already created it.
    const std::string sub_name = sub.filename().string();
    if (!s.durable_loose_dirs.contains(sub_name)) {
      const result<bool> sub_exists = s.fs->exists(sub);
      if (!sub_exists) {
        return with_context(sub_exists.error(), context);
      }
      if (!*sub_exists) {
        if (result<void> ok = ensure_dir(*s.fs, sub); !ok) {
          return with_context(ok.error(), context);
        }
      }
      if (result<void> ok = sync_dir(*s.fs, loose_dir(s.dir), s.options.sync); !ok) {
        return with_context(ok.error(), context);
      }
      s.durable_loose_dirs.insert(sub_name);
    }
    std::filesystem::path tmp = final_path;
    tmp += ".tmp";
    if (result<void> ok = write_synced(*s.fs, tmp, p.frame, s.options.sync); !ok) {
      return with_context(ok.error(), context + ": loose blob " + p.id.to_hex());
    }
    if (result<void> ok = s.fs->rename(tmp, final_path); !ok) {
      return with_context(ok.error(), context + ": loose blob " + p.id.to_hex());
    }
    if (result<void> ok = sync_dir(*s.fs, sub, s.options.sync); !ok) {
      return with_context(ok.error(), context + ": loose blob " + p.id.to_hex());
    }
  }

  // Object frames, intent frames, then the commit frame, in one buffer.
  std::vector<std::uint8_t> buffer;
  objects_hasher hasher;
  const auto put = [&buffer](const result<std::vector<std::uint8_t>> &f) -> result<void> {
    if (!f) {
      return std::unexpected(f.error());
    }
    buffer.insert(buffer.end(), f->begin(), f->end());
    return {};
  };
  for (placed &p : framed) {
    p.at = buffer.size() + frame_prefix_size;
    if (result<void> ok = put(encode_frame(frame_type::object, p.frame)); !ok) {
      return with_context(ok.error(), context);
    }
    hasher.add(p.id);
  }
  for (const intent_record &in : batch.intents) {
    const result<std::vector<std::uint8_t>> payload = encode_intent(in);
    if (!payload) {
      return with_context(payload.error(), context);
    }
    const frame_type t =
        in.phase == intent_phase::begin ? frame_type::intent_begin : frame_type::intent_end;
    if (result<void> ok = put(encode_frame(t, *payload)); !ok) {
      return with_context(ok.error(), context);
    }
  }
  commit_record rec;
  rec.seq = seq;
  rec.prev = s.chain.last;
  rec.tx_record = first_of_kind(batch.objects, object_kind::tx);
  rec.revision = first_of_kind(batch.objects, object_kind::revision);
  rec.refs = batch.refs;
  rec.object_count = hasher.count();
  rec.objects_digest = hasher.finish();
  for (const placed &p : loose) {
    rec.loose.push_back(p.id);
  }
  rec.commit_time_us = batch.commit_time_us;
  const result<std::vector<std::uint8_t>> payload = encode_commit(rec);
  if (!payload) {
    return with_context(payload.error(), context);
  }
  if (result<void> ok = put(encode_frame(frame_type::commit, *payload)); !ok) {
    return with_context(ok.error(), context);
  }

  // A commit never spans segments: rotate first when it would grow the segment past rotate_at.
  if (s.segments.back().size > segment_header_size &&
      s.segments.back().size + buffer.size() > s.options.rotate_at) {
    const std::uint32_t number = s.segments.back().number + 1;
    const std::filesystem::path path = log_dir(s.dir) / segment_file_name(number);
    result<vfs_file> file = s.fs->open(
        path,
        open_options{
            .write = true, .create = true, .exclusive = false, .truncate = true, .append = true});
    result<void> ok = file ? result<void>{} : result<void>(std::unexpected(file.error()));
    if (ok) {
      const auto header = encode_segment_header(segment_header{
          .major = journal_major, .minor = journal_minor, .segment = number, .first_seq = seq});
      const result<std::uint64_t> at = file->append(header);
      ok = at ? sync_file(*file, s.options.sync) : result<void>(std::unexpected(at.error()));
    }
    if (ok) {
      ok = sync_dir(*s.fs, log_dir(s.dir), s.options.sync);
    }
    if (!ok) {
      s.broken = true;
      s.broken_reason = context + ": rotating to " + path.filename().string() + " failed (" +
                        ok.error().message + "); reopen to recover";
      return fail(errc::store_io, context, s.broken_reason);
    }
    s.segments.push_back(
        segment_file{.number = number, .file = std::move(*file), .size = segment_header_size});
  }

  segment_file &seg = s.segments.back();
  const result<std::uint64_t> at = seg.file.append(buffer);
  if (!at) {
    s.broken = true;
    s.broken_reason = context + ": append failed (" + at.error().message +
                      "); the log tail is unknown, reopen to recover";
    return fail(errc::store_io, context, s.broken_reason);
  }
  if (result<void> ok = sync_file(seg.file, s.options.sync); !ok || *at != seg.size) {
    s.broken = true;
    s.broken_reason = context +
                      (ok ? ": the segment grew outside this backend"
                          : ": sync failed (" + ok.error().message + ")") +
                      "; the commit is decided by frame intactness on reopen";
    return fail(errc::commit_indeterminate, context, s.broken_reason);
  }

  // The commit frame is durable: publish (ARCH2-R07 C4).
  const auto segment_index = static_cast<std::uint32_t>(s.segments.size() - 1);
  for (const placed &p : framed) {
    s.index.try_emplace(p.id, location{.segment = segment_index,
                                       .offset = *at + p.at,
                                       .length = p.frame.size(),
                                       .loose = false});
  }
  for (const placed &p : loose) {
    s.index.try_emplace(
        p.id, location{.segment = 0, .offset = 0, .length = p.frame.size(), .loose = true});
  }
  for (ref_update &r : batch.refs) {
    s.refs.insert_or_assign(std::move(r.name), r.desired);
  }
  for (intent_record &in : batch.intents) {
    s.intents.push_back(std::move(in));
  }
  s.open = std::move(open);
  s.chain = chain_state{.seq = seq, .last = commit_digest(*payload)};
  seg.size += buffer.size();
  return commit_ack{.seq = seq};
}

result<verify_report> log_backend::verify(const verify_scope &scope) const noexcept {
  std::vector<digest> stored;
  {
    const std::shared_lock lock(state_->mutex);
    stored.reserve(state_->index.size());
    for (const auto &entry : state_->index) {
      stored.push_back(entry.first);
    }
  }
  return verify_store(*this, scope, stored);
}

std::size_t log_backend::object_count() const noexcept {
  const std::shared_lock lock(state_->mutex);
  return state_->index.size();
}

std::uint64_t log_backend::last_seq() const noexcept {
  const std::shared_lock lock(state_->mutex);
  return state_->chain.seq;
}

std::optional<digest> log_backend::last_commit_digest() const noexcept {
  const std::shared_lock lock(state_->mutex);
  return state_->chain.last;
}

std::uint32_t log_backend::segment_count() const noexcept {
  const std::shared_lock lock(state_->mutex);
  return static_cast<std::uint32_t>(state_->segments.size());
}

std::vector<intent_record> log_backend::intents() const {
  const std::shared_lock lock(state_->mutex);
  return state_->intents;
}

std::vector<record_id> log_backend::open_intents() const {
  const std::shared_lock lock(state_->mutex);
  return {state_->open.begin(), state_->open.end()};
}

bool log_backend::needs_recovery() const noexcept {
  const std::shared_lock lock(state_->mutex);
  return state_->broken;
}

} // namespace sl::fabric
