// FAB-INV-TX-10 (SL-ARCH-002 ARCH2-R12, ARCH2-R02): N reader threads and one writer see monotonic
// revisions with consistent roots. Readers take head snapshots without any lock of the commit
// path while the writer commits; every snapshot a reader sees has a generation at least the last
// one it saw, and its content matches its generation (commit g writes the name "g<g>"), so no
// reader ever sees a half-published revision. Threads start on a latch; the workload is bounded
// (the writer's commit count). Run under ThreadSanitizer by `just workflow ci-tsan`.

#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/value.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/tx_fixture.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;
namespace tg = sl::fabric::testing;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

struct reader_report {
  std::uint64_t reads = 0;
  std::uint64_t regressions = 0;
  std::uint64_t mismatches = 0;
  std::uint64_t errors = 0;
  std::uint64_t last_generation = 0;
};

constexpr std::size_t reader_count = 4;
constexpr int commit_count = 40;

} // namespace

TEST_CASE("FAB-INV-TX-10: N reader threads and one writer see monotonic revisions with "
          "consistent roots") {
  auto ws =
      must(workspace::create_memory(must(tg::scene_options(std::make_shared<manual_clock>(1)))));
  transaction_manager &m = ws->manager();
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  const record_id hero = tg::entity_named("hero");
  must(tg::commit_ops(m, editor, {must(tg::create_entity("hero"))}));

  std::latch start(reader_count + 1);
  std::atomic<bool> done{false};
  std::vector<reader_report> reports(reader_count);
  std::vector<std::thread> readers;
  readers.reserve(reader_count);
  for (std::size_t i = 0; i < reader_count; ++i) {
    readers.emplace_back([&, i] {
      reader_report &rep = reports[i];
      start.arrive_and_wait();
      bool last_round = false;
      while (!last_round) {
        last_round = done.load(std::memory_order_acquire);
        auto head = m.head();
        if (!head) {
          ++rep.errors;
          continue;
        }
        ++rep.reads;
        const std::uint64_t g = head->generation();
        if (g < rep.last_generation) {
          ++rep.regressions;
        }
        rep.last_generation = g;
        auto r = head->get(hero);
        if (!r) {
          ++rep.errors;
          continue;
        }
        const auto name = r->body.get({path_segment::field(1)});
        const std::string want = g == 0 ? std::string("hero") : "g" + std::to_string(g);
        if (!name || name->as_string() != want || head->root_digest() == digest{}) {
          ++rep.mismatches;
        }
      }
    });
  }

  start.arrive_and_wait();
  int committed = 0;
  for (int g = 1; g <= commit_count; ++g) {
    auto r = tg::commit_ops(m, editor, {must(tg::set_name(hero, "g" + std::to_string(g)))});
    committed += r.has_value() ? 1 : 0;
  }
  done.store(true, std::memory_order_release);
  for (std::thread &t : readers) {
    t.join();
  }

  CHECK(committed == commit_count);
  CHECK(must(m.head()).generation() == static_cast<std::uint64_t>(commit_count));
  for (std::size_t i = 0; i < reader_count; ++i) {
    CAPTURE(i);
    CHECK(reports[i].reads > 0);
    CHECK(reports[i].regressions == 0);
    CHECK(reports[i].mismatches == 0);
    CHECK(reports[i].errors == 0);
    CHECK(reports[i].last_generation == static_cast<std::uint64_t>(commit_count));
  }
}
