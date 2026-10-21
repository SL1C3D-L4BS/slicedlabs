// RT-INV-DIAG-02 (SL-ARCH-003 ARCH3-R053, schemas/runtime/memreport.schema.json): to_json then
// from_json round trips every counter, equal reports serialize byte-identically, the schema
// example reads back to its own bytes, and each planted mismatch is invalid_argument naming its
// key. The last case writes a live report to SL_RUNTIME_EVIDENCE_ROOT/test/memreport.json for
// `just runtime-memreport-check` and the neg-runtime-leak gate.

#include "sl/core/error.hpp"
#include "sl/runtime/allocators.hpp"
#include "sl/runtime/memdiag.hpp"
#include "sl/runtime/memory.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace mem = sl::runtime::mem;

std::string read_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

mem::memory_report sample_report() {
  mem::memory_report r;
  r.instance = 3;
  r.frame = 1200;
  r.tags = {{.tag = "ecs.chunks",
             .live_bytes = 4194304,
             .live_blocks = 2,
             .peak_bytes = 8388608,
             .allocs = 9,
             .frees = 7,
             .failures = 1},
            {.tag = "events",
             .live_bytes = 0,
             .live_blocks = 0,
             .peak_bytes = 640,
             .allocs = 12,
             .frees = 12,
             .failures = 0},
            {.tag = "frame.arena",
             .live_bytes = std::numeric_limits<std::uint64_t>::max(),
             .live_blocks = 10,
             .peak_bytes = std::numeric_limits<std::uint64_t>::max(),
             .allocs = 10,
             .frees = 0,
             .failures = 3}};
  r.allocators = {
      {.name = "ecs.chunk_pool",
       .free_bytes = 2015232,
       .largest_free_run = 1966080,
       .fragmentation_permille = 25},
      {.name = "jobs.pool", .free_bytes = 0, .largest_free_run = 0, .fragmentation_permille = 0}};
  r.external = {{.tag = "fabric.projection", .bytes = 40960}, {.tag = "gpu.textures", .bytes = 7}};
  r.leaks = {{.tag = "ecs.chunks", .site = "engine/runtime/src/a.cpp:10", .size = 64},
             {.tag = "ecs.chunks", .site = "engine/runtime/src/a.cpp:10", .size = 128},
             {.tag = "events", .site = "odd \"site\" \\ \xc3\xa9 \t:1", .size = 1}};
  r.upstream_allocs_last_frame = 4;
  return r;
}

std::string empty_report_json() {
  mem::memory_report r;
  r.instance = 1;
  return mem::to_json(r);
}

// Replaces the first `from` in `text` by `to`; REQUIREs that `from` occurs.
std::string replace_once(std::string text, std::string_view from, std::string_view to) {
  const auto at = text.find(from);
  REQUIRE(at != std::string::npos);
  text.replace(at, from.size(), to);
  return text;
}

} // namespace

TEST_CASE("RT-INV-DIAG-02: to_json then from_json round trips every counter") {
  const mem::memory_report r = sample_report();
  const std::string text = mem::to_json(r);
  const auto back = mem::from_json(text);
  REQUIRE(back.has_value());
  CHECK(*back == r);
  CHECK(mem::to_json(*back) == text);
  CHECK(text.starts_with("{\n  \"format\": \"sl-runtime-memreport/1\",\n  \"instance\": 3,\n"));
  CHECK(text.ends_with("  \"upstream_allocs_last_frame\": 4\n}\n"));

  // an empty report
  mem::memory_report empty;
  empty.instance = 1;
  const std::string empty_text = mem::to_json(empty);
  CHECK(empty_text.find("\"tags\": [],\n") != std::string::npos);
  CHECK(empty_text.find("\"leaks\": [],\n") != std::string::npos);
  const auto empty_back = mem::from_json(empty_text);
  REQUIRE(empty_back.has_value());
  CHECK(*empty_back == empty);

  // whitespace and key order do not matter to from_json
  const auto compact = mem::from_json(
      R"({"upstream_allocs_last_frame":0,"leaks":[],"external":[],"allocators":[],"tags":[],)"
      R"("frame":5,"instance":2,"format":"sl-runtime-memreport/1"})");
  REQUIRE(compact.has_value());
  CHECK(compact->instance == 2);
  CHECK(compact->frame == 5);
}

TEST_CASE("RT-INV-DIAG-02: equal reports serialize byte-identically") {
  const mem::memory_report a = sample_report();
  mem::memory_report b = sample_report();
  // the same report with every array in another order
  std::swap(b.tags[0], b.tags[2]);
  std::swap(b.allocators[0], b.allocators[1]);
  std::swap(b.external[0], b.external[1]);
  std::swap(b.leaks[0], b.leaks[2]);
  const std::string text = mem::to_json(a);
  CHECK(mem::to_json(b) == text);
  // leaks sorted by (tag, site, size): the 64-byte block before the 128-byte one
  CHECK(text.find("\"size\": 64") < text.find("\"size\": 128"));
}

TEST_CASE("RT-INV-DIAG-02: the schema example reads back to its own bytes") {
  const std::string path =
      std::string(SL_RUNTIME_SOURCE_DIR) + "/schemas/runtime/examples/memreport.json";
  const std::string text = read_file(path);
  REQUIRE_FALSE(text.empty());
  const auto report = mem::from_json(text);
  REQUIRE(report.has_value());
  CHECK(report->instance == 1);
  CHECK(report->frame == 600);
  REQUIRE(report->allocators.size() == 1);
  CHECK(report->allocators[0].fragmentation_permille ==
        mem::fragmentation_permille(report->allocators[0].free_bytes,
                                    report->allocators[0].largest_free_run));
  CHECK(mem::to_json(*report) == text);
}

TEST_CASE("RT-INV-DIAG-02: each mismatch is invalid_argument naming the key") {
  const std::string good = mem::to_json(sample_report());
  REQUIRE(mem::from_json(good).has_value());
  struct planted {
    std::string text;
    std::string_view key;
  };
  const auto at_line_end = [](std::string_view text) { return std::string(text) + "\n"; };
  const std::array<planted, 16> cases{{
      {.text = replace_once(good, at_line_end(R"("frame": 1200,)"), ""), .key = "/frame"},
      {.text = replace_once(good, R"("frame": 1200,)", R"("frame": 1200, "extra": 1,)"),
       .key = "/extra"},
      {.text = replace_once(good, R"("frame": 1200,)", R"("frame": 1200, "frame": 1,)"),
       .key = "/frame"},
      {.text = replace_once(good, R"("instance": 3)", R"("instance": 0)"), .key = "/instance"},
      {.text = replace_once(good, R"("instance": 3)", R"("instance": 4294967296)"),
       .key = "/instance"},
      {.text = replace_once(good, R"("sl-runtime-memreport/1")", R"("sl-runtime-memreport/2")"),
       .key = "/format"},
      {.text = replace_once(good, R"("allocs": 9)", R"("allocs": "9")"), .key = "/tags/0/allocs"},
      {.text = replace_once(good, R"("frees": 12)", R"("frees": -1)"), .key = "/tags/1/frees"},
      {.text = replace_once(good, R"("peak_bytes": 640)", R"("peak_bytes": 640.5)"),
       .key = "/tags/1/peak_bytes"},
      {.text = replace_once(good, R"("fragmentation_permille": 25)",
                            R"("fragmentation_permille": 1001)"),
       .key = "/allocators/0/fragmentation_permille"},
      {.text = replace_once(good, R"("bytes": 40960)", R"("bytes": 18446744073709551616)"),
       .key = "/external/0/bytes"},
      {.text = replace_once(good, at_line_end(R"("size": 1)"), at_line_end(R"("size": 0)")),
       .key = "/leaks/2/size"},
      {.text = replace_once(good, R"("name": "jobs.pool")", R"("name": "")"),
       .key = "/allocators/1/name"},
      {.text = replace_once(good, at_line_end(R"("failures": 1)"),
                            at_line_end(R"("failures": 1, "site": "x")")),
       .key = "/tags/0/site"},
      {.text = replace_once(good, R"("external": [)", R"("external": {"a": [)"),
       .key = "malformed JSON"},
      {.text = replace_once(empty_report_json(), R"("tags": [])", R"("tags": 7)"), .key = "/tags"},
  }};
  for (const planted &p : cases) {
    const auto got = mem::from_json(p.text);
    REQUIRE_FALSE(got.has_value());
    CHECK(got.error().code == sl::errc::invalid_argument);
    INFO(got.error().message);
    CHECK(got.error().message.find(p.key) != std::string::npos);
  }
  const auto truncated = mem::from_json(good.substr(0, good.size() / 2));
  REQUIRE_FALSE(truncated.has_value());
  CHECK(truncated.error().code == sl::errc::invalid_argument);
  const auto trailing = mem::from_json(good + "{}");
  REQUIRE_FALSE(trailing.has_value());
  CHECK(trailing.error().message.find("trailing") != std::string::npos);
  const auto not_object = mem::from_json("[]");
  REQUIRE_FALSE(not_object.has_value());
  CHECK(not_object.error().code == sl::errc::invalid_argument);
}

TEST_CASE("RT-INV-DIAG-02: a live report round trips and is written for runtime-memreport-check") {
  mem::tag_registry registry;
  for (const std::string_view name : mem::runtime_tag_names) {
    REQUIRE(registry.add(name).has_value());
  }
  for (const std::string_view name : mem::runtime_external_tag_names) {
    REQUIRE(registry.reserve_external(name).has_value());
  }
  REQUIRE(registry.freeze().has_value());
  const auto chunks = registry.find("ecs.chunks");
  const auto events = registry.find("events");
  const auto projection = registry.find("fabric.projection");
  REQUIRE(chunks.has_value());
  REQUIRE(events.has_value());
  REQUIRE(projection.has_value());

  mem::tracking_resource root("runtime.root", *chunks, &registry);
  mem::pool_resource pool(root, 256, 16, *chunks);
  mem::diagnostics diag(registry, 1);
  diag.add(root);
  diag.add("ecs.chunk_pool", pool);

  std::vector<void *> blocks;
  for (int i = 0; i < 8; ++i) {
    auto p = pool.try_allocate(256, 16, *chunks);
    REQUIRE(p.has_value());
    blocks.push_back(*p);
  }
  auto e = root.try_allocate(4096, 64, *events);
  REQUIRE(e.has_value());
  REQUIRE(diag.report_external(*projection, 40960).has_value());
  diag.on_frame_end(1);
  for (std::size_t i = 0; i < blocks.size(); i += 2) {
    pool.release(blocks[i], 256, 16);
  }
  root.release(*e, 4096, 64);
  diag.on_frame_end(2);

  mem::memory_report report = diag.make_report(2);
  report.leaks = diag.leaks(); // the pool's slab is still live: one entry until the pool goes
  CHECK(report.leaks.size() == 1);
  CHECK(report.upstream_allocs_last_frame == 0);
  REQUIRE(report.allocators.size() == 1);
  CHECK(report.allocators[0].free_bytes > 0);
  REQUIRE(report.external.size() == 1);
  CHECK(report.external[0].bytes == 40960);
  const std::string with_leak = mem::to_json(report);
  const auto back = mem::from_json(with_leak);
  REQUIRE(back.has_value());
  CHECK(*back == report);

  for (std::size_t i = 1; i < blocks.size(); i += 2) {
    pool.release(blocks[i], 256, 16);
  }
  // the evidence report: every block returned to the pool, whose slab is the one live tracked
  // block; make_report leaves leaks empty (the runtime takes leaks() once its allocators are gone)
  report = diag.make_report(3);
  const std::string text = mem::to_json(report);
  const std::filesystem::path dir = std::filesystem::path(SL_RUNTIME_EVIDENCE_ROOT) / "test";
  std::filesystem::create_directories(dir);
  {
    std::ofstream out(dir / "memreport.json", std::ios::binary | std::ios::trunc);
    out << text;
    REQUIRE(out.good());
  }
  CHECK(read_file((dir / "memreport.json").string()) == text);
  CHECK(text.find("\"leaks\": [],\n") != std::string::npos);
}
