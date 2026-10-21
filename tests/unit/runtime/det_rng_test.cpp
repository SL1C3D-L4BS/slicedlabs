// RT-INV-DET-01 (SL-ARCH-003 ARCH3-R089, R091, ADR-0026): rng_stream equals the committed vectors
// of tests/golden/runtime/rng_vectors.json for 16 (seed, stream) pairs (8 x next_u64, then 4 x
// next_below(1000), then 2 x next_unit as bit patterns, from one stream in that order; the oracle
// is an independent stdlib Python MT19937-64 recorded in the WO-084 deviation file), and a stream
// restored from its captured state continues identically, directly and through the runtime.rng
// contributor. Malformed state text is snapshot_corrupt and changes nothing.

#include "sl/core/error.hpp"
#include "sl/runtime/replay.hpp"

#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace det = sl::runtime::det;

std::string read_golden(const char *name) {
  std::ifstream in(std::string(SL_RUNTIME_SOURCE_DIR "/tests/golden/runtime/") + name);
  std::ostringstream os;
  os << in.rdbuf();
  return os.str();
}

// The quoted string, or the array of quoted strings, after "key": on one line (no escapes).
std::vector<std::string> field(std::string_view line, std::string_view key) {
  std::vector<std::string> out;
  const std::string needle = "\"" + std::string(key) + "\": ";
  const std::size_t at = line.find(needle);
  if (at == std::string_view::npos) {
    return out;
  }
  std::size_t pos = at + needle.size();
  const bool array = line[pos] == '[';
  for (;;) {
    const std::size_t open = line.find('"', pos);
    const std::size_t stop = array ? line.find(']', pos) : std::string_view::npos;
    if (open == std::string_view::npos || (array && open > stop)) {
      break;
    }
    const std::size_t close = line.find('"', open + 1);
    out.emplace_back(line.substr(open + 1, close - open - 1));
    pos = close + 1;
    if (!array) {
      break;
    }
  }
  return out;
}

std::uint64_t number(std::string_view text) {
  int base = 10;
  if (text.starts_with("0x")) {
    text.remove_prefix(2);
    base = 16;
  }
  std::uint64_t v = 0;
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), v, base);
  REQUIRE(ec == std::errc{});
  REQUIRE(ptr == text.data() + text.size());
  return v;
}

struct golden_case {
  std::uint64_t seed = 0;
  std::uint64_t stream = 0;
  std::vector<std::uint64_t> u64;
  std::vector<std::uint64_t> below;
  std::vector<std::uint64_t> unit_bits;
};

std::vector<golden_case> load() {
  const std::string text = read_golden("rng_vectors.json");
  REQUIRE_FALSE(text.empty());
  std::vector<golden_case> out;
  std::istringstream lines(text);
  for (std::string line; std::getline(lines, line);) {
    if (!line.contains("\"seed\"")) {
      continue;
    }
    golden_case c;
    c.seed = number(field(line, "seed").at(0));
    c.stream = number(field(line, "stream").at(0));
    for (const std::string &v : field(line, "u64")) {
      c.u64.push_back(number(v));
    }
    for (const std::string &v : field(line, "below_1000")) {
      c.below.push_back(number(v));
    }
    for (const std::string &v : field(line, "unit_bits")) {
      c.unit_bits.push_back(number(v));
    }
    out.push_back(c);
  }
  return out;
}

} // namespace

TEST_CASE("RT-INV-DET-01: rng_stream equals rng_vectors.json for 16 (seed, stream) pairs") {
  const std::vector<golden_case> cases = load();
  REQUIRE(cases.size() == 16);
  for (const golden_case &c : cases) {
    CAPTURE(c.seed);
    CAPTURE(c.stream);
    REQUIRE(c.u64.size() == 8);
    REQUIRE(c.below.size() == 4);
    REQUIRE(c.unit_bits.size() == 2);
    det::rng_stream r(c.seed, c.stream);
    for (const std::uint64_t v : c.u64) {
      CHECK(r.next_u64() == v);
    }
    for (const std::uint64_t v : c.below) {
      CHECK(r.next_below(1000) == v);
    }
    for (const std::uint64_t bits : c.unit_bits) {
      const double u = r.next_unit();
      CHECK(std::bit_cast<std::uint64_t>(u) == bits);
      CHECK(u >= 0.0);
      CHECK(u < 1.0);
    }
  }
}

TEST_CASE("RT-INV-DET-01: streams differ by seed and by stream; next_below stays in range") {
  det::rng_stream a(1, 0);
  det::rng_stream b(1, 1);
  det::rng_stream c(2, 0);
  const std::uint64_t x = a.next_u64();
  CHECK(x != b.next_u64());
  CHECK(x != c.next_u64());
  det::rng_stream d(99, 3);
  for (int i = 0; i < 10000; ++i) {
    CHECK(d.next_below(7) < 7);
  }
  CHECK(d.next_below(1) == 0);
  const std::uint64_t top = (std::uint64_t{1} << 63U) + 1; // a heavily biased tail is rejected
  for (int i = 0; i < 1000; ++i) {
    CHECK(d.next_below(top) < top);
  }
}

TEST_CASE("RT-INV-DET-01: a restored stream continues identically") {
  det::rng_stream a(0xDEADBEEFULL, 0xCAFEBABEULL);
  for (int i = 0; i < 1000; ++i) {
    (void)a.next_u64(); // cross a regeneration of the 312-word state
  }
  const std::string state = a.capture();
  det::rng_stream b(0, 0);
  REQUIRE(b.restore(state).has_value());
  CHECK(a == b);
  for (int i = 0; i < 2000; ++i) {
    CHECK(a.next_u64() == b.next_u64());
    CHECK(a.next_below(1000) == b.next_below(1000));
    CHECK(std::bit_cast<std::uint64_t>(a.next_unit()) ==
          std::bit_cast<std::uint64_t>(b.next_unit()));
  }
  CHECK(a.capture() == b.capture());
}

TEST_CASE("RT-INV-DET-01: malformed state text is snapshot_corrupt and changes nothing") {
  det::rng_stream a(5, 5);
  const det::rng_stream before = a;
  auto r = a.restore("not a state");
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == sl::errc::snapshot_corrupt);
  CHECK(a == before);
  const std::string state = a.capture();
  r = a.restore(state.substr(0, state.size() / 2));
  REQUIRE_FALSE(r.has_value());
  CHECK(a == before);
  r = a.restore(state + " 17");
  REQUIRE_FALSE(r.has_value());
  CHECK(a == before);
}

TEST_CASE("RT-INV-DET-01: the runtime.rng contributor round trips and hashes the states") {
  det::rng_stream s0(1, 10);
  det::rng_stream s1(1, 11);
  for (int i = 0; i < 400; ++i) {
    (void)s0.next_u64();
  }
  (void)s1.next_below(10);
  const std::array<det::rng_stream *, 2> live{&s0, &s1};
  const det::rng_contributor ca(live);
  CHECK(ca.name() == "runtime.rng");
  CHECK(ca.version() == 1);
  std::vector<std::byte> bytes;
  REQUIRE(ca.capture(bytes).has_value());

  det::rng_stream t0(0, 0);
  det::rng_stream t1(0, 0);
  const std::array<det::rng_stream *, 2> other{&t0, &t1};
  det::rng_contributor cb(other);
  REQUIRE(cb.restore(bytes).has_value());
  CHECK(t0 == s0);
  CHECK(t1 == s1);
  det::state_hasher ha;
  ca.hash(ha);
  det::state_hasher hb;
  cb.hash(hb);
  CHECK(ha.finish() == hb.finish());
  CHECK(s0.next_u64() == t0.next_u64());
  det::state_hasher hc;
  ca.hash(hc);
  CHECK(hc.finish() != hb.finish()); // the hash follows the state

  // Truncated, trailing or miscounted bytes change nothing.
  const std::string keep = t1.capture();
  std::vector<std::byte> cut(bytes.begin(), bytes.end() - 3);
  auto r = cb.restore(cut);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == sl::errc::snapshot_corrupt);
  std::vector<std::byte> extra = bytes;
  extra.push_back(std::byte{0});
  CHECK_FALSE(cb.restore(extra).has_value());
  std::vector<std::byte> miscount = bytes;
  miscount[0] = std::byte{3};
  CHECK_FALSE(cb.restore(miscount).has_value());
  CHECK(t1.capture() == keep);
}
