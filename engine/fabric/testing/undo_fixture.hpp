#pragma once

// Helpers for the undo manager tests (WO-052): an intent log that records the store's commit
// sequence next to each intent (so a test sees the compensation commit between its brackets), a
// compensator built from a function, and readers of the stored tx record and patch of a revision.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/gen/fabric.history.gen.hpp"
#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/snapshot.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/undo.hpp"

namespace sl::fabric::testing {

/// One intent record and the store's last commit sequence number when it was written.
struct logged_intent {
  intent_record record;
  std::uint64_t seq = 0;
};

/// An intent_log over a memory_backend's sequence (which must outlive it).
class seq_intent_log final : public intent_log {
public:
  explicit seq_intent_log(const memory_backend &store) noexcept : store_(&store) {}

  [[nodiscard]] result<void> write(const intent_record &r) override;
  [[nodiscard]] std::vector<logged_intent> records() const;

private:
  const memory_backend *store_;
  mutable std::mutex mutex_;
  std::vector<logged_intent> records_;
};

/// A compensator that calls `fn`; counts its calls.
class function_compensator final : public compensator {
public:
  using function = std::function<result<patch>(const compensation &, const snapshot &)>;

  function_compensator(std::string command, function fn)
      : command_(std::move(command)), fn_(std::move(fn)) {}

  [[nodiscard]] std::string_view command() const noexcept override { return command_; }
  [[nodiscard]] result<patch> compensate(const compensation &c,
                                         const snapshot &head) const override;
  [[nodiscard]] std::size_t calls() const noexcept { return calls_; }

private:
  std::string command_;
  function fn_;
  mutable std::size_t calls_ = 0;
};

/// The tx record of revision `revision`.
[[nodiscard]] result<gen::fabric_history::tx_record> stored_tx(const store_backend &store,
                                                               const digest &revision);
/// The patch object of revision `revision` (normalised, with its meta).
[[nodiscard]] result<patch> stored_patch(const store_backend &store, const digest &revision);
/// True when the records tries of `a` and `b` differ only in tombstones: the same live record
/// objects, and an id tombstoned on one side is absent or tombstoned with the same last record on
/// the other. Retirements are history: an undone create leaves a tombstone, and a redo retires an
/// id again under its own tx id (retired_by).
[[nodiscard]] result<bool> same_up_to_tombstones(const snapshot &a, const snapshot &b);

/// `n` times SL_FABRIC_SWEEP_SCALE (at most 1, 0.1 under the sanitizer presets), at least 1.
[[nodiscard]] std::size_t sweep_scaled(std::size_t n);

/// The number of revisions on the first-parent chain ending at `head` (0 for none).
[[nodiscard]] result<std::size_t> history_length(const store_backend &store,
                                                 const std::optional<digest> &head);

} // namespace sl::fabric::testing
