// RT-INV-DIAG-04 (SL-ARCH-003 ARCH3-R055, ADR-0024): memory reports read concurrently with 8
// allocating threads are TSan clean and monotone in totals. Eight threads allocate and free
// through a tracking_resource (three tags, some bad-alignment failures) and a synchronized pool,
// and report external GPU bytes; a reader thread takes reports without pause and checks that
// allocs, frees, failures and peaks never decrease; the owner thread runs on_frame_end. At the end
// every count is exact. Operations scale with SL_RUNTIME_STRESS_SCALE (0.1 in sanitizer builds).

#include "sl/runtime/allocators.hpp"
#include "sl/runtime/memdiag.hpp"
#include "sl/runtime/memory.hpp"

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace mem = sl::runtime::mem;

constexpr std::uint32_t threads = 8;
constexpr std::uint64_t base_seed = 0x5EED0850004ULL;

class stream {
public:
  explicit stream(std::uint64_t seed) noexcept : state_(seed) {}
  std::uint64_t next() noexcept {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
  }

private:
  std::uint64_t state_;
};

double stress_scale() {
  const char *text = std::getenv("SL_RUNTIME_STRESS_SCALE");
  if (text == nullptr) {
    return 1.0;
  }
  char *end = nullptr;
  const double v = std::strtod(text, &end);
  return end != text && v > 0.0 ? v : 1.0;
}

std::uint32_t scaled(std::uint32_t full) {
  const double n = static_cast<double>(full) * stress_scale();
  return n < 1000.0 ? 1000U : static_cast<std::uint32_t>(n);
}

struct totals {
  std::uint64_t allocs = 0;
  std::uint64_t frees = 0;
  std::uint64_t failures = 0;
};

struct held {
  void *p = nullptr;
  std::size_t size = 0;
  bool pooled = false;
};

} // namespace

TEST_CASE("RT-INV-DIAG-04: reports read concurrently with 8 allocating threads are TSan clean "
          "and monotone in totals") {
  mem::tag_registry registry;
  const std::array<sl::result<mem::memory_tag>, 3> tags{
      registry.add("ecs.chunks"), registry.add("events"), registry.add("jobs.records")};
  const auto heap = registry.reserve_external("gpu.heap");
  for (const auto &t : tags) {
    REQUIRE(t.has_value());
  }
  REQUIRE(heap.has_value());
  REQUIRE(registry.freeze().has_value());

  mem::tracking_resource tracked("tracked", *tags[0], &registry);
  mem::pool_resource pool(tracked, 64, 16, *tags[2]);
  mem::synchronized_resource shared(pool);
  mem::diagnostics diag(registry, 1);
  diag.add(tracked);
  diag.add("jobs.pool", shared);

  const std::uint32_t ops = scaled(20000);
  std::atomic<std::uint32_t> running{threads};
  std::atomic<bool> monotone{true};
  std::atomic<bool> external_ok{true};
  std::atomic<std::uint64_t> reports{0};
  std::array<totals, threads> expected{};

  std::thread reader([&] {
    totals last;
    std::map<std::string, std::uint64_t> last_peak;
    std::map<std::string, std::uint64_t> last_allocs;
    while (running.load() != 0) {
      const mem::memory_report r = diag.make_report(0);
      totals now;
      for (const mem::tag_stats &t : r.tags) {
        now.allocs += t.allocs;
        now.frees += t.frees;
        now.failures += t.failures;
        if (t.live_bytes > t.peak_bytes || t.frees > t.allocs || t.peak_bytes < last_peak[t.tag] ||
            t.allocs < last_allocs[t.tag]) {
          monotone.store(false);
        }
        last_peak[t.tag] = t.peak_bytes;
        last_allocs[t.tag] = t.allocs;
      }
      if (now.allocs < last.allocs || now.frees < last.frees || now.failures < last.failures) {
        monotone.store(false);
      }
      last = now;
      reports.fetch_add(1);
      std::this_thread::yield();
    }
  });

  std::vector<std::thread> workers;
  workers.reserve(threads);
  for (std::uint32_t k = 0; k < threads; ++k) {
    workers.emplace_back([&, k] {
      stream rng(base_seed + k);
      std::vector<held> mine;
      totals &count = expected.at(k);
      for (std::uint32_t i = 0; i < ops; ++i) {
        const std::uint64_t roll = rng.next();
        if (roll % 97U == 0 && !diag.report_external(*heap, roll % 65536U).has_value()) {
          external_ok.store(false);
        }
        if (roll % 101U == 0) {
          // a bad alignment: a failure under the tag, nothing allocated
          const auto bad = tracked.try_allocate(8, 3, *tags.at(k % 3U));
          if (!bad.has_value()) {
            ++count.failures;
          }
          continue;
        }
        if (mine.empty() || roll % 3U != 0) {
          const bool pooled = roll % 5U == 0;
          const std::size_t size = pooled ? 64 : 1 + ((roll >> 8U) % 2048U);
          auto p = pooled ? shared.try_allocate(64, 16, *tags[2])
                          : tracked.try_allocate(size, 16, *tags.at(k % 2U));
          if (p.has_value()) {
            mine.push_back(held{.p = *p, .size = size, .pooled = pooled});
            if (!pooled) {
              ++count.allocs;
            }
          }
        } else {
          const held h = mine.back();
          mine.pop_back();
          if (h.pooled) {
            shared.release(h.p, 64, 16);
          } else {
            tracked.release(h.p, h.size, 16);
            ++count.frees;
          }
        }
        if (mine.size() > 64) {
          const held h = mine.front();
          mine.erase(mine.begin());
          if (h.pooled) {
            shared.release(h.p, 64, 16);
          } else {
            tracked.release(h.p, h.size, 16);
            ++count.frees;
          }
        }
      }
      for (const held &h : mine) {
        if (h.pooled) {
          shared.release(h.p, 64, 16);
        } else {
          tracked.release(h.p, h.size, 16);
          ++count.frees;
        }
      }
      running.fetch_sub(1);
    });
  }

  std::uint64_t frame = 0;
  while (running.load() != 0) {
    diag.on_frame_end(++frame); // the owner thread samples the synchronized pool meanwhile
    std::this_thread::sleep_for(std::chrono::microseconds(500)); // frames, not a spin on the mutex
  }
  for (std::thread &t : workers) {
    t.join();
  }
  reader.join();
  CHECK(monotone.load());
  CHECK(external_ok.load());
  CHECK(reports.load() > 0);
  CHECK(frame > 0);

  diag.on_frame_end(++frame);
  const mem::memory_report r = diag.make_report(frame);
  totals want;
  for (const totals &t : expected) {
    want.allocs += t.allocs;
    want.frees += t.frees;
    want.failures += t.failures;
  }
  std::uint64_t allocs = 0;
  std::uint64_t frees = 0;
  std::uint64_t failures = 0;
  std::uint64_t live = 0;
  for (const mem::tag_stats &t : r.tags) {
    allocs += t.allocs;
    frees += t.frees;
    failures += t.failures;
    live += t.live_bytes;
  }
  const std::uint64_t slabs = pool.slabs(); // the pool's slabs are tracked blocks too
  CHECK(allocs == want.allocs + slabs);
  CHECK(frees == want.frees);
  CHECK(failures == want.failures);
  CHECK(live == tracked.totals().live_bytes);
  REQUIRE(r.allocators.size() == 1);
  CHECK(r.allocators[0].free_bytes == pool.total_blocks() * pool.stride());
  REQUIRE(r.external.size() == 1);
  CHECK(r.external[0].tag == "gpu.heap");
  const auto back = mem::from_json(mem::to_json(r));
  REQUIRE(back.has_value());
  CHECK(*back == r);
}
