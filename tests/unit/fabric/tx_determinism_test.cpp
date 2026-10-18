// FAB-INV-TX-07 (SL-ARCH-002 ARCH2-R11): same base, same patch and same injected values give the
// golden revision object id whatever the commit time. A fixed base (two entities), a patch that
// renames one and creates an entity whose id is minted from the injected id source (seed 42), and
// two clocks decades apart: both runs, on memory_backend and on log_backend, give the revision,
// root and tx id of tests/fixtures/fabric/tx/golden_revision.json, while the receipts carry the
// two different commit times. Another seed mints another id and so another revision.
// SL_UPDATE_GOLDEN=1 rewrites the fixture (then `python3 -I tools/testing/fixtures.py lock`).

#include "sl/fabric/codec.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/transaction.hpp"
#include "sl/fabric/workspace.hpp"

#include "../../../engine/fabric/testing/test_schemas.hpp"
#include "../../../engine/fabric/testing/tx_fixture.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace sl::fabric;
namespace tg = sl::fabric::testing;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

constexpr std::uint64_t golden_seed = 42;
constexpr std::int64_t clock_a = 1'000'000;
constexpr std::int64_t clock_b = 1'760'000'000'000'000;

struct outcome {
  digest base;
  record_id minted;
  digest revision;
  digest root;
  tx_id tx;
  std::int64_t commit_time_us = 0;
};

outcome run(const std::shared_ptr<store_backend> &store, std::uint64_t seed, std::int64_t now) {
  auto clock = std::make_shared<manual_clock>(now);
  auto ws = must(workspace::create_with(store, must(tg::scene_options(clock, seed))));
  transaction_manager &m = ws->manager();
  const tx_origin editor = tg::tx_origin_of(tg::tx_editor());
  outcome out;
  out.base = must(tg::commit_ops(m, editor,
                                 {must(tg::create_entity("hero")), must(tg::create_entity("foe"))}))
                 .revision;
  clock->advance_us(1'234); // time moves between commits: it must not matter
  out.minted = must(m.mint(id_class::entity));
  patch_meta meta;
  meta.label = "golden edit";
  const commit_receipt r = must(
      tg::commit_ops(m, editor,
                     {must(tg::set_name(tg::entity_named("hero"), "hero prime")),
                      op_create(out.minted, tg::entity_type(), must(tg::sample_entity("minted")))},
                     meta));
  out.revision = r.revision;
  out.root = r.root;
  out.tx = r.tx;
  out.commit_time_us = r.commit_time_us;
  return out;
}

std::string golden_path() {
  return std::string(SL_FABRIC_FIXTURES_DIR) + "/tx/golden_revision.json";
}

std::string render(const outcome &o) {
  std::ostringstream s;
  s << "{\n"
    << R"(  "format": "sl-fabric-tx-golden/1",)" << "\n"
    << R"(  "seed": )" << golden_seed << ",\n"
    << R"(  "clocks_us": [)" << clock_a << ", " << clock_b << "],\n"
    << R"(  "base": ")" << o.base.to_hex() << "\",\n"
    << R"(  "minted": ")" << o.minted.to_string() << "\",\n"
    << R"(  "revision": ")" << o.revision.to_hex() << "\",\n"
    << R"(  "root": ")" << o.root.to_hex() << "\",\n"
    << R"(  "tx": ")" << o.tx.to_string() << "\"\n"
    << "}\n";
  return s.str();
}

std::string member(const json_value &doc, std::string_view key) {
  const json_value *v = doc.find(key);
  REQUIRE_MESSAGE(v != nullptr, std::string(key));
  return v->text;
}

} // namespace

TEST_CASE("FAB-INV-TX-07: same base, patch and injected values give the golden revision oid "
          "whatever the commit time") {
  const outcome a = run(std::make_shared<memory_backend>(), golden_seed, clock_a);
  const outcome b = run(std::make_shared<memory_backend>(), golden_seed, clock_b);
  tg::tx_log_store log;
  const outcome c = run(must(log.open()), golden_seed, clock_b);
  CHECK(a.commit_time_us != b.commit_time_us); // the time is in the receipt only
  CHECK(b.commit_time_us == c.commit_time_us);
  for (const outcome *o : {&b, &c}) {
    CHECK(o->base == a.base);
    CHECK(o->minted == a.minted);
    CHECK(o->revision == a.revision);
    CHECK(o->root == a.root);
    CHECK(o->tx == a.tx);
  }

  const char *update = std::getenv("SL_UPDATE_GOLDEN");
  if (update != nullptr && std::string_view(update) == "1") {
    std::ofstream(golden_path(), std::ios::binary) << render(a);
  }
  std::ifstream in(golden_path(), std::ios::binary);
  REQUIRE_MESSAGE(in.good(), golden_path());
  std::ostringstream text;
  text << in.rdbuf();
  const json_value doc = must(parse_json(text.str()));
  CHECK(member(doc, "format") == "sl-fabric-tx-golden/1");
  CHECK(member(doc, "base") == a.base.to_hex());
  CHECK(member(doc, "minted") == a.minted.to_string());
  CHECK(member(doc, "revision") == a.revision.to_hex());
  CHECK(member(doc, "root") == a.root.to_hex());
  CHECK(member(doc, "tx") == a.tx.to_string());
  CHECK(text.str() == render(a));
}

TEST_CASE("FAB-INV-TX-07: another seed mints another id and gives another revision") {
  const outcome a = run(std::make_shared<memory_backend>(), golden_seed, clock_a);
  const outcome d = run(std::make_shared<memory_backend>(), golden_seed + 1, clock_a);
  CHECK(d.base == a.base);
  CHECK(d.minted != a.minted);
  CHECK(d.revision != a.revision);
}
