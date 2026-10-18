// FAB-INV-REV-05 (SL-ARCH-001 ARCH1-R51): changing one record in a 100k-record trie writes at most
// depth + 4 objects. A genesis revision of 100 000 records in 100 documents is committed; then
// twenty records, one per revision, get a new body, and each batch must hold at most the records
// trie path length plus 4 objects (record, path nodes, root, tx, revision) and no ref. The
// record count scales with SL_FABRIC_SWEEP_SCALE (0.1 under the sanitizer presets).

#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/tree.hpp"
#include "sl/fabric/value.hpp"

#include "../../../engine/fabric/testing/tree_fixture.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace sl::fabric;

template <class T> T must(sl::result<T> r) {
  REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : r.error().message));
  return std::move(*r);
}

double sweep_scale() {
  const char *env = std::getenv("SL_FABRIC_SWEEP_SCALE"); // NOLINT(concurrency-mt-unsafe)
  if (env == nullptr) {
    return 1.0;
  }
  const std::string_view s(env);
  double v = 1.0;
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  return ec == std::errc{} && v > 0.0 ? std::min(v, 1.0) : 1.0;
}

value body(std::uint64_t n) { return must(value::struct_({field{.no = 1, .v = value::uint(n)}})); }

} // namespace

TEST_CASE("FAB-INV-REV-05: changing one record in a 100k-record trie writes at most depth + 4") {
  const auto records = static_cast<std::size_t>(100'000.0 * sweep_scale());
  schema_registry reg;
  REQUIRE(testing::add_tree_record_types(reg).has_value());
  memory_backend store;
  const std::vector<record_id> ids = testing::tree_ids(records, id_class::entity, "cost");

  revision_builder genesis(must(snapshot::empty(store)), reg);
  for (std::size_t i = 0; i < ids.size(); ++i) {
    REQUIRE(genesis
                .put(ids[i],
                     record_write{.type = type_id_of("gen.rec0"),
                                  .document = well_known(
                                      id_class::entity, static_cast<std::uint32_t>(i % 100) + 1000),
                                  .body = body(i),
                                  .version = std::nullopt})
                .has_value());
  }
  const built_revision g = must(genesis.build(testing::tree_inputs("genesis", 1)));
  REQUIRE(testing::commit_built(store, g).has_value());
  snapshot s = must(snapshot::open(store, g.revision));
  CHECK(must(s.entry_count()) == records);
  const tree_reader reader(store);
  const std::size_t depth = must(testing::trie_depth(reader, s.records_root()));
  MESSAGE("records " << records << ", trie depth " << depth << ", genesis objects "
                     << g.batch.objects.size());

  for (std::uint32_t k = 0; k < 20; ++k) {
    const record_id &id = ids[(static_cast<std::size_t>(k) * 4999) % ids.size()];
    const std::size_t path = must(reader.path_length(s.records_root(), id));
    CHECK(path <= depth);
    const snapshot_entry before = must(s.get(id));
    revision_builder b(s, reg);
    REQUIRE(b.put(id, record_write{.type = before.type,
                                   .document = before.document,
                                   .body = body(1'000'000 + k),
                                   .version = std::nullopt})
                .has_value());
    const built_revision built =
        must(b.build(testing::tree_inputs(std::format("edit {}", k), k + 2)));
    CHECK(built.batch.refs.empty());
    CHECK_MESSAGE(built.batch.objects.size() <= path + 4,
                  "objects " << built.batch.objects.size() << " path " << path);
    CHECK(built.batch.objects.size() <= depth + 4);
    REQUIRE(testing::commit_built(store, built).has_value());
    s = must(snapshot::open(store, built.revision));
    CHECK(must(s.get(id)).body == body(1'000'000 + k));
    const std::vector<record_delta> d = must(must(snapshot::open(store, g.revision)).diff(s));
    CHECK(d.size() == k + 1);
  }
}
