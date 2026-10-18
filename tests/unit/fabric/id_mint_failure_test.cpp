// FAB-INV-ID-05 (SL-ARCH-001 ARCH1-R02): a failing id_source makes mint return entropy_failure,
// never a null or repeated id; the entropy self-test refuses stuck or zero sources.

#include "sl/fabric/id.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>

namespace {

using sl::fabric::id_class;

// Fails every call after the first `good` calls; writes 0xAA into the buffer before failing, so
// a mint that used a partly filled buffer would be visible.
class failing_source final : public sl::fabric::id_source {
public:
  explicit failing_source(int good) : good_(good) {}
  sl::result<void> fill(std::span<std::uint8_t> out) override {
    std::ranges::fill(out, std::uint8_t{0xAA});
    if (calls_++ < good_) {
      out[0] = static_cast<std::uint8_t>(calls_);
      return {};
    }
    return sl::make_error(sl::errc::io_error, "device unplugged");
  }

private:
  int good_;
  int calls_ = 0;
};

// Succeeds with the same bytes every time (a stuck generator).
class constant_source final : public sl::fabric::id_source {
public:
  explicit constant_source(std::uint8_t v) : v_(v) {}
  sl::result<void> fill(std::span<std::uint8_t> out) override {
    std::ranges::fill(out, v_);
    return {};
  }

private:
  std::uint8_t v_;
};

} // namespace

TEST_CASE("FAB-INV-ID-05: a failing id_source makes mint return entropy_failure") {
  failing_source src{1};
  const auto first = sl::fabric::mint(id_class::asset, src);
  REQUIRE(first.has_value());
  for (int i = 0; i < 3; ++i) {
    const auto again = sl::fabric::mint(id_class::asset, src);
    REQUIRE_FALSE(again.has_value()); // never a null id, never the previous id
    CHECK(again.error().code == sl::errc::entropy_failure);
    CHECK(again.error().message.find("device unplugged") != std::string::npos);
  }
}

TEST_CASE("FAB-INV-ID-05: an all-zero payload is entropy_failure, not a null-looking id") {
  constant_source zero{0};
  const auto id = sl::fabric::mint(id_class::entity, zero);
  REQUIRE_FALSE(id.has_value());
  CHECK(id.error().code == sl::errc::entropy_failure);
}

TEST_CASE("FAB-INV-ID-05: the entropy self-test refuses failing, zero and stuck sources") {
  failing_source dead{0};
  CHECK(sl::fabric::entropy_self_test(dead).error().code == sl::errc::entropy_failure);
  failing_source one_shot{1};
  CHECK(sl::fabric::entropy_self_test(one_shot).error().code == sl::errc::entropy_failure);
  constant_source zero{0};
  CHECK(sl::fabric::entropy_self_test(zero).error().code == sl::errc::entropy_failure);
  constant_source stuck{0x5A};
  const auto r = sl::fabric::entropy_self_test(stuck);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == sl::errc::entropy_failure);
  CHECK(r.error().message.find("equal") != std::string::npos);
  failing_source healthy{2};
  CHECK(sl::fabric::entropy_self_test(healthy).has_value());
}

TEST_CASE("FAB-INV-ID-05: an unassigned class is refused before the source is read") {
  failing_source src{0};
  const auto id = sl::fabric::mint(id_class::none, src);
  REQUIRE_FALSE(id.has_value());
  CHECK(id.error().code == sl::errc::invalid_argument);
  CHECK(sl::fabric::mint(static_cast<id_class>(16), src).error().code ==
        sl::errc::invalid_argument);
}
