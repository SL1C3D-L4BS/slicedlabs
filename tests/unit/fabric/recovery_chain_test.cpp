// FAB-INV-RCV-03 (SL-ARCH-002 ARCH2-R30, `recovery_steps` R5, corruption C3): a deleted middle
// commit is detected by the prev chain. Logs of five commits are edited with intact CRCs: a middle
// commit's frames removed, only its commit frame removed, two commits swapped, a commit frame
// re-encoded with a forged prev, and a segment header whose first seq skips a commit. Every edit
// is a blocking C3 finding naming the last valid seq and (for the chain) the expected and found
// prev digests; recovery touches nothing and the write open is refused with journal_corrupt.

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/journal.hpp"
#include "sl/fabric/recovery.hpp"
#include "sl/fabric/vfs.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/crash_harness.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;
namespace tg = sl::fabric::testing;
using sl::errc;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

using bytes_t = std::vector<std::uint8_t>;

struct commit_group {
  std::uint64_t begin = 0;        // first frame of the commit
  std::uint64_t commit_frame = 0; // the commit frame
  std::uint64_t end = 0;          // after the commit frame
};

std::vector<commit_group> groups_of(const bytes_t &seg) {
  std::vector<commit_group> out;
  std::uint64_t begin = segment_header_size;
  for (const tg::frame_entry &f : tg::frame_table(seg)) {
    if (f.type == frame_type::commit) {
      out.push_back(commit_group{.begin = begin, .commit_frame = f.offset, .end = f.end});
      begin = f.end;
    }
  }
  return out;
}

bytes_t slice(const bytes_t &b, std::uint64_t from, std::uint64_t to) {
  return {b.begin() + static_cast<std::ptrdiff_t>(from),
          b.begin() + static_cast<std::ptrdiff_t>(to)};
}

digest payload_digest(const bytes_t &seg, const commit_group &g) {
  return commit_digest(
      std::span(seg).subspan(static_cast<std::size_t>(g.commit_frame) + frame_prefix_size,
                             static_cast<std::size_t>(g.end - g.commit_frame) - frame_prefix_size));
}

// Recovers an edited image: exactly one blocking C3 finding, nothing touched, open refused.
recovery_finding expect_c3(const tg::disk_image &edited, const tg::harness_config &cfg) {
  auto fs = must(tg::restore(edited));
  const recovered_log rec = must(recover_log(*fs, tg::harness_store(), cfg.policy, cfg.log));
  REQUIRE_MESSAGE(rec.report.blocking(), rec.report.summary());
  REQUIRE_FALSE(rec.store);
  REQUIRE(rec.report.findings.size() == 1);
  const recovery_finding f = rec.report.findings[0];
  CHECK(f.kind == corruption_class::c3);
  CHECK(f.code == errc::journal_corrupt);
  CHECK(f.blocking);
  CHECK(f.message.starts_with("C3 broken chain or seq gap"));
  CHECK(rec.report.quarantined.empty());
  CHECK(must(tg::image_of(*fs->memory(), tg::harness_root())) == edited);
  const auto ws = workspace::open(tg::harness_root(), open_mode::write, cfg.policy,
                                  must(tg::harness_options(fs, cfg)));
  REQUIRE_FALSE(ws.has_value());
  CHECK(ws.error().code == errc::journal_corrupt);
  CHECK(ws.error().message.find("C3") != std::string::npos);
  return f;
}

tg::harness_config one_segment() {
  tg::harness_config cfg;
  cfg.log.rotate_at = 1U << 20U;
  cfg.log.loose_above = 1U << 20U;
  return cfg;
}

} // namespace

TEST_CASE("FAB-INV-RCV-03: a deleted middle commit is detected by the prev chain") {
  const tg::harness_config cfg = one_segment();
  const tg::built_log log = must(tg::build_log(21, 5, cfg));
  const std::string seg = tg::segment_path(1);
  const bytes_t &b = log.image.files.at(seg);
  const std::vector<commit_group> g = groups_of(b);
  REQUIRE(g.size() == 5);

  // Commit 3 removed whole: commit 4 has seq 4 and the prev of commit 3.
  tg::disk_image edited = log.image;
  bytes_t cut = slice(b, 0, g[1].end);
  const bytes_t tail = slice(b, g[2].end, b.size());
  cut.insert(cut.end(), tail.begin(), tail.end());
  edited.files[seg] = cut;
  const recovery_finding f = expect_c3(edited, cfg);
  CHECK(f.seq == 2);
  CHECK(f.offset == g[3].commit_frame - (g[2].end - g[1].end));
  CHECK(f.expected == payload_digest(b, g[1]));
  CHECK(f.found == payload_digest(b, g[2]));
  CHECK(f.file == "log/" + segment_file_name(1));
}

TEST_CASE("FAB-INV-RCV-03: a removed commit frame, swapped commits and a forged prev break the "
          "chain") {
  const tg::harness_config cfg = one_segment();
  const tg::built_log log = must(tg::build_log(22, 5, cfg));
  const std::string seg = tg::segment_path(1);
  const bytes_t &b = log.image.files.at(seg);
  const std::vector<commit_group> g = groups_of(b);
  REQUIRE(g.size() == 5);

  SUBCASE("only the commit frame of commit 3 removed") {
    tg::disk_image edited = log.image;
    bytes_t cut = slice(b, 0, g[2].commit_frame);
    const bytes_t tail = slice(b, g[2].end, b.size());
    cut.insert(cut.end(), tail.begin(), tail.end());
    edited.files[seg] = cut;
    const recovery_finding f = expect_c3(edited, cfg);
    CHECK(f.seq == 2);
    CHECK(f.expected == payload_digest(b, g[1]));
  }
  SUBCASE("commits 3 and 4 swapped") {
    tg::disk_image edited = log.image;
    bytes_t cut = slice(b, 0, g[1].end);
    for (const bytes_t &part : {slice(b, g[3].begin, g[3].end), slice(b, g[2].begin, g[2].end),
                                slice(b, g[4].begin, b.size())}) {
      cut.insert(cut.end(), part.begin(), part.end());
    }
    edited.files[seg] = cut;
    const recovery_finding f = expect_c3(edited, cfg);
    CHECK(f.seq == 2);
  }
  SUBCASE("commit 4 re-framed with a forged prev and a valid CRC") {
    const std::size_t payload_at = static_cast<std::size_t>(g[3].commit_frame) + frame_prefix_size;
    commit_record c = must(decode_commit(
        std::span(b).subspan(payload_at, static_cast<std::size_t>(g[3].end) - payload_at)));
    digest forged;
    forged.bytes = sha256::hash(std::string_view("forged"));
    c.prev = forged;
    const bytes_t frame = must(encode_frame(frame_type::commit, must(encode_commit(c))));
    tg::disk_image edited = log.image;
    bytes_t cut = slice(b, 0, g[3].commit_frame);
    cut.insert(cut.end(), frame.begin(), frame.end());
    const bytes_t tail = slice(b, g[3].end, b.size());
    cut.insert(cut.end(), tail.begin(), tail.end());
    edited.files[seg] = cut;
    const recovery_finding f = expect_c3(edited, cfg);
    CHECK(f.seq == 3);
    CHECK(f.offset == g[3].commit_frame);
    CHECK(f.expected == payload_digest(b, g[2]));
    CHECK(f.found == forged);
  }
}

TEST_CASE("FAB-INV-RCV-03: a segment header whose first seq skips a commit is a seq gap") {
  tg::harness_config cfg;
  cfg.log.rotate_at = 3000;
  cfg.log.loose_above = 1U << 20U;
  const tg::built_log log = must(tg::build_log(23, 6, cfg));
  REQUIRE(log.image.files.contains(tg::segment_path(2)));
  bytes_t second = log.image.files.at(tg::segment_path(2));
  const segment_header h = must(parse_segment_header(std::span(second).first(segment_header_size)));
  const auto forged = encode_segment_header(segment_header{
      .major = h.major, .minor = h.minor, .segment = h.segment, .first_seq = h.first_seq + 1});
  std::ranges::copy(forged, second.begin());
  tg::disk_image edited = log.image;
  edited.files[tg::segment_path(2)] = second;
  const recovery_finding f = expect_c3(edited, cfg);
  CHECK(f.file == "log/" + segment_file_name(2));
  CHECK(f.offset == 0);
  CHECK(f.seq == h.first_seq - 1);
}
