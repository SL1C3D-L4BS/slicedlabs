// Planted corruption for the corruption tests (WO-059). See corrupt_store.hpp.

#include "corrupt_store.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "crash_harness.hpp"
#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/checkpoint.hpp"
#include "sl/fabric/clock.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/journal.hpp"
#include "sl/fabric/log_backend.hpp"
#include "sl/fabric/recovery.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/tree.hpp"
#include "sl/fabric/vfs.hpp"
#include "sl/fabric/workspace.hpp"
#include "tx_fixture.hpp"

namespace sl::fabric::testing {

namespace {

namespace stdfs = std::filesystem;

template <class T> std::unexpected<error> pass(const result<T> &r) {
  return std::unexpected(r.error());
}

result<std::uint32_t> last_segment(vfs &fs, const stdfs::path &dir) {
  const result<std::vector<std::string>> names = fs.list(dir / "log");
  if (!names) {
    return pass(names);
  }
  std::uint32_t last = 0;
  for (const std::string &n : *names) {
    if (const std::optional<std::uint32_t> s = parse_segment_file_name(n)) {
      last = std::max(last, *s);
    }
  }
  if (last == 0) {
    return make_error(errc::not_found, "no segment");
  }
  return last;
}

std::string segment_rel(std::uint32_t n) { return "log/" + segment_file_name(n); }

result<void> flip_byte(vfs &fs, const stdfs::path &path, std::uint64_t offset) {
  result<std::vector<std::uint8_t>> bytes = read_file(fs, path);
  if (!bytes) {
    return pass(bytes);
  }
  if (offset >= bytes->size()) {
    return make_error(errc::invalid_argument, "flip past the end of " + path.string());
  }
  (*bytes)[static_cast<std::size_t>(offset)] ^= 0x5aU;
  return write_bytes(fs, path, *bytes);
}

// The loose blobs of the store, sorted by path relative to the store directory.
result<std::vector<std::string>> loose_blobs(vfs &fs, const stdfs::path &dir) {
  std::vector<std::string> out;
  const result<bool> present = fs.exists(dir / "loose");
  if (!present) {
    return pass(present);
  }
  if (!*present) {
    return out;
  }
  const result<std::vector<std::string>> fans = fs.list(dir / "loose");
  if (!fans) {
    return pass(fans);
  }
  for (const std::string &fan : *fans) {
    const result<std::vector<std::string>> names = fs.list(dir / "loose" / fan);
    if (!names) {
      return pass(names);
    }
    for (const std::string &n : *names) {
      std::string rel = "loose/";
      rel += fan;
      rel += '/';
      rel += n;
      out.push_back(std::move(rel));
    }
  }
  std::ranges::sort(out);
  return out;
}

result<planted> first_loose(vfs &fs, const stdfs::path &dir) {
  const result<std::vector<std::string>> blobs = loose_blobs(fs, dir);
  if (!blobs) {
    return pass(blobs);
  }
  if (blobs->empty()) {
    return make_error(errc::not_found, "the store holds no loose blob");
  }
  planted p;
  p.file = blobs->front();
  const result<std::vector<std::uint8_t>> bytes = read_file(fs, dir / p.file);
  if (!bytes) {
    return pass(bytes);
  }
  const result<object> o = parse_object(*bytes);
  if (!o) {
    return pass(o);
  }
  p.oid = o->id;
  p.length = bytes->size();
  return p;
}

} // namespace

result<void> write_bytes(vfs &fs, const stdfs::path &path, byte_span bytes) {
  result<vfs_file> f = fs.open(path, open_options{.write = true, .create = true, .truncate = true});
  if (!f) {
    return pass(f);
  }
  return f->pwrite(0, bytes);
}

result<workspace_options> sample_options(std::shared_ptr<vfs> fs, const sample_config &cfg) {
  result<workspace_options> o = scene_options(std::make_shared<manual_clock>(1'000'000), 59);
  if (!o) {
    return o;
  }
  o->fs = std::move(fs);
  o->log.sync = durability::none;
  o->log.loose_above = cfg.loose_above;
  o->log.rotate_at = cfg.rotate_at;
  o->omit_builtin = cfg.omit_builtin;
  return o;
}

result<sample_store> make_sample_store(const sample_config &cfg) {
  sample_store s;
  s.fs = std::make_shared<vfs_memory>();
  s.root = "/cor";
  s.dir = s.root / workspace::store_dir_name;
  result<workspace_options> o = sample_options(s.fs, cfg);
  if (!o) {
    return pass(o);
  }
  result<std::unique_ptr<workspace>> ws = workspace::create(s.root, std::move(*o));
  if (!ws) {
    return pass(ws);
  }
  transaction_manager &m = (*ws)->manager();
  const record_id e1 = entity_named("e1");
  for (std::size_t k = 1; k <= std::max<std::size_t>(cfg.commits, 1); ++k) {
    std::vector<patch_op> ops;
    if (k == 1) {
      result<patch_op> mesh = create_mesh("m0", 8);
      result<patch_op> entity = create_entity("e1");
      if (!mesh || !entity) {
        return make_error(errc::invalid_argument, "sample ops");
      }
      ops.push_back(std::move(*mesh));
      ops.push_back(std::move(*entity));
    } else {
      result<patch_op> entity = create_entity("e" + std::to_string(k));
      result<patch_op> rename = set_name(e1, "e1 v" + std::to_string(k));
      if (!entity || !rename) {
        return make_error(errc::invalid_argument, "sample ops");
      }
      ops.push_back(std::move(*entity));
      ops.push_back(std::move(*rename));
    }
    result<commit_receipt> r = commit_ops(m, tx_origin_of(tx_editor()), ops);
    if (!r) {
      return pass(r);
    }
    s.heads.push_back(r->revision);
    s.seqs.push_back(r->seq);
  }
  return s;
}

result<std::unique_ptr<workspace>> open_sample(const sample_store &s, const sample_config &cfg) {
  result<workspace_options> o = sample_options(s.fs, cfg);
  if (!o) {
    return pass(o);
  }
  return workspace::open(s.root, open_mode::write, recovery_policy{}, std::move(*o));
}

result<planted> plant_truncate(vfs &fs, const stdfs::path &dir, std::uint64_t bytes) {
  const result<std::uint32_t> last = last_segment(fs, dir);
  if (!last) {
    return pass(last);
  }
  planted p;
  p.file = segment_rel(*last);
  result<vfs_file> f = fs.open(dir / p.file, open_options{.write = true});
  if (!f) {
    return pass(f);
  }
  const result<std::uint64_t> size = f->size();
  if (!size) {
    return pass(size);
  }
  if (bytes == 0 || *size <= segment_header_size + bytes) {
    return make_error(errc::invalid_argument, "truncate: the segment is too short");
  }
  p.offset = *size - bytes;
  p.length = bytes;
  if (result<void> ok = f->truncate(p.offset); !ok) {
    return pass(ok);
  }
  return p;
}

result<planted> plant_flip_object(vfs &fs, const stdfs::path &dir) {
  planted p;
  p.file = segment_rel(1);
  const result<std::vector<std::uint8_t>> bytes = read_file(fs, dir / p.file);
  if (!bytes) {
    return pass(bytes);
  }
  std::vector<frame_entry> objects;
  for (const frame_entry &f : frame_table(*bytes)) {
    if (f.type == frame_type::object) {
      objects.push_back(f);
    }
  }
  if (objects.size() < 3) {
    return make_error(errc::invalid_argument, "flip: fewer than 3 object frames");
  }
  const frame_entry &f = objects[objects.size() / 2];
  const byte_span payload =
      byte_span(*bytes).subspan(static_cast<std::size_t>(f.offset + frame_prefix_size),
                                static_cast<std::size_t>(f.end - f.offset - frame_prefix_size));
  const result<object> o = parse_object(payload);
  if (!o) {
    return pass(o);
  }
  p.oid = o->id;
  p.offset = f.offset;
  p.length = f.end - f.offset;
  if (result<void> ok =
          flip_byte(fs, dir / p.file, f.offset + frame_prefix_size + (payload.size() / 2));
      !ok) {
    return pass(ok);
  }
  return p;
}

result<planted> plant_delete_commit(vfs &fs, const stdfs::path &dir) {
  planted p;
  p.file = segment_rel(1);
  result<std::vector<std::uint8_t>> bytes = read_file(fs, dir / p.file);
  if (!bytes) {
    return pass(bytes);
  }
  const std::vector<frame_entry> frames = frame_table(*bytes);
  std::vector<std::size_t> commits;
  for (std::size_t i = 0; i < frames.size(); ++i) {
    if (frames[i].type == frame_type::commit) {
      commits.push_back(i);
    }
  }
  if (commits.size() < 3) {
    return make_error(errc::invalid_argument, "delete: fewer than 3 commits in segment 1");
  }
  const std::size_t k = commits.size() / 2; // a middle commit, never the first or the last
  const std::uint64_t from = frames[commits[k - 1]].end;
  const std::uint64_t to = frames[commits[k]].end;
  const result<commit_record> c = decode_commit(byte_span(*bytes).subspan(
      static_cast<std::size_t>(frames[commits[k]].offset + frame_prefix_size),
      static_cast<std::size_t>(to - frames[commits[k]].offset - frame_prefix_size)));
  if (!c) {
    return pass(c);
  }
  p.seq = c->seq;
  p.offset = from;
  p.length = to - from;
  bytes->erase(bytes->begin() + static_cast<std::ptrdiff_t>(from),
               bytes->begin() + static_cast<std::ptrdiff_t>(to));
  if (result<void> ok = write_bytes(fs, dir / p.file, *bytes); !ok) {
    return pass(ok);
  }
  return p;
}

result<planted> plant_remove_loose(vfs &fs, const stdfs::path &dir) {
  result<planted> p = first_loose(fs, dir);
  if (!p) {
    return p;
  }
  if (result<void> ok = fs.remove(dir / p->file); !ok) {
    return pass(ok);
  }
  return p;
}

result<planted> plant_flip_loose(vfs &fs, const stdfs::path &dir) {
  result<planted> p = first_loose(fs, dir);
  if (!p) {
    return p;
  }
  p->offset = p->length / 2;
  if (result<void> ok = flip_byte(fs, dir / p->file, p->offset); !ok) {
    return pass(ok);
  }
  return p;
}

result<planted> plant_newer_format(vfs &fs, const stdfs::path &dir, std::string_view text) {
  const std::vector<std::uint8_t> bytes(text.begin(), text.end());
  if (result<void> ok = write_bytes(fs, dir / "FORMAT", bytes); !ok) {
    return pass(ok);
  }
  return planted{.file = "FORMAT", .offset = 0, .length = bytes.size(), .oid = {}, .seq = 0};
}

result<planted> plant_stale_lock(vfs &fs, const stdfs::path &dir, std::string_view holder) {
  std::vector<std::uint8_t> bytes(holder.begin(), holder.end());
  bytes.push_back('\n');
  if (result<void> ok = write_bytes(fs, dir / "lock", bytes); !ok) {
    return pass(ok);
  }
  return planted{.file = "lock", .offset = 0, .length = bytes.size(), .oid = {}, .seq = 0};
}

result<planted> plant_broken_checkpoint(vfs &fs, const stdfs::path &dir) {
  const result<std::vector<std::string>> names = list_checkpoints(fs, dir);
  if (!names) {
    return pass(names);
  }
  if (names->empty()) {
    return make_error(errc::not_found, "the store holds no checkpoint");
  }
  std::vector<std::string> sorted = *names;
  std::ranges::sort(sorted);
  planted p;
  p.file = std::string(checkpoint_dir_name) + "/" + sorted.back();
  const result<std::vector<std::uint8_t>> bytes = read_file(fs, dir / p.file);
  if (!bytes) {
    return pass(bytes);
  }
  p.offset = bytes->size() / 2;
  p.length = 1;
  if (result<void> ok = flip_byte(fs, dir / p.file, p.offset); !ok) {
    return pass(ok);
  }
  return p;
}

result<void> make_checkpoint(const sample_store &s, const sample_config &cfg) {
  const result<std::uint32_t> last = last_segment(*s.fs, s.dir);
  if (!last) {
    return pass(last);
  }
  result<vfs_file> f = s.fs->open(s.dir / segment_rel(*last), open_options{.write = true});
  if (!f) {
    return pass(f);
  }
  const std::vector<std::uint8_t> torn = {0x40, 0x00, 0x00};
  if (result<std::uint64_t> ok = f->append(torn); !ok) {
    return pass(ok);
  }
  result<std::unique_ptr<workspace>> ws = open_sample(s, cfg);
  if (!ws) {
    return pass(ws);
  }
  return {};
}

result<digest> rewrite_head(store_backend &store, std::vector<tree_change> changes,
                            std::vector<object> extra, std::string_view ref) {
  const result<digest> head = store.read_ref(ref);
  if (!head) {
    return pass(head);
  }
  const result<object> rev_object = store.get(*head);
  if (!rev_object) {
    return pass(rev_object);
  }
  result<gen::fabric_history::revision> rev = decode_revision(*rev_object);
  if (!rev) {
    return pass(rev);
  }
  const result<object> root_obj = store.get(rev->root);
  if (!root_obj) {
    return pass(root_obj);
  }
  result<root_object> root = decode_root_object(*root_obj);
  if (!root) {
    return pass(root);
  }
  object_sink sink(store);
  for (object &o : extra) {
    if (result<void> ok = sink.add(std::move(o)); !ok) {
      return pass(ok);
    }
  }
  const tree_reader reader(store);
  const result<subtree> records = update_tree(reader, root->records, std::move(changes), sink);
  if (!records) {
    return pass(records);
  }
  root->records = records->id;
  result<object> new_root = encode_root_object(*root);
  if (!new_root) {
    return pass(new_root);
  }
  rev->root = new_root->id;
  rev->parents = {*head};
  rev->generation += 1;
  result<object> new_rev = encode_revision(*rev);
  if (!new_rev) {
    return pass(new_rev);
  }
  const digest id = new_rev->id;
  if (result<void> ok = sink.add(std::move(*new_root)); !ok) {
    return pass(ok);
  }
  if (result<void> ok = sink.add(std::move(*new_rev)); !ok) {
    return pass(ok);
  }
  commit_batch batch;
  batch.objects = sink.take();
  batch.refs.push_back(ref_update{.name = std::string(ref), .expected = *head, .desired = id});
  if (result<commit_ack> ok = backdoor::commit(store, std::move(batch)); !ok) {
    return pass(ok);
  }
  return id;
}

result<digest> rewrite_sample(const sample_store &s, std::vector<tree_change> changes,
                              std::vector<object> extra, const sample_config &cfg) {
  log_options log;
  log.sync = durability::none;
  log.loose_above = cfg.loose_above;
  log.rotate_at = cfg.rotate_at;
  result<recovered_log> r = recover_log(*s.fs, s.dir, recovery_policy{}, log);
  if (!r) {
    return pass(r);
  }
  if (!r->store) {
    return make_error(errc::recovery_required, "rewrite_sample: " + r->report.summary());
  }
  return rewrite_head(*r->store, std::move(changes), std::move(extra));
}

result<digest> plant_record(const sample_store &s, const record_id &id, object o,
                            const sample_config &cfg) {
  std::vector<tree_change> changes;
  changes.push_back(tree_change{
      .id = id, .entry = tree_entry{.id = id, .kind = entry_kind::record, .target = o.id}});
  std::vector<object> extra;
  extra.push_back(std::move(o));
  return rewrite_sample(s, std::move(changes), std::move(extra), cfg);
}

result<record_object> record_at(const store_backend &store, const digest &revision,
                                const record_id &id) {
  const result<snapshot> snap = snapshot::open(store, revision);
  if (!snap) {
    return pass(snap);
  }
  const auto e = snap->get(id);
  if (!e) {
    return pass(e);
  }
  const result<object> o = store.get(e->object);
  if (!o) {
    return pass(o);
  }
  return decode_record_object(*o);
}

} // namespace sl::fabric::testing
