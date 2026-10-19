// FAB-INV-JRN-02 (SL-ARCH-002 ARCH2-R30): a commit frame is valid iff its CRC is intact, its seq
// is the previous seq + 1, its prev is the SHA-256 of the previous commit payload (absent for seq
// 1) and the object frames since the previous commit hash to objects_digest. The payload is the
// SLB/1 struct of commit_fields 1-10. Checked on hand-built records and on logs written by
// log_backend, including forged commit frames with intact CRCs.

#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/journal.hpp"
#include "sl/fabric/log_backend.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/vfs.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using sl::errc;
using sl::fabric::chain_state;
using sl::fabric::commit_batch;
using sl::fabric::commit_record;
using sl::fabric::digest;
using sl::fabric::frame_type;
using sl::fabric::log_backend;
using sl::fabric::object;
using sl::fabric::objects_hasher;
using sl::fabric::ref_update;
using sl::fabric::testing::backdoor;

constexpr std::string_view store_dir = "ws/.slfabric";
constexpr std::string_view segment1 = "ws/.slfabric/log/seg-00000001.slg";

object blob(std::string_view text) {
  const std::vector<std::uint8_t> bytes(text.begin(), text.end());
  return sl::fabric::make_object(sl::fabric::object_kind::blob, bytes);
}

digest digest_of(std::string_view text) {
  return digest{.algo = sl::fabric::hash_algo::sha256, .bytes = sl::fabric::sha256::hash(text)};
}

bool same(const commit_record &a, const commit_record &b) {
  if (a.refs.size() != b.refs.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.refs.size(); ++i) {
    if (a.refs[i].name != b.refs[i].name || a.refs[i].expected != b.refs[i].expected ||
        a.refs[i].desired != b.refs[i].desired) {
      return false;
    }
  }
  return a.seq == b.seq && a.prev == b.prev && a.tx_record == b.tx_record &&
         a.revision == b.revision && a.object_count == b.object_count &&
         a.objects_digest == b.objects_digest && a.loose == b.loose &&
         a.commit_time_us == b.commit_time_us && a.actor == b.actor;
}

commit_record full_record() {
  commit_record c;
  c.seq = 7;
  c.prev = digest_of("previous");
  c.tx_record = digest_of("tx");
  c.revision = digest_of("revision");
  c.refs.push_back(
      ref_update{.name = "main", .expected = digest_of("a"), .desired = digest_of("b")});
  c.refs.push_back(
      ref_update{.name = "pins/x", .expected = std::nullopt, .desired = digest_of("c")});
  c.object_count = 3;
  c.objects_digest = digest_of("objects");
  c.loose = {digest_of("loose 1"), digest_of("loose 2")};
  c.commit_time_us = -1234567;
  c.actor = sl::fabric::well_known(sl::fabric::id_class::actor, 9);
  return c;
}

// The commits of one segment in log order, with the object ids seen before each.
struct scanned_commit {
  commit_record record;
  std::vector<std::uint8_t> payload;
  std::vector<digest> objects;
};

std::vector<scanned_commit> scan(sl::fabric::vfs &fs, std::string_view path) {
  auto file = fs.open(path, sl::fabric::open_read);
  REQUIRE(file.has_value());
  std::vector<scanned_commit> out;
  std::vector<digest> objects;
  const auto stop = sl::fabric::scan_segment(
      *file, sl::fabric::segment_header_size,
      [&out, &objects](const sl::fabric::frame_view &f) -> sl::result<void> {
        if (f.type == frame_type::object) {
          const auto o = sl::fabric::parse_object(f.payload);
          REQUIRE(o.has_value());
          objects.push_back(o->id);
        } else if (f.type == frame_type::commit) {
          auto c = sl::fabric::decode_commit(f.payload);
          REQUIRE(c.has_value());
          // Built field by field, not as one aggregate (gcc -fanalyzer, WO-064).
          scanned_commit sc;
          sc.payload.assign(f.payload.begin(), f.payload.end());
          sc.record = std::move(*c);
          sc.objects = std::move(objects);
          out.push_back(std::move(sc));
          objects.clear();
        }
        return {};
      });
  REQUIRE(stop.has_value());
  CHECK(stop->clean());
  return out;
}

std::unique_ptr<log_backend> open_store(sl::fabric::vfs &fs) {
  auto s = log_backend::open(fs, store_dir);
  REQUIRE(s.has_value());
  return std::move(*s);
}

void append_frame(sl::fabric::vfs &fs, frame_type t, const std::vector<std::uint8_t> &payload) {
  auto file = fs.open(segment1, sl::fabric::open_append);
  REQUIRE(file.has_value());
  const auto f = sl::fabric::encode_frame(t, payload);
  REQUIRE(f.has_value());
  REQUIRE(file->append(*f).has_value());
}

// A store with three commits: two blobs and main; one blob and a main move; an intent.
void three_commits(sl::fabric::vfs_memory &fs) {
  REQUIRE(fs.make_dir("ws").has_value());
  const auto store = open_store(fs);
  const object a = blob("a");
  const object b = blob("b");
  const object c = blob("c");
  commit_batch first{.objects = {a, b}, .refs = {}, .intents = {}, .commit_time_us = 10};
  first.refs.push_back(ref_update{.name = "main", .expected = std::nullopt, .desired = b.id});
  REQUIRE(backdoor::commit(*store, std::move(first)).has_value());
  commit_batch second{.objects = {c, a}, .refs = {}, .intents = {}, .commit_time_us = 20};
  second.refs.push_back(ref_update{.name = "main", .expected = b.id, .desired = c.id});
  REQUIRE(backdoor::commit(*store, std::move(second)).has_value());
  commit_batch third{.objects = {}, .refs = {}, .intents = {}, .commit_time_us = 30};
  third.intents.push_back(sl::fabric::intent_record{
      .operation = sl::fabric::well_known(sl::fabric::id_class::operation, 1),
      .kind = "import",
      .phase = sl::fabric::intent_phase::begin,
      .outcome = sl::fabric::intent_outcome::none});
  REQUIRE(backdoor::commit(*store, std::move(third)).has_value());
}

} // namespace

TEST_CASE("FAB-INV-JRN-02: the commit payload is the SLB/1 struct of fields 1-10") {
  const commit_record c = full_record();
  const auto bytes = sl::fabric::encode_commit(c);
  REQUIRE(bytes.has_value());
  const auto back = sl::fabric::decode_commit(*bytes);
  REQUIRE(back.has_value());
  CHECK(same(*back, c));
  const auto v = sl::fabric::decode(*bytes);
  REQUIRE(v.has_value());
  REQUIRE(v->kind() == sl::fabric::value_kind::struct_);
  std::vector<std::uint32_t> numbers;
  for (const sl::fabric::field &f : v->fields()) {
    numbers.push_back(f.no);
  }
  CHECK(numbers == std::vector<std::uint32_t>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
  CHECK(v->find_field(1)->as_uint() == 7);
  CHECK(v->find_field(9)->as_i64() == std::optional<std::int64_t>(-1234567));

  commit_record first = c;
  first.seq = 1;
  first.prev.reset();
  const auto first_bytes = sl::fabric::encode_commit(first);
  REQUIRE(first_bytes.has_value());
  CHECK_FALSE(sl::fabric::decode(*first_bytes).value().has_field(2)); // absent for seq 1
  CHECK(same(sl::fabric::decode_commit(*first_bytes).value(), first));
}

TEST_CASE("FAB-INV-JRN-02: a malformed commit payload is journal_corrupt") {
  const auto not_struct = sl::fabric::encode(sl::fabric::value::uint(3));
  REQUIRE(not_struct.has_value());
  CHECK(sl::fabric::decode_commit(*not_struct).error().code == errc::journal_corrupt);
  const std::vector<std::uint8_t> garbage = {0xFF, 0x00, 0x13};
  CHECK(sl::fabric::decode_commit(garbage).error().code == errc::journal_corrupt);
  for (std::uint32_t drop : {1U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U}) {
    const auto full = sl::fabric::decode(sl::fabric::encode_commit(full_record()).value()).value();
    const auto without = full.without(sl::fabric::path{sl::fabric::path_segment::field(drop)});
    REQUIRE(without.has_value());
    const auto r = sl::fabric::decode_commit(sl::fabric::encode(*without).value());
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == errc::journal_corrupt);
    CHECK(r.error().message.find("field " + std::to_string(drop)) != std::string::npos);
  }
  const auto full = sl::fabric::decode(sl::fabric::encode_commit(full_record()).value()).value();
  const auto wrong_kind =
      full.with(sl::fabric::path{sl::fabric::path_segment::field(7)}, sl::fabric::value::uint(1));
  REQUIRE(wrong_kind.has_value());
  CHECK(sl::fabric::decode_commit(sl::fabric::encode(*wrong_kind).value()).error().code ==
        errc::journal_corrupt);
}

TEST_CASE("FAB-INV-JRN-02: objects_digest is SHA-256 over the 33-byte ids in log order") {
  const object a = blob("a");
  const object b = blob("b");
  objects_hasher h;
  h.add(a.id);
  h.add(b.id);
  sl::fabric::sha256 manual;
  for (const object *o : {&a, &b}) {
    const std::vector<std::uint8_t> algo{static_cast<std::uint8_t>(o->id.algo)};
    manual.update(algo);
    manual.update(o->id.bytes);
  }
  CHECK(h.count() == 2);
  CHECK(h.finish().bytes == manual.finish());
  objects_hasher swapped;
  swapped.add(b.id);
  swapped.add(a.id);
  CHECK(swapped.finish() != h.finish());
  objects_hasher none;
  CHECK(none.finish().bytes == sl::fabric::sha256::hash(std::string_view{}));
}

TEST_CASE("FAB-INV-JRN-02: a commit is valid iff seq, prev chain and objects digest hold") {
  objects_hasher objs;
  objs.add(blob("x").id);
  commit_record c1;
  c1.seq = 1;
  c1.object_count = 1;
  c1.objects_digest = objs.finish();
  const chain_state empty{};
  CHECK(sl::fabric::check_commit(c1, empty, objs).has_value());
  const auto p1 = sl::fabric::encode_commit(c1);
  REQUIRE(p1.has_value());
  const chain_state after1{.seq = 1, .last = sl::fabric::commit_digest(*p1)};
  CHECK(after1.last->bytes == sl::fabric::sha256::hash(*p1));

  const objects_hasher nothing;
  commit_record c2;
  c2.seq = 2;
  c2.prev = after1.last;
  c2.objects_digest = nothing.finish();
  CHECK(sl::fabric::check_commit(c2, after1, nothing).has_value());

  const auto refused = [](const commit_record &c, const chain_state &chain, const objects_hasher &h,
                          std::string_view why) {
    const auto r = sl::fabric::check_commit(c, chain, h);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == errc::journal_corrupt);
    CHECK(r.error().message.find(why) != std::string::npos);
  };
  commit_record gap = c2;
  gap.seq = 3;
  refused(gap, after1, nothing, "seq 3, expected 2");
  commit_record repeat = c2;
  repeat.seq = 1;
  refused(repeat, after1, nothing, "expected 2");
  commit_record no_prev = c2;
  no_prev.prev.reset();
  refused(no_prev, after1, nothing, "prev absent");
  commit_record wrong_prev = c2;
  wrong_prev.prev = digest_of("other");
  refused(wrong_prev, after1, nothing, "prev ");
  commit_record first_with_prev = c1;
  first_with_prev.prev = digest_of("nothing came before");
  refused(first_with_prev, empty, objs, "expected absent");
  commit_record count = c1;
  count.object_count = 2;
  refused(count, empty, objs, "object_count 2");
  commit_record digest_off = c1;
  digest_off.objects_digest = digest_of("not the objects");
  refused(digest_off, empty, objs, "objects_digest");
}

TEST_CASE("FAB-INV-JRN-02: log_backend writes a valid chain of commit frames") {
  sl::fabric::vfs_memory fs;
  three_commits(fs);
  const std::vector<scanned_commit> commits = scan(fs, segment1);
  REQUIRE(commits.size() == 3);
  chain_state chain;
  for (const scanned_commit &c : commits) {
    objects_hasher h;
    for (const digest &id : c.objects) {
      h.add(id);
    }
    CHECK(sl::fabric::check_commit(c.record, chain, h).has_value());
    chain = chain_state{.seq = c.record.seq, .last = sl::fabric::commit_digest(c.payload)};
  }
  CHECK(commits[0].record.object_count == 2);
  CHECK(commits[1].record.object_count == 1); // `a` is already stored
  CHECK(commits[2].record.object_count == 0);
  CHECK(commits[0].record.commit_time_us == 10);
  CHECK(commits[1].record.refs.size() == 1);
  CHECK(commits[1].record.refs[0].expected == std::optional<digest>(blob("b").id));
  const auto reopened = open_store(fs);
  CHECK(reopened->last_seq() == 3);
  CHECK(reopened->last_commit_digest() == chain.last);
}

TEST_CASE("FAB-INV-JRN-02: open accepts a forged commit only when the chain holds") {
  sl::fabric::vfs_memory fs;
  three_commits(fs);
  const std::vector<scanned_commit> commits = scan(fs, segment1);
  const digest last = sl::fabric::commit_digest(commits.back().payload);
  const objects_hasher none;
  commit_record next;
  next.seq = 4;
  next.prev = last;
  next.objects_digest = none.finish();

  SUBCASE("a valid fourth commit opens") {
    append_frame(fs, frame_type::commit, sl::fabric::encode_commit(next).value());
    const auto s = log_backend::open(fs, store_dir);
    REQUIRE(s.has_value());
    CHECK((*s)->last_seq() == 4);
  }
  SUBCASE("a seq gap is journal_corrupt") {
    commit_record bad = next;
    bad.seq = 5;
    append_frame(fs, frame_type::commit, sl::fabric::encode_commit(bad).value());
    CHECK(log_backend::open(fs, store_dir).error().code == errc::journal_corrupt);
  }
  SUBCASE("a broken prev chain is journal_corrupt") {
    commit_record bad = next;
    bad.prev = sl::fabric::commit_digest(commits.front().payload);
    append_frame(fs, frame_type::commit, sl::fabric::encode_commit(bad).value());
    const auto r = log_backend::open(fs, store_dir);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == errc::journal_corrupt);
    CHECK(r.error().message.find("prev") != std::string::npos);
  }
  SUBCASE("an objects digest that does not match the object frames is journal_corrupt") {
    const object d = blob("d");
    append_frame(fs, frame_type::object, sl::fabric::frame_object(d.header, d.payload));
    commit_record bad = next;
    bad.object_count = 1; // the count matches, the digest still covers no frames
    append_frame(fs, frame_type::commit, sl::fabric::encode_commit(bad).value());
    CHECK(log_backend::open(fs, store_dir).error().code == errc::journal_corrupt);
  }
  SUBCASE("a commit frame with a broken CRC is not a commit") {
    std::vector<std::uint8_t> f =
        sl::fabric::encode_frame(frame_type::commit, sl::fabric::encode_commit(next).value())
            .value();
    f.back() ^= 0x40U;
    auto file = fs.open(segment1, sl::fabric::open_append);
    REQUIRE(file.has_value());
    REQUIRE(file->append(f).has_value());
    const auto r = log_backend::open(fs, store_dir);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == errc::recovery_required); // a torn tail, for WO-055's recovery
    CHECK(r.error().message.find("crc mismatch") != std::string::npos);
  }
}
