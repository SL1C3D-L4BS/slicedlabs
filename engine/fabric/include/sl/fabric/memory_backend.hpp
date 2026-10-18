#pragma once

// The in-memory store backend (WO-042, SL-ARCH-002 ARCH2-R02 and R07, ADR-0014): ordered maps of
// object frames by id and of refs by name, an intent log and a commit sequence, behind one
// reader-writer lock. Objects are kept as their SLFO frames and verified on every read. A commit
// applies its steps one by one and rolls every applied step back when a later one fails, so a
// batch lands whole or not at all; memory_backend_faults is the test seam that fails a chosen
// step (engine/fabric/testing/store_faults.hpp).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/store.hpp"

namespace sl::fabric {

/// Fault injection for memory_backend. Neither hook can reach stored data: one can only fail a
/// commit step, the other only alters a private copy of a frame on its way to verification.
class memory_backend_faults {
public:
  memory_backend_faults() = default;
  memory_backend_faults(const memory_backend_faults &) = delete;
  memory_backend_faults(memory_backend_faults &&) = delete;
  memory_backend_faults &operator=(const memory_backend_faults &) = delete;
  memory_backend_faults &operator=(memory_backend_faults &&) = delete;
  virtual ~memory_backend_faults() = default;

  /// Called under the writer lock before each internal step of a commit ("object 0", "ref main",
  /// "intent 0", "seq"); an error makes the commit roll back the steps applied and return it.
  [[nodiscard]] virtual result<void> before_step(std::string_view step) noexcept = 0;
  /// Called, possibly from several readers at once, with a private copy of a stored frame before
  /// it is parsed and verified (get, verify).
  virtual void on_read(const digest &id, std::span<std::uint8_t> frame) const noexcept = 0;
};

class memory_backend final : public store_backend {
public:
  explicit memory_backend(std::shared_ptr<memory_backend_faults> faults = nullptr);
  memory_backend(const memory_backend &) = delete;
  memory_backend(memory_backend &&) = delete;
  memory_backend &operator=(const memory_backend &) = delete;
  memory_backend &operator=(memory_backend &&) = delete;
  ~memory_backend() override;

  [[nodiscard]] result<object> get(const digest &id) const noexcept override;
  [[nodiscard]] result<bool> has(const digest &id) const noexcept override;
  [[nodiscard]] result<digest> read_ref(std::string_view name) const noexcept override;
  [[nodiscard]] result<std::vector<ref_entry>> list_refs() const noexcept override;
  [[nodiscard]] result<commit_ack> commit(const mutation_capability &cap,
                                          commit_batch batch) noexcept override;
  [[nodiscard]] result<verify_report> verify(const verify_scope &scope) const noexcept override;

  /// The number of stored objects.
  [[nodiscard]] std::size_t object_count() const noexcept;
  /// The sequence number of the last commit (0 before the first).
  [[nodiscard]] std::uint64_t last_seq() const noexcept;
  /// Every intent record committed, in commit order.
  [[nodiscard]] std::vector<intent_record> intents() const;
  /// The operations whose begin has no end yet, sorted.
  [[nodiscard]] std::vector<record_id> open_intents() const;

private:
  struct state;
  std::unique_ptr<state> state_;
};

} // namespace sl::fabric
