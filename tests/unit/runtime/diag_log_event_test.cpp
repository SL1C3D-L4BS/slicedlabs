// RT-INV-DIAG-05 (SL-ARCH-003 ARCH3-R053, master plan section 3 F6): on_frame_end emits one info
// event of system runtime.mem per report_interval frames, with the sl-log-event/1 fields (checked
// through a json_sink writing under SL_RUNTIME_EVIDENCE_ROOT/work) and the correlation scope
// runtime/<instance> with tick = frame. A memory_sink on the default logger captures the events.

#include "sl/core/log.hpp"
#include "sl/runtime/memdiag.hpp"
#include "sl/runtime/memory.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace mem = sl::runtime::mem;

// Attaches a sink to the default logger for the scope of the object.
class attached {
public:
  explicit attached(std::shared_ptr<sl::log::sink> sink) : sink_(std::move(sink)) {
    sl::log::default_logger().add_sink(sink_);
  }
  attached(const attached &) = delete;
  attached(attached &&) = delete;
  attached &operator=(const attached &) = delete;
  attached &operator=(attached &&) = delete;
  ~attached() { sl::log::default_logger().remove_sink(sink_.get()); }

private:
  std::shared_ptr<sl::log::sink> sink_;
};

std::vector<sl::log::memory_sink::record> mem_events(const sl::log::memory_sink &sink) {
  std::vector<sl::log::memory_sink::record> out;
  for (auto &r : sink.records()) {
    if (r.system == "runtime.mem" && r.level == sl::log::level::info) {
      out.push_back(std::move(r));
    }
  }
  return out;
}

std::string text_field(const sl::log::memory_sink::record &r, std::string_view key) {
  const auto *v = r.find(key);
  return v != nullptr && std::holds_alternative<std::string>(*v) ? std::get<std::string>(*v)
                                                                 : std::string("<none>");
}

std::int64_t int_field(const sl::log::memory_sink::record &r, std::string_view key) {
  const auto *v = r.find(key);
  return v != nullptr && std::holds_alternative<std::int64_t>(*v) ? std::get<std::int64_t>(*v) : -1;
}

struct fixture {
  fixture() {
    REQUIRE(chunks.has_value());
    REQUIRE(events.has_value());
    REQUIRE(projection.has_value());
    REQUIRE(registry.freeze().has_value());
  }
  mem::tag_registry registry;
  sl::result<mem::memory_tag> chunks = registry.add("ecs.chunks");
  sl::result<mem::memory_tag> events = registry.add("events");
  sl::result<mem::memory_tag> projection = registry.reserve_external("fabric.projection");
};

} // namespace

TEST_CASE("RT-INV-DIAG-05: one runtime.mem event per report_interval frames with scope "
          "runtime/<instance>") {
  fixture f;
  mem::tracking_resource tracked("tracked", *f.chunks, &f.registry);
  mem::diagnostics diag(f.registry, 7, 10);
  diag.add(tracked);
  CHECK(diag.report_interval() == 10);
  CHECK(diag.scope() == "runtime/7");
  auto a = tracked.try_allocate(100, 8, *f.events);
  REQUIRE(a.has_value());
  REQUIRE(diag.report_external(*f.projection, 4096).has_value());

  auto sink = std::make_shared<sl::log::memory_sink>(256);
  {
    const attached guard(sink);
    for (std::uint64_t frame = 1; frame <= 35; ++frame) {
      diag.on_frame_end(frame);
    }
  }
  const auto got = mem_events(*sink);
  REQUIRE(got.size() == 3);
  std::uint64_t expected_frame = 10;
  std::uint64_t previous_id = 0;
  for (const auto &e : got) {
    CHECK(e.message == "memory report");
    CHECK(e.scope == "runtime/7");
    CHECK(e.tick == expected_frame);
    CHECK(e.correlation_id != 0);
    CHECK(e.correlation_id != previous_id);
    previous_id = e.correlation_id;
    CHECK(int_field(e, "instance") == 7);
    CHECK(int_field(e, "frame") == static_cast<std::int64_t>(expected_frame));
    CHECK(int_field(e, "live_bytes") == 100);
    CHECK(int_field(e, "live_blocks") == 1);
    CHECK(int_field(e, "peak_bytes") == 100);
    CHECK(int_field(e, "external_bytes") == 4096);
    CHECK(int_field(e, "upstream_allocs_last_frame") == 0);
    CHECK(text_field(e, "tags") == "ecs.chunks=0/0,events=100/100");
    CHECK(text_field(e, "external") == "fabric.projection=4096");
    expected_frame += 10;
  }
  tracked.release(*a, 100, 8);
}

TEST_CASE("RT-INV-DIAG-05: the default interval is report_interval_frames and frame 0 is silent") {
  fixture f;
  mem::diagnostics diag(f.registry, 1);
  mem::diagnostics zero(f.registry, 2, 0);
  CHECK(diag.report_interval() == mem::report_interval_frames);
  CHECK(zero.report_interval() == mem::report_interval_frames);
  CHECK(mem::report_interval_frames == 600);

  auto sink = std::make_shared<sl::log::memory_sink>(64);
  {
    const attached guard(sink);
    for (std::uint64_t frame = 0; frame <= 1300; ++frame) {
      diag.on_frame_end(frame);
    }
  }
  const auto got = mem_events(*sink);
  REQUIRE(got.size() == 2);
  CHECK(got[0].tick == 600);
  CHECK(got[1].tick == 1200);
  CHECK(got[0].scope == "runtime/1");
}

TEST_CASE("RT-INV-DIAG-05: the event carries the allocations of the frame and the "
          "sl-log-event/1 fields") {
  fixture f;
  mem::tracking_resource tracked("tracked", *f.chunks, &f.registry);
  mem::diagnostics diag(f.registry, 3, 2);
  diag.add(tracked);

  const std::filesystem::path dir = std::filesystem::path(SL_RUNTIME_EVIDENCE_ROOT) / "work";
  std::filesystem::create_directories(dir);
  const std::string path = (dir / "diag_log_event.jsonl").string();
  std::FILE *file = std::fopen(path.c_str(), "w+");
  REQUIRE(file != nullptr);
  auto memory = std::make_shared<sl::log::memory_sink>(64);
  {
    const attached keep(memory);
    const attached json(std::make_shared<sl::log::json_sink>(file));
    diag.on_frame_end(1); // nothing allocated yet
    std::vector<void *> blocks;
    for (int i = 0; i < 5; ++i) {
      auto p = tracked.try_allocate(32, 8, *f.chunks);
      REQUIRE(p.has_value());
      blocks.push_back(*p);
    }
    diag.on_frame_end(2); // five upstream allocations in frame 2: event
    CHECK(diag.make_report(2).upstream_allocs_last_frame == 5);
    diag.on_frame_end(3);
    CHECK(diag.make_report(3).upstream_allocs_last_frame == 0);
    for (void *p : blocks) {
      tracked.release(p, 32, 8);
    }
    diag.on_frame_end(4); // a steady frame: event with 0
  }
  const auto got = mem_events(*memory);
  REQUIRE(got.size() == 2);
  CHECK(int_field(got[0], "upstream_allocs_last_frame") == 5);
  CHECK(int_field(got[0], "live_bytes") == 160);
  CHECK(int_field(got[1], "upstream_allocs_last_frame") == 0);
  CHECK(int_field(got[1], "live_bytes") == 0);
  CHECK(int_field(got[1], "peak_bytes") == 160);

  std::fflush(file);
  REQUIRE(std::fseek(file, 0, SEEK_SET) == 0);
  std::string lines;
  std::array<char, 4096> buffer{};
  while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), file) != nullptr) {
    lines += buffer.data();
  }
  std::fclose(file);
  std::filesystem::remove(path);
  std::size_t count = 0;
  const std::string_view system = R"("system":"runtime.mem")";
  for (std::size_t at = lines.find(system); at != std::string::npos;
       at = lines.find(system, at + 1)) {
    ++count;
  }
  CHECK(count == 2);
  CHECK(lines.find(R"("schema":"sl-log-event/1")") != std::string::npos);
  CHECK(lines.find(R"("level":"info")") != std::string::npos);
  CHECK(lines.find(R"("scope":"runtime/3")") != std::string::npos);
  CHECK(lines.find(R"("tick":4)") != std::string::npos);
}
