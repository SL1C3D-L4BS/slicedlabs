// The allocator contract suite (WO-079): see allocator_contract.hpp.

#include "allocator_contract.hpp"

#include "sl/core/error.hpp"
#include "sl/runtime/memory.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <utility>
#include <vector>

namespace sl::runtime::testing {

namespace {

constexpr std::size_t max_messages = 64;
constexpr std::size_t max_live = 256;
constexpr std::size_t max_seeded_bytes = 512;

struct block {
  void *p = nullptr;
  std::size_t bytes = 0;
  std::size_t align = 0;
  std::uint64_t pattern = 0;
};

// splitmix64: a deterministic stream with no std distribution (ARCH3-R087).
class stream {
public:
  explicit stream(std::uint64_t seed) noexcept : state_(seed) {}
  std::uint64_t next() noexcept {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
  }

private:
  std::uint64_t state_;
};

class checker {
public:
  void fail(std::string message) {
    if (messages_.size() < max_messages) {
      messages_.push_back("allocator contract: " + std::move(message));
    }
  }
  [[nodiscard]] std::vector<std::string> take() { return std::move(messages_); }

private:
  std::vector<std::string> messages_;
};

std::byte pattern_byte(std::uint64_t pattern, std::size_t i) noexcept {
  return static_cast<std::byte>((pattern + (i * 131U)) & 0xFFU);
}

// Writes the block's pattern over its served size (at least one byte).
void write_pattern(const block &b) noexcept {
  auto *bytes = static_cast<std::byte *>(b.p);
  const std::size_t n = mem::effective_size(b.bytes);
  for (std::size_t i = 0; i < n; ++i) {
    bytes[i] = pattern_byte(b.pattern, i);
  }
}

bool pattern_intact(const block &b) noexcept {
  const auto *bytes = static_cast<const std::byte *>(b.p);
  const std::size_t n = mem::effective_size(b.bytes);
  for (std::size_t i = 0; i < n; ++i) {
    if (bytes[i] != pattern_byte(b.pattern, i)) {
      return false;
    }
  }
  return true;
}

std::string describe(const block &b) {
  return std::to_string(b.bytes) + " bytes, align " + std::to_string(b.align);
}

bool aligned(const void *p, std::size_t align) noexcept {
  return std::bit_cast<std::uintptr_t>(p) % mem::effective_alignment(align) == 0;
}

// Allocates one block, checks it is non-null and aligned and writes its pattern; false on error.
bool take_block(mem::resource &r, checker &c, const char *step, block &b) {
  auto got = r.try_allocate(b.bytes, b.align, r.default_tag());
  if (!got.has_value()) {
    c.fail(std::string(step) + ": try_allocate(" + describe(b) +
           ") failed: " + got.error().message);
    return false;
  }
  b.p = *got;
  if (b.p == nullptr) {
    c.fail(std::string(step) + ": try_allocate(" + describe(b) + ") returned nullptr");
    return false;
  }
  if (!aligned(b.p, b.align)) {
    c.fail(std::string(step) + ": " + describe(b) + " is not aligned to " +
           std::to_string(mem::effective_alignment(b.align)));
  }
  write_pattern(b);
  return true;
}

void give_block(mem::resource &r, checker &c, const char *step, const block &b) {
  if (!pattern_intact(b)) {
    c.fail(std::string(step) + ": the pattern of " + describe(b) + " changed before release");
  }
  r.release(b.p, b.bytes, b.align);
}

void alignment_sweep(mem::resource &r, checker &c) {
  constexpr std::array<std::size_t, 4> sizes = {1, 24, 100, 4104};
  std::vector<block> held;
  std::uint64_t pattern = 1;
  for (std::size_t align = 1; align <= mem::max_alignment; align <<= 1U) {
    for (const std::size_t bytes : sizes) {
      block b{.p = nullptr, .bytes = bytes, .align = align, .pattern = pattern++};
      if (take_block(r, c, "alignment sweep", b)) {
        held.push_back(b);
      }
    }
  }
  for (const block &b : held) {
    give_block(r, c, "alignment sweep (blocks held together)", b);
  }
}

void refusals(mem::resource &r, checker &c) {
  constexpr std::array<std::size_t, 7> bad = {0, 3, 6, 24, 48, 8192, std::size_t{1} << 20U};
  for (const std::size_t align : bad) {
    auto got = r.try_allocate(64, align, r.default_tag());
    if (got.has_value()) {
      c.fail("alignment " + std::to_string(align) + " was served, not refused");
      r.release(*got, 64, align);
    } else if (got.error().code != errc::bad_alignment) {
      c.fail("alignment " + std::to_string(align) + " was refused with " +
             std::string(to_string(got.error().code)) + ", not bad_alignment");
    }
  }
}

void zero_bytes(mem::resource &r, checker &c) {
  block a{.p = nullptr, .bytes = 0, .align = 1, .pattern = 7};
  block b{.p = nullptr, .bytes = 0, .align = 64, .pattern = 9};
  const bool got_a = take_block(r, c, "zero bytes", a);
  const bool got_b = take_block(r, c, "zero bytes", b);
  if (got_a && got_b && a.p == b.p) {
    c.fail("zero bytes: two live zero-byte blocks share one address");
  }
  if (got_a) {
    give_block(r, c, "zero bytes", a);
  }
  if (got_b) {
    give_block(r, c, "zero bytes", b);
  }
}

void pmr_path(mem::resource &r, checker &c) {
  std::pmr::memory_resource &base = r;
  void *p = base.allocate(64, 32);
  if (p == nullptr || !aligned(p, 32)) {
    c.fail("pmr allocate(64, 32) returned a null or misaligned block");
  } else {
    base.deallocate(p, 64, 32);
  }
  if (!base.is_equal(r)) {
    c.fail("is_equal is false for the resource itself");
  }
  if (base.is_equal(*std::pmr::new_delete_resource())) {
    c.fail("is_equal is true for another resource");
  }
}

void seeded(mem::resource &r, checker &c, const contract_options &o,
            const mem::tracking_resource *tracking, const mem::tag_counters &before) {
  stream rng(o.seed);
  std::vector<block> live;
  live.reserve(max_live);
  std::uint64_t live_bytes = 0;
  for (std::uint32_t op = 0; op < o.operations; ++op) {
    const std::uint64_t roll = rng.next();
    const bool allocate = live.empty() || (live.size() < max_live && roll % 3U != 0U);
    if (allocate) {
      block b{.p = nullptr,
              .bytes = static_cast<std::size_t>(rng.next() % max_seeded_bytes),
              .align = std::size_t{1} << (rng.next() % 9U),
              .pattern = rng.next()};
      if (take_block(r, c, "seeded allocate", b)) {
        live.push_back(b);
        live_bytes += mem::effective_size(b.bytes);
      }
    } else {
      const auto i = static_cast<std::size_t>(rng.next() % live.size());
      give_block(r, c, "seeded release", live[i]);
      live_bytes -= mem::effective_size(live[i].bytes);
      live[i] = live.back();
      live.pop_back();
    }
  }
  if (tracking != nullptr) {
    const mem::tag_counters now = tracking->counters(r.default_tag());
    if (now.live_bytes != before.live_bytes + live_bytes ||
        now.live_blocks != before.live_blocks + live.size()) {
      c.fail("tag accounting: " + std::to_string(now.live_bytes) + " live bytes in " +
             std::to_string(now.live_blocks) + " blocks, expected " +
             std::to_string(before.live_bytes + live_bytes) + " in " +
             std::to_string(before.live_blocks + live.size()));
    }
  }
  for (const block &b : live) {
    give_block(r, c, "final release", b);
  }
}

} // namespace

std::vector<std::string> run_allocator_contract(mem::resource &r, const contract_options &options) {
  checker c;
  const auto *tracking =
      options.owner_thread_only ? dynamic_cast<const mem::tracking_resource *>(&r) : nullptr;
  const mem::tag_counters before =
      tracking != nullptr ? tracking->counters(r.default_tag()) : mem::tag_counters{};
  alignment_sweep(r, c);
  refusals(r, c);
  zero_bytes(r, c);
  pmr_path(r, c);
  seeded(r, c, options, tracking, before);
  if (tracking != nullptr) {
    const mem::tag_counters after = tracking->counters(r.default_tag());
    if (after.live_bytes != before.live_bytes || after.live_blocks != before.live_blocks) {
      c.fail("tag accounting: " + std::to_string(after.live_bytes - before.live_bytes) +
             " bytes in " + std::to_string(after.live_blocks - before.live_blocks) +
             " blocks still live after every block was released");
    }
    if (after.allocs - before.allocs != after.frees - before.frees) {
      c.fail("tag accounting: " + std::to_string(after.allocs - before.allocs) + " allocs but " +
             std::to_string(after.frees - before.frees) + " frees");
    }
    if (after.peak_bytes < before.peak_bytes) {
      c.fail("tag accounting: the peak went down");
    }
  }
  return c.take();
}

} // namespace sl::runtime::testing
