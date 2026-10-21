// RT-INV-SNAP-03 (SL-ARCH-003 ARCH3-R144, ADR-0026): every single-bit flip of a small snapshot is
// snapshot_corrupt. A world of a few entities with two reflected components and one two-byte
// contributor gives a snapshot of a few hundred bytes; each of its bits is flipped in turn and
// snapshot::from_bytes refuses the copy with snapshot_corrupt (the SHA-256 trailer fails first).
// The same holds for a snapshot file read back through a faulty vfs that flips bits under load.
// Under a recomputed valid trailer, a major of 2 is snapshot_version and a truncated or extended
// body is snapshot_corrupt.

#include "sl/core/error.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/runtime/ecs/component.hpp"
#include "sl/runtime/ecs/entity.hpp"
#include "sl/runtime/ecs/world.hpp"
#include "sl/runtime/memory.hpp"
#include "sl/runtime/platform.hpp"
#include "sl/runtime/reflect.hpp"
#include "sl/runtime/replay.hpp"
#include "sl/runtime/snapshot.hpp"

#include "../../../engine/fabric/testing/faulty_vfs.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace rt = sl::runtime;
namespace ecs = sl::runtime::ecs;
namespace mem = sl::runtime::mem;
namespace det = sl::runtime::det;
namespace ft = sl::fabric::testing;

constexpr rt::field_flags kept = rt::field_flags::snapshot | rt::field_flags::hashed;

struct spot {
  float x = 0.0F;
  std::int32_t level = 0;
};

struct mark {
  std::uint16_t code = 0;
};

/// Two bytes of contributor state, so the contributor section is flipped too.
class tiny_contributor final : public det::snapshot_contributor {
public:
  [[nodiscard]] std::string_view name() const noexcept override { return "test.tiny"; }
  [[nodiscard]] std::uint32_t version() const noexcept override { return 1; }
  [[nodiscard]] sl::result<void> capture(std::vector<std::byte> &out) const override {
    out.push_back(std::byte{0x5A});
    out.push_back(std::byte{0xA5});
    return {};
  }
  [[nodiscard]] sl::result<void> restore(std::span<const std::byte> /*in*/) override { return {}; }
  void hash(det::state_hasher &h) const override { h.u8(0x5A); }
};

struct fixture {
  fixture() {
    rt::type_builder<spot> s("snap.spot");
    SL_RT_FIELD(s, spot, x, kept);
    SL_RT_FIELD(s, spot, level, kept);
    rt::type_builder<mark> m("snap.mark");
    SL_RT_FIELD(m, mark, code, kept);
    auto spot_type = types.add(s.build());
    auto mark_type = types.add(m.build());
    REQUIRE(spot_type.has_value());
    REQUIRE(mark_type.has_value());
    REQUIRE(types.freeze().has_value());
    REQUIRE(registry.add<spot>("snap.spot", *spot_type).has_value());
    REQUIRE(registry.add<mark>("snap.mark", *mark_type).has_value());
    REQUIRE(registry.freeze().has_value());
    auto made = ecs::world::make(registry, memory);
    REQUIRE(made.has_value());
    w = std::move(*made);
    for (std::int32_t i = 0; i < 3; ++i) {
      auto e = w->create();
      REQUIRE(e.has_value());
      REQUIRE(w->add(*e, spot{.x = 0.5F * static_cast<float>(i), .level = i}).has_value());
      if (i != 1) {
        REQUIRE(w->add(*e, mark{.code = static_cast<std::uint16_t>(100 + i)}).has_value());
      }
    }
  }

  [[nodiscard]] rt::snapshot take() const {
    const tiny_contributor tiny;
    const std::array<const det::snapshot_contributor *, 1> contributors{&tiny};
    rt::snapshot_meta meta;
    meta.frame = 2;
    meta.tick = 2;
    auto s = rt::take(*w, registry, types, contributors, meta);
    REQUIRE(s.has_value());
    return *s;
  }

  rt::type_registry types;
  ecs::component_registry registry;
  mem::tracking_resource memory{"snap.bitflip"};
  std::unique_ptr<ecs::world> w;
};

std::vector<std::byte> copy_of(std::span<const std::byte> bytes) {
  return {bytes.begin(), bytes.end()};
}

/// Replaces the trailer of `bytes` with the SHA-256 of everything before it.
void reseal(std::vector<std::byte> &bytes) {
  const std::size_t body = bytes.size() - 32;
  const sl::fabric::digest d = rt::detail::sha256_of(std::span(bytes).first(body));
  for (std::size_t i = 0; i < 32; ++i) {
    bytes.at(body + i) = std::byte{d.bytes.at(i)};
  }
}

sl::errc refusal(std::vector<std::byte> bytes) {
  auto s = rt::snapshot::from_bytes(std::move(bytes));
  return s.has_value() ? sl::errc::ok : s.error().code;
}

} // namespace

TEST_CASE("RT-INV-SNAP-03: every single-bit flip of a small snapshot is snapshot_corrupt") {
  fixture f;
  const rt::snapshot s = f.take();
  const std::span<const std::byte> bytes = s.bytes();
  REQUIRE(bytes.size() < 1024);
  REQUIRE(refusal(copy_of(bytes)) == sl::errc::ok);
  std::uint64_t refused = 0;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      std::vector<std::byte> flipped = copy_of(bytes);
      flipped.at(i) ^= std::byte{static_cast<std::uint8_t>(1U << bit)};
      const sl::errc code = refusal(std::move(flipped));
      if (code != sl::errc::snapshot_corrupt) {
        CAPTURE(i);
        CAPTURE(bit);
        FAIL("a flipped bit was not snapshot_corrupt");
      }
      ++refused;
    }
  }
  CHECK(refused == bytes.size() * 8);
}

TEST_CASE("RT-INV-SNAP-03: a snapshot file flipped under load is snapshot_corrupt") {
  fixture f;
  const rt::snapshot s = f.take();
  ft::faulty_vfs fs;
  const rt::file_access files(fs, "/");
  REQUIRE(rt::save(files, "a.slrs", s).has_value());
  auto clean = rt::load(files, "a.slrs");
  REQUIRE(clean.has_value());
  CHECK(std::ranges::equal(clean->bytes(), s.bytes()));
  for (std::uint64_t offset = 0; offset < s.bytes().size(); offset += 37) {
    CAPTURE(offset);
    fs.flip_bit("/a.slrs", offset, static_cast<unsigned>(offset % 8));
    auto loaded = rt::load(files, "a.slrs");
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().code == sl::errc::snapshot_corrupt);
    fs.clear_flips();
  }
}

TEST_CASE("RT-INV-SNAP-03: under a valid trailer, major 2 is snapshot_version and a bad body "
          "snapshot_corrupt") {
  fixture f;
  const rt::snapshot s = f.take();
  std::vector<std::byte> major = copy_of(s.bytes());
  major.at(4) = std::byte{2};
  reseal(major);
  CHECK(refusal(major) == sl::errc::snapshot_version);

  std::vector<std::byte> shorter = copy_of(s.bytes());
  shorter.erase(shorter.end() - 33); // the last byte of the state hash
  reseal(shorter);
  CHECK(refusal(shorter) == sl::errc::snapshot_corrupt);

  std::vector<std::byte> longer = copy_of(s.bytes());
  longer.insert(longer.end() - 32, std::byte{0});
  reseal(longer);
  CHECK(refusal(longer) == sl::errc::snapshot_corrupt);

  std::vector<std::byte> magic = copy_of(s.bytes());
  magic.at(0) = std::byte{'X'};
  reseal(magic);
  CHECK(refusal(magic) == sl::errc::snapshot_corrupt);
  CHECK(refusal({}) == sl::errc::snapshot_corrupt);
}
