// RT-INV-DIAG-03 (SL-ARCH-003 ARCH3-R054, ADR-0026): the leak report is sorted by (tag, site, size)
// and byte-identical across runs and worker counts. The same 48 blocks (three tags, three call
// sites, a size table) are allocated by 1, 2, 4 and 8 threads, each taking every k-th block; the
// leak report written as sl-runtime-memreport/1 must not change. Debug and sanitizer builds list
// every block with its site; Release lists one entry per tag (no site table).

#include "sl/runtime/memdiag.hpp"
#include "sl/runtime/memory.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {

namespace mem = sl::runtime::mem;

constexpr std::size_t block_count = 48;
constexpr std::array<std::size_t, 6> sizes{24, 100, 7, 4096, 100, 513};

// Three allocation sites: the site of a block is the try_allocate line inside the function.
void *site_a(mem::tracking_resource &r, std::size_t n, mem::memory_tag t) {
  auto p = r.try_allocate(n, 16, t);
  return p.has_value() ? *p : nullptr;
}
void *site_b(mem::tracking_resource &r, std::size_t n, mem::memory_tag t) {
  auto p = r.try_allocate(n, 16, t);
  return p.has_value() ? *p : nullptr;
}
void *site_c(mem::tracking_resource &r, std::size_t n, mem::memory_tag t) {
  auto p = r.try_allocate(n, 16, t);
  return p.has_value() ? *p : nullptr;
}

struct planned {
  std::size_t tag_index = 0;
  std::size_t site = 0;
  std::size_t size = 0;
};

planned plan(std::size_t i) {
  // a fixed shuffle so allocation order is unrelated to the sort order
  const std::size_t j = ((i * 29U) + 11U) % block_count;
  return planned{.tag_index = j % 3U, .site = (j / 3U) % 3U, .size = sizes.at(j % sizes.size())};
}

void *allocate_at(mem::tracking_resource &tracked, const planned &p, mem::memory_tag tag) {
  switch (p.site) {
  case 0:
    return site_a(tracked, p.size, tag);
  case 1:
    return site_b(tracked, p.size, tag);
  default:
    return site_c(tracked, p.size, tag);
  }
}

// Allocates every planned block with `workers` threads and returns the leak report as JSON.
std::string leak_json(std::uint32_t workers) {
  mem::tag_registry registry;
  const std::array<sl::result<mem::memory_tag>, 3> tags{
      registry.add("snapshots"), registry.add("ecs.chunks"), registry.add("events")};
  for (const auto &t : tags) {
    REQUIRE(t.has_value());
  }
  REQUIRE(registry.freeze().has_value());
  mem::tracking_resource tracked("tracked", *tags[0], &registry);
  mem::diagnostics diag(registry, 1);
  diag.add(tracked);

  std::array<void *, block_count> blocks{};
  {
    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (std::uint32_t k = 0; k < workers; ++k) {
      threads.emplace_back([&, k] {
        for (std::size_t i = k; i < block_count; i += workers) {
          const planned p = plan(i);
          blocks.at(i) = allocate_at(tracked, p, *tags.at(p.tag_index));
        }
      });
    }
    for (std::thread &t : threads) {
      t.join();
    }
  }

  mem::memory_report report;
  report.instance = 1;
  report.leaks = diag.leaks();
  for (std::size_t i = 1; i < report.leaks.size(); ++i) {
    const mem::leak_entry &a = report.leaks[i - 1];
    const mem::leak_entry &b = report.leaks[i];
    CHECK(std::tie(a.tag, a.site, a.size) <= std::tie(b.tag, b.site, b.size));
  }
  if (mem::debug_memory) {
    CHECK(report.leaks.size() == block_count);
    for (const mem::leak_entry &l : report.leaks) {
      CHECK(l.site.find("diag_leak_order_test.cpp:") != std::string::npos);
    }
  } else {
    CHECK(report.leaks.size() == 3);
    for (const mem::leak_entry &l : report.leaks) {
      CHECK(l.site == "unknown");
    }
  }
  std::string json = mem::to_json(report);

  for (std::size_t i = 0; i < block_count; ++i) {
    REQUIRE(blocks.at(i) != nullptr);
    tracked.release(blocks.at(i), plan(i).size, 16);
  }
  CHECK(diag.leaks().empty());
  return json;
}

} // namespace

TEST_CASE("RT-INV-DIAG-03: the leak report is sorted by (tag, site, size)") {
  const std::string json = leak_json(1);
  const auto report = mem::from_json(json);
  REQUIRE(report.has_value());
  REQUIRE_FALSE(report->leaks.empty());
  CHECK(report->leaks.front().tag == "ecs.chunks");
  CHECK(report->leaks.back().tag == "snapshots");
}

TEST_CASE("RT-INV-DIAG-03: the leak report is byte-identical across runs and worker counts") {
  const std::string reference = leak_json(1);
  CHECK(leak_json(1) == reference);
  for (const std::uint32_t workers : {2U, 4U, 8U}) {
    INFO("workers " << workers);
    CHECK(leak_json(workers) == reference);
  }
  CHECK(leak_json(4) == reference);
}
