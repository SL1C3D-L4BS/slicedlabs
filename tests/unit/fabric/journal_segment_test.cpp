// FAB-INV-JRN-04 (SL-ARCH-002 ARCH2-R29): segment rotation keeps seq and the prev chain across
// files. A commit never spans segments; each segment header names its number and the first seq
// it holds; a segment out of chain is journal_corrupt; a torn header of a new last segment needs
// recovery.

#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/journal.hpp"
#include "sl/fabric/log_backend.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/vfs.hpp"

#include <doctest/doctest.h>

#include <array>
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
using sl::fabric::digest;
using sl::fabric::frame_type;
using sl::fabric::log_backend;
using sl::fabric::log_options;
using sl::fabric::object;
using sl::fabric::objects_hasher;
using sl::fabric::testing::backdoor;

constexpr std::string_view store_dir = "ws/.slfabric";
constexpr std::string_view log_dir = "ws/.slfabric/log/";
constexpr log_options small{
    .sync = sl::fabric::durability::fsync, .rotate_at = 512, .loose_above = 1U << 20U};

object blob(std::size_t i) {
  const std::string text = "segment test blob " + std::to_string(i) + std::string(60, 'x');
  const std::vector<std::uint8_t> bytes(text.begin(), text.end());
  return sl::fabric::make_object(sl::fabric::object_kind::blob, bytes);
}

std::unique_ptr<log_backend> open_small(sl::fabric::vfs &fs) {
  auto s = log_backend::open(fs, store_dir, small);
  REQUIRE(s.has_value());
  return std::move(*s);
}

// Writes `commits` commits of one blob each, moving ref main.
std::vector<object> fill(sl::fabric::vfs_memory &fs, std::size_t commits) {
  REQUIRE(fs.make_dir("ws").has_value());
  const auto store = open_small(fs);
  std::vector<object> objects;
  std::optional<digest> head;
  for (std::size_t i = 0; i < commits; ++i) {
    const object o = blob(i);
    sl::fabric::commit_batch b{.objects = {o}, .refs = {}, .intents = {}, .commit_time_us = 0};
    b.refs.push_back(sl::fabric::ref_update{.name = "main", .expected = head, .desired = o.id});
    const auto ack = backdoor::commit(*store, std::move(b));
    REQUIRE(ack.has_value());
    CHECK(ack->seq == i + 1);
    head = o.id;
    objects.push_back(o);
  }
  return objects;
}

void rewrite_header(sl::fabric::vfs &fs, std::uint32_t number,
                    const sl::fabric::segment_header &h) {
  auto f = fs.open(std::string(log_dir) + sl::fabric::segment_file_name(number),
                   sl::fabric::open_options{.write = true});
  REQUIRE(f.has_value());
  REQUIRE(f->pwrite(0, sl::fabric::encode_segment_header(h)).has_value());
}

} // namespace

TEST_CASE("FAB-INV-JRN-04: rotation keeps seq and the prev chain across segment files") {
  sl::fabric::vfs_memory fs;
  const std::vector<object> objects = fill(fs, 30);
  const auto names = fs.list("ws/.slfabric/log");
  REQUIRE(names.has_value());
  REQUIRE(names->size() > 3);

  chain_state chain;
  std::vector<std::uint8_t> last_payload;
  for (std::size_t n = 1; n <= names->size(); ++n) {
    CHECK((*names)[n - 1] == sl::fabric::segment_file_name(static_cast<std::uint32_t>(n)));
    const auto bytes = fs.contents(std::string(log_dir) + (*names)[n - 1]);
    REQUIRE(bytes.has_value());
    CHECK(bytes->size() <= small.rotate_at);
    const auto h = sl::fabric::parse_segment_header(*bytes);
    REQUIRE(h.has_value());
    CHECK(h->segment == n);
    CHECK(h->first_seq == chain.seq + 1);
    sl::fabric::frame_reader r(sl::fabric::byte_span(*bytes).subspan(24), 24);
    objects_hasher pending;
    bool ends_with_commit = false;
    while (const auto f = r.next()) {
      ends_with_commit = f->type == frame_type::commit;
      if (f->type == frame_type::object) {
        pending.add(sl::fabric::parse_object(f->payload).value().id);
      } else if (f->type == frame_type::commit) {
        const auto c = sl::fabric::decode_commit(f->payload);
        REQUIRE(c.has_value());
        CHECK(sl::fabric::check_commit(*c, chain, pending).has_value());
        chain = chain_state{.seq = c->seq, .last = sl::fabric::commit_digest(f->payload)};
        last_payload.assign(f->payload.begin(), f->payload.end());
        pending.reset();
      }
    }
    CHECK(r.stop().clean());
    CHECK(ends_with_commit); // a commit never spans segments
    CHECK(pending.count() == 0);
  }
  CHECK(chain.seq == 30);

  const auto store = open_small(fs);
  CHECK(store->last_seq() == 30);
  CHECK(store->segment_count() == names->size());
  CHECK(store->last_commit_digest() == chain.last);
  for (const object &o : objects) {
    CHECK(store->get(o.id).value_or(object{}) == o);
  }
  CHECK(store->read_ref("main").value_or(digest{}) == objects.back().id);
  const object next = blob(99);
  sl::fabric::commit_batch b{.objects = {next}, .refs = {}, .intents = {}, .commit_time_us = 0};
  REQUIRE(backdoor::commit(*store, std::move(b)).value_or(sl::fabric::commit_ack{}).seq == 31);
}

TEST_CASE("FAB-INV-JRN-04: the default rotation is 256 MiB and a large commit gets its own file") {
  CHECK(sl::fabric::segment_rotate_size == 256U * 1024U * 1024U);
  CHECK(log_options{}.rotate_at == sl::fabric::segment_rotate_size);
  sl::fabric::vfs_memory fs;
  fill(fs, 2);
  const auto store = open_small(fs);
  const std::uint32_t before = store->segment_count();
  std::vector<object> many;
  for (std::size_t i = 100; i < 120; ++i) {
    many.push_back(blob(i)); // about 2 KiB of frames: larger than rotate_at
  }
  sl::fabric::commit_batch b{.objects = many, .refs = {}, .intents = {}, .commit_time_us = 0};
  REQUIRE(backdoor::commit(*store, std::move(b)).has_value());
  CHECK(store->segment_count() == before + 1);
  const auto bytes = fs.contents(std::string(log_dir) + sl::fabric::segment_file_name(before + 1));
  REQUIRE(bytes.has_value());
  CHECK(bytes->size() > small.rotate_at);
  CHECK(sl::fabric::parse_segment_header(*bytes).value().first_seq == 3);
}

TEST_CASE("FAB-INV-JRN-04: a segment out of chain is journal_corrupt") {
  sl::fabric::vfs_memory fs;
  fill(fs, 30);
  const auto names = fs.list("ws/.slfabric/log");
  REQUIRE(names.has_value());
  REQUIRE(names->size() > 3);
  const auto h2 =
      sl::fabric::parse_segment_header(fs.contents(std::string(log_dir) + (*names)[1]).value());
  REQUIRE(h2.has_value());

  SUBCASE("a header first seq off by one") {
    sl::fabric::segment_header bad = *h2;
    bad.first_seq += 1;
    rewrite_header(fs, 2, bad);
    const auto r = log_backend::open(fs, store_dir, small);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == errc::journal_corrupt);
    CHECK(r.error().message.find("first seq") != std::string::npos);
  }
  SUBCASE("a header naming another segment") {
    sl::fabric::segment_header bad = *h2;
    bad.segment = 3;
    rewrite_header(fs, 2, bad);
    CHECK(log_backend::open(fs, store_dir, small).error().code == errc::journal_corrupt);
  }
  SUBCASE("a missing middle segment") {
    REQUIRE(fs.remove(std::string(log_dir) + (*names)[1]).has_value());
    const auto r = log_backend::open(fs, store_dir, small);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == errc::journal_corrupt);
    CHECK(r.error().message.find("segment 2 is missing") != std::string::npos);
  }
  SUBCASE("a stranger in log/") {
    REQUIRE(fs.open(std::string(log_dir) + "notes.txt", sl::fabric::open_create).has_value());
    CHECK(log_backend::open(fs, store_dir, small).error().code == errc::journal_corrupt);
  }
  SUBCASE("trailing bytes in a middle segment") {
    auto f = fs.open(std::string(log_dir) + (*names)[1], sl::fabric::open_append);
    REQUIRE(f.has_value());
    const std::array<std::uint8_t, 3> junk{1, 2, 3};
    REQUIRE(f->append(junk).has_value());
    f->close();
    CHECK(log_backend::open(fs, store_dir, small).error().code == errc::journal_corrupt);
  }
}

TEST_CASE("FAB-INV-JRN-04: a torn header of the last segment needs recovery, elsewhere corrupt") {
  sl::fabric::vfs_memory fs;
  fill(fs, 30);
  const auto names = fs.list("ws/.slfabric/log");
  REQUIRE(names.has_value());
  const auto truncate_to = [&fs](const std::string &name, std::uint64_t n) {
    auto f = fs.open(std::string(log_dir) + name, sl::fabric::open_options{.write = true});
    REQUIRE(f.has_value());
    REQUIRE(f->truncate(n).has_value());
  };
  SUBCASE("last") {
    truncate_to(names->back(), 10);
    CHECK(log_backend::open(fs, store_dir, small).error().code == errc::recovery_required);
  }
  SUBCASE("middle") {
    truncate_to((*names)[1], 10);
    CHECK(log_backend::open(fs, store_dir, small).error().code == errc::journal_corrupt);
  }
}
