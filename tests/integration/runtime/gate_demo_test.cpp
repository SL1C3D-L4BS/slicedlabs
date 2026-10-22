// RT-INV-GATE-01 (SL-ARCH-003 ARCH3-R167 and R168, ADR-0028; WO-099): the gate demo at
// gate_test_scale (0.1 of the scene: 5000 lanterns, 600 steps, the fault at 450) ends with
// "runtime-demo: ok" and writes every gate_evidence file the runner owns, each with its format
// tag, under its evidence directory and nowhere else; the command line takes exactly the seven
// flags and a usage error exits 2 with the usage text. The full pass runs without the resume and
// replay variants and hashes every 50 steps (runtime-determinism runs the full matrix).

#include "../../../engine/runtime/testing/stress_harness.hpp"
#include "../../../engine/runtime/tools/demo/scene.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace demo = sl::runtime::demo;
namespace fs = std::filesystem;

[[nodiscard]] std::string slurp(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

[[nodiscard]] fs::path fresh_dir(std::string_view name) {
  const fs::path dir = fs::path(SL_RUNTIME_EVIDENCE_ROOT) / "gate-test" / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

/// The file names directly in `dir`.
[[nodiscard]] std::set<std::string> names_in(const fs::path &dir) {
  std::set<std::string> names;
  std::ranges::transform(fs::directory_iterator(dir), std::inserter(names, names.end()),
                         [](const fs::directory_entry &e) { return e.path().filename().string(); });
  return names;
}

struct cli_run {
  int code = -1;
  std::string out;
  std::string err;
};

[[nodiscard]] cli_run run(const std::vector<std::string> &args) {
  std::vector<std::string_view> views(args.begin(), args.end());
  cli_run r;
  r.code = demo::run_cli(
      views, [&r](std::string_view t) { r.out += t; }, [&r](std::string_view t) { r.err += t; });
  return r;
}

} // namespace

TEST_CASE("RT-INV-GATE-01: the scaled demo prints ok and writes its evidence under its directory "
          "only") {
  const fs::path parent = fresh_dir("demo");
  const fs::path dir = parent / "evidence";
  demo::demo_options o;
  o.entities = static_cast<std::uint32_t>(sl::runtime::testing::scaled(demo::gate_lanterns / 10));
  o.workers = 4;
  o.evidence_dir = dir.string();
  o.variants = false;
  o.hash_every = 50;
  const demo::demo_summary s = demo::run_demo(o);
  INFO(s.message);
  CHECK(s.code == demo::exit_code::ok);
  REQUIRE_FALSE(s.log.empty());
  CHECK(s.log.back() == demo::ok_line);
  REQUIRE(demo::write_evidence(o, s).has_value());

  const std::vector<std::pair<std::string, std::string>> files = {
      {"demo-report.json", R"("format": "sl-runtime-gate/1")"},
      {"memreport.json", "sl-runtime-memreport/1"},
      {"fault-450.json", "sl-runtime-fault/1"},
      {"schedule.json", "sl-runtime-schedule/1"},
      {"hashes.json", "sl-runtime-hashes/1"},
  };
  std::set<std::string> expected = {"demo.log"};
  for (const auto &[name, tag] : files) {
    INFO(name);
    expected.insert(name);
    CHECK(slurp(dir / name).find(tag) != std::string::npos);
  }
  const std::string log = slurp(dir / "demo.log");
  CHECK(log.ends_with(std::string(demo::ok_line) + "\n"));
  CHECK(names_in(dir) == expected);
  CHECK(names_in(parent) == std::set<std::string>{"evidence"});

  const std::string report = slurp(dir / "demo-report.json");
  CHECK(report.find(R"("fault_tick": 450, "restored_tick": 300, "hash_equal": true)") !=
        std::string::npos);
  CHECK(report.find(R"("phase": "end_frame")") != std::string::npos);
}

TEST_CASE("RT-INV-GATE-01: the command line runs the scene and ends with runtime-demo: ok") {
  const fs::path dir = fresh_dir("cli");
  const cli_run r =
      run({"--seed", "7", "--ticks", "30", "--workers", "2", "--mode", "serial", "--entities",
           "200", "--snapshot-every", "10", "--evidence-dir", dir.string()});
  INFO(r.out, r.err);
  CHECK(r.code == 0);
  CHECK(r.out.ends_with(std::string(demo::ok_line) + "\n"));
  CHECK(fs::exists(dir / "demo.log"));
  CHECK(slurp(dir / "hashes.json").find(R"("mode": "serial")") != std::string::npos);
}

TEST_CASE("RT-INV-GATE-01: a usage error exits 2 with the usage text and runs nothing") {
  const std::vector<std::vector<std::string>> bad = {
      {"--bogus"},
      {"--ticks"},
      {"--ticks", "x"},
      {"--ticks", "0"},
      {"--workers", "0"},
      {"--workers", "65"},
      {"--mode", "fast"},
      {"--seed", "-1"},
      {"--entities", "1e3"},
      {"--seed", "1", "--seed", "2"},
      {"--evidence-dir", ""},
      {"positional"},
  };
  for (const auto &args : bad) {
    const cli_run r = run(args);
    INFO(args.front());
    CHECK(r.code == 2);
    CHECK(r.err.find("usage: sl_runtime_demo") != std::string::npos);
    CHECK(r.out.empty());
  }
  auto parsed = demo::parse_args({});
  REQUIRE(parsed.has_value());
  CHECK(parsed->seed == 1);
  CHECK(parsed->ticks == demo::gate_ticks);
  CHECK(parsed->entities == demo::gate_lanterns);
  CHECK(parsed->snapshot_every == demo::gate_snapshot_every);
  CHECK(parsed->evidence_dir == demo::default_evidence_dir);
  CHECK_FALSE(parsed->workers.has_value());
}
