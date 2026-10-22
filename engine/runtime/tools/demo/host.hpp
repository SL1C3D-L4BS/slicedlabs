#pragma once

// The lantern-field host (WO-099, SL-ARCH-003 ARCH3-R149, R156, R159, R161, R169; ADR-0027): the
// Fabric side of the gate scene, the in-tool counterpart of testing::fabric_host. It owns a memory
// workspace (workspace::create_memory, no file) whose two content.scene record types are
// registered in code through the public schema.hpp:
//   content.scene.Beacon  record (entity, document, document root) {1 glow f32, 2 lit u32}
//   content.scene.Ward    record (entity, document, document root) {1 radius f32, 2 strength f32}
// The editor (a human actor, subsystem content) seeds beacon_records beacons and ward_records
// wards (256 records, gate_records) in four commits. The runtime actor is a service actor holding
// write:content.scene only. submit commits a proposal with its origin over the patch base under
// commit_policy::rebase_if_commutes and the memory backend's durability (none), then posts the
// head projection to the attached link. The host never reads the F5 ledger (f5.hpp is readable
// only outside engine/runtime). Owner thread.

#include "sl/core/error.hpp"
#include "sl/fabric/authority.hpp"
#include "sl/fabric/boundary.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/workspace.hpp"
#include "sl/runtime/bridge.hpp"

#include <cstdint>
#include <memory>
#include <string_view>

namespace sl::runtime::demo {

inline constexpr std::string_view beacon_type = "content.scene.Beacon";
inline constexpr std::string_view ward_type = "content.scene.Ward";
inline constexpr std::string_view scene_domain = "content.scene";
/// gate_records (`limits`): beacons and wards together.
inline constexpr std::uint32_t beacon_records = 128;
inline constexpr std::uint32_t ward_records = 128;

class lantern_host final : public link::proposal_sink {
  struct passkey {};

public:
  /// A memory workspace with the two types, seeded; the workspace's errors. `seed` feeds the
  /// workspace's deterministic id source only (the seeded state does not depend on it).
  [[nodiscard]] static result<std::unique_ptr<lantern_host>> make(std::uint64_t seed);

  lantern_host(passkey /*key*/, std::unique_ptr<sl::fabric::workspace> ws,
               sl::fabric::document_projection head, std::uint64_t seed_commits) noexcept;
  lantern_host(const lantern_host &) = delete;
  lantern_host(lantern_host &&) = delete;
  lantern_host &operator=(const lantern_host &) = delete;
  lantern_host &operator=(lantern_host &&) = delete;
  ~lantern_host() override = default;

  /// The head of main over content.scene.
  [[nodiscard]] const sl::fabric::document_projection &projection() const noexcept {
    return projection_;
  }
  /// Commits `p` by `origin` on p.base under rebase_if_commutes, then posts the head projection to
  /// the attached link. The manager's errors.
  [[nodiscard]] result<void> submit(const sl::fabric::patch &p,
                                    const sl::fabric::tx_origin &origin) noexcept override;
  /// The link the head projection is posted to after each commit.
  void attach(link::world_link &l) noexcept { link_ = &l; }

  /// Successful submits (the observations committed).
  [[nodiscard]] std::uint64_t commits() const noexcept { return commits_; }
  /// The editor's seeding commits.
  [[nodiscard]] std::uint64_t seed_commits() const noexcept { return seed_commits_; }
  [[nodiscard]] const sl::fabric::schema_registry &schemas() const noexcept {
    return ws_->schemas();
  }
  /// The ARCH3-R159 token: expires with the host.
  [[nodiscard]] std::weak_ptr<const void> alive_token() const noexcept { return alive_; }

  [[nodiscard]] static sl::fabric::actor_id runtime_actor() noexcept;
  [[nodiscard]] static sl::fabric::actor_id editor_actor() noexcept;
  [[nodiscard]] static sl::fabric::record_id beacon_id(std::uint32_t i);
  [[nodiscard]] static sl::fabric::record_id ward_id(std::uint32_t i);

private:
  std::unique_ptr<sl::fabric::workspace> ws_;
  sl::fabric::document_projection projection_;
  std::uint64_t seed_commits_ = 0;
  link::world_link *link_ = nullptr;
  std::uint64_t commits_ = 0;
  std::shared_ptr<const int> alive_ = std::make_shared<const int>(0);
};

} // namespace sl::runtime::demo
