// FAB-INV-JRN-03 (SL-ARCH-002 ARCH2-R31): frame types 1-5 are known, 0 and 6-127 are
// journal_corrupt, 128-255 are notes of a newer minor and are skipped, by the frame reader and by
// log_backend::open.

#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/journal.hpp"
#include "sl/fabric/log_backend.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/vfs.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using sl::errc;
using sl::fabric::frame_fault;
using sl::fabric::frame_reader;
using sl::fabric::frame_type;
using sl::fabric::frame_type_class;
using sl::fabric::frame_view;
using sl::fabric::log_backend;
using sl::fabric::testing::backdoor;

constexpr std::string_view store_dir = "ws/.slfabric";
constexpr std::string_view segment1 = "ws/.slfabric/log/seg-00000001.slg";

std::vector<std::uint8_t> frame(std::uint8_t type, std::string_view payload) {
  const std::vector<std::uint8_t> p(payload.begin(), payload.end());
  const auto f = sl::fabric::encode_frame(type, p);
  REQUIRE(f.has_value());
  return *f;
}

void append(std::vector<std::uint8_t> &out, const std::vector<std::uint8_t> &more) {
  out.insert(out.end(), more.begin(), more.end());
}

void append_to_segment(sl::fabric::vfs &fs, const std::vector<std::uint8_t> &bytes) {
  auto file = fs.open(segment1, sl::fabric::open_append);
  REQUIRE(file.has_value());
  REQUIRE(file->append(bytes).has_value());
}

void commit_blob(log_backend &store, std::string_view text) {
  const std::vector<std::uint8_t> bytes(text.begin(), text.end());
  sl::fabric::commit_batch b;
  b.objects.push_back(sl::fabric::make_object(sl::fabric::object_kind::blob, bytes));
  REQUIRE(backdoor::commit(store, std::move(b)).has_value());
}

} // namespace

TEST_CASE("FAB-INV-JRN-03: types 1-5 are known, 0 and 6-127 corrupt, 128-255 skipped") {
  for (unsigned t = 0; t < 256; ++t) {
    const frame_type_class cls = sl::fabric::classify_frame_type(static_cast<std::uint8_t>(t));
    if (t >= 1 && t <= 5) {
      CHECK(cls == frame_type_class::known);
    } else if (t >= 128) {
      CHECK(cls == frame_type_class::skipped);
    } else {
      CHECK(cls == frame_type_class::corrupt);
    }
  }
  CHECK(static_cast<int>(frame_type::object) == 1);
  CHECK(static_cast<int>(frame_type::commit) == 2);
  CHECK(static_cast<int>(frame_type::intent_begin) == 3);
  CHECK(static_cast<int>(frame_type::intent_end) == 4);
  CHECK(static_cast<int>(frame_type::note) == 5);
}

TEST_CASE("FAB-INV-JRN-03: the reader stops at a type 0 or 6-127 frame and skips 128-255") {
  const std::vector<std::uint8_t> head = frame(1, "object");
  for (unsigned t = 0; t < 256; ++t) {
    if (t >= 1 && t <= 5) {
      continue;
    }
    std::vector<std::uint8_t> log = head;
    append(log, frame(static_cast<std::uint8_t>(t), "unknown"));
    append(log, frame(2, "commit"));
    frame_reader r(log);
    const auto first = r.next();
    REQUIRE(first.has_value());
    CHECK(first.value_or(frame_view{}).type == frame_type::object);
    const auto second = r.next();
    if (t >= 128) {
      REQUIRE(second.has_value());
      CHECK(second.value_or(frame_view{}).type == frame_type::commit);
      CHECK_FALSE(r.next().has_value());
      CHECK(r.stop().clean());
      CHECK(r.stop().skipped == 1);
    } else {
      CHECK_FALSE(second.has_value());
      CHECK(r.stop().fault == frame_fault::unknown_type);
      CHECK(r.stop().offset == head.size());
      CHECK(r.stop().end == head.size());
      CHECK(r.stop().message.find("type " + std::to_string(t)) != std::string::npos);
    }
  }
}

TEST_CASE("FAB-INV-JRN-03: a note frame (type 5) is returned and changes nothing") {
  std::vector<std::uint8_t> log = frame(5, "diagnostic");
  append(log, frame(200, "newer note"));
  frame_reader r(log);
  const auto note = r.next();
  REQUIRE(note.has_value());
  CHECK(note.value_or(frame_view{}).type == frame_type::note);
  CHECK_FALSE(r.next().has_value());
  CHECK(r.stop().clean());
}

TEST_CASE("FAB-INV-JRN-03: log_backend opens over skipped frames and refuses unknown types") {
  sl::fabric::vfs_memory fs;
  REQUIRE(fs.make_dir("ws").has_value());
  {
    auto store = log_backend::open(fs, store_dir);
    REQUIRE(store.has_value());
    commit_blob(**store, "one");
  }
  SUBCASE("128-255 and notes are skipped, before and after more commits") {
    for (const unsigned t : {128U, 200U, 255U}) {
      append_to_segment(fs, frame(static_cast<std::uint8_t>(t), "from a newer minor"));
    }
    append_to_segment(fs, frame(5, "note"));
    {
      auto store = log_backend::open(fs, store_dir);
      REQUIRE(store.has_value());
      CHECK((*store)->last_seq() == 1);
      commit_blob(**store, "two");
    }
    auto store = log_backend::open(fs, store_dir);
    REQUIRE(store.has_value());
    CHECK((*store)->last_seq() == 2);
    CHECK((*store)->object_count() == 2);
  }
  SUBCASE("6-127 are journal_corrupt even at the tail") {
    for (const unsigned t : {0U, 6U, 64U, 127U}) {
      sl::fabric::vfs_memory copy;
      REQUIRE(copy.make_dir("ws").has_value());
      {
        auto store = log_backend::open(copy, store_dir);
        REQUIRE(store.has_value());
        commit_blob(**store, "one");
      }
      append_to_segment(copy, frame(static_cast<std::uint8_t>(t), "unknown"));
      const auto r = log_backend::open(copy, store_dir);
      REQUIRE_FALSE(r.has_value());
      CHECK(r.error().code == errc::journal_corrupt);
      CHECK(r.error().message.find("unknown frame type") != std::string::npos);
    }
  }
}
