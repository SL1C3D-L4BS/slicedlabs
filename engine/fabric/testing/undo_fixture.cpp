// Helpers for the undo manager tests (WO-052).

#include "undo_fixture.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/tree.hpp"

namespace sl::fabric::testing {

result<void> seq_intent_log::write(const intent_record &r) {
  const std::scoped_lock lock(mutex_);
  records_.push_back(logged_intent{.record = r, .seq = store_->last_seq()});
  return {};
}

std::vector<logged_intent> seq_intent_log::records() const {
  const std::scoped_lock lock(mutex_);
  return records_;
}

result<patch> function_compensator::compensate(const compensation &c, const snapshot &head) const {
  ++calls_;
  return fn_(c, head);
}

namespace {

result<gen::fabric_history::revision> stored_revision(const store_backend &store,
                                                      const digest &revision) {
  auto o = store.get(revision);
  if (!o) {
    return std::unexpected(o.error());
  }
  return decode_revision(*o);
}

} // namespace

result<gen::fabric_history::tx_record> stored_tx(const store_backend &store,
                                                 const digest &revision) {
  auto rev = stored_revision(store, revision);
  if (!rev) {
    return std::unexpected(rev.error());
  }
  auto o = store.get(rev->tx);
  if (!o) {
    return std::unexpected(o.error());
  }
  return decode_tx_record(*o);
}

result<patch> stored_patch(const store_backend &store, const digest &revision) {
  auto tx = stored_tx(store, revision);
  if (!tx) {
    return std::unexpected(tx.error());
  }
  auto o = store.get(tx->patch);
  if (!o) {
    return std::unexpected(o.error());
  }
  auto v = decode(o->payload);
  if (!v) {
    return std::unexpected(v.error());
  }
  return from_value(*v);
}

result<bool> same_up_to_tombstones(const snapshot &a, const snapshot &b) {
  auto deltas = a.diff(b);
  if (!deltas) {
    return std::unexpected(deltas.error());
  }
  const auto live = [](const std::optional<tree_entry> &e) {
    return e && e->kind != entry_kind::tombstone;
  };
  for (const tree_delta &d : *deltas) {
    if (live(d.before) || live(d.after)) {
      return false;
    }
    if (!d.before || !d.after) {
      continue;
    }
    auto ta = a.store().get(d.before->target);
    auto tb = b.store().get(d.after->target);
    if (!ta || !tb) {
      return std::unexpected(!ta ? ta.error() : tb.error());
    }
    auto da = decode_tombstone(*ta);
    auto db = decode_tombstone(*tb);
    if (!da || !db) {
      return std::unexpected(!da ? da.error() : db.error());
    }
    if (da->id != db->id || da->last_record != db->last_record) {
      return false;
    }
  }
  return true;
}

std::size_t sweep_scaled(std::size_t n) {
  const char *env = std::getenv("SL_FABRIC_SWEEP_SCALE"); // NOLINT(concurrency-mt-unsafe)
  double scale = 1.0;
  if (env != nullptr) {
    const std::string_view s(env);
    double v = 1.0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec == std::errc{} && v > 0.0) {
      scale = std::min(v, 1.0);
    }
  }
  const auto scaled = static_cast<std::size_t>(static_cast<double>(n) * scale);
  return std::max<std::size_t>(scaled, 1);
}

result<std::size_t> history_length(const store_backend &store, const std::optional<digest> &head) {
  std::size_t n = 0;
  std::optional<digest> cur = head;
  while (cur) {
    auto rev = stored_revision(store, *cur);
    if (!rev) {
      return std::unexpected(rev.error());
    }
    ++n;
    cur = rev->parents.empty() ? std::nullopt : std::optional<digest>(rev->parents.front());
  }
  return n;
}

} // namespace sl::fabric::testing
