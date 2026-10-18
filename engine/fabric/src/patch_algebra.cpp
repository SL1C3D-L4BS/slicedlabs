// Patch algebra: invert, compose, footprints, commutes, rebase and order keys (WO-043,
// SL-ARCH-002 ARCH2-R08, ARCH2-R20, ARCH2-R22, ARCH2-R45, ARCH2-R48; ADR-0018, ADR-0019).

#include <algorithm>
#include <compare>
#include <cstddef>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include "sl/core/assert.hpp"
#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/patch.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

namespace {

patch_op invert_op(const patch_op &op) {
  patch_op inv;
  inv.target = op.target;
  inv.path = op.path;
  inv.elem = op.elem;
  inv.rev = op.rev;
  switch (op.kind) {
  case op_kind::create_record:
    inv.kind = op_kind::delete_record;
    inv.captured = op.new_value;
    inv.type = op.type;
    break;
  case op_kind::delete_record:
    inv.kind = op_kind::create_record;
    inv.new_value = op.captured;
    inv.type = op.type;
    break;
  case op_kind::set:
    if (op.expect.is_absent()) {
      inv.kind = op_kind::unset;
    } else {
      inv.kind = op_kind::set;
      inv.new_value = op.expect.as_optional();
    }
    inv.expect = expectation::of(op.new_value);
    break;
  case op_kind::unset:
    if (op.expect.is_present()) {
      inv.kind = op_kind::set;
      inv.new_value = op.expect.expected();
      inv.expect = expectation::absent();
    } else {
      inv.kind = op_kind::unset;
      inv.expect = op.expect;
    }
    break;
  case op_kind::insert_elem:
    inv.kind = op_kind::remove_elem;
    inv.captured = op.new_value;
    inv.expect_order_key = op.order_key;
    break;
  case op_kind::remove_elem:
    inv.kind = op_kind::insert_elem;
    inv.new_value = op.captured;
    inv.order_key = op.expect_order_key;
    break;
  case op_kind::move_elem:
    inv.kind = op_kind::move_elem;
    inv.order_key = op.expect_order_key;
    inv.expect_order_key = op.order_key;
    break;
  case op_kind::retype_record:
    inv.kind = op_kind::retype_record;
    inv.type = op.expect_type;
    inv.new_value = op.captured;
    inv.captured = op.new_value;
    inv.expect_type = op.type;
    break;
  case op_kind::annotate:
    return op;
  }
  return inv;
}

bool is_prefix(const path &p, const path &of) noexcept {
  return p.size() <= of.size() && std::equal(p.begin(), p.end(), of.begin());
}

reversibility stricter(reversibility a, reversibility b) noexcept {
  return static_cast<std::uint8_t>(a) >= static_cast<std::uint8_t>(b) ? a : b;
}

bool coalescible(const patch_op &op) noexcept {
  return (op.kind == op_kind::set && op.new_value.has_value()) || op.kind == op_kind::unset;
}

/// Appends `op` to `ops`, coalescing it into the latest overlapping op when that op is a set or
/// unset of the same record and path.
result<void> fold(std::vector<patch_op> &ops, std::vector<footprint> &prints, const patch_op &op) {
  footprint fp = footprint_of(op);
  if (coalescible(op)) {
    for (std::size_t i = ops.size(); i-- > 0;) {
      if (!prints[i].conflicts_with(fp)) {
        continue;
      }
      patch_op &earlier = ops[i];
      // Only into a set: an unset may fail where a later set at its path would succeed (a
      // variant payload), so folding a set into an unset could make a failing sequence apply.
      if (earlier.kind == op_kind::set && earlier.new_value.has_value() &&
          earlier.target == op.target && earlier.path == op.path) {
        if (!op.expect.holds(earlier.new_value)) {
          return make_error(errc::conflict,
                            std::format("{} of {} at {} expects what the earlier op does not leave",
                                        to_string(op.kind), op.target.to_string(),
                                        to_string(op.path)));
        }
        earlier.kind = op.kind;
        earlier.new_value = op.new_value;
        earlier.rev = stricter(earlier.rev, op.rev);
        return {};
      }
      break;
    }
  }
  ops.push_back(op);
  prints.push_back(std::move(fp));
  return {};
}

std::string entry_text(const footprint_entry &e) {
  return std::format("{}({} {})", to_string(e.mode), e.record.to_string(), to_string(e.path));
}

} // namespace

std::string_view to_string(access a) noexcept {
  switch (a) {
  case access::read:
    return "read";
  case access::write:
    return "write";
  case access::create:
    return "create";
  case access::remove:
    return "remove";
  }
  return "unknown";
}

patch invert(const patch &normalized) {
  SL_DEBUG_ASSERT(is_normalized(normalized), "invert needs a normalised patch");
  patch out;
  out.version = normalized.version;
  out.base = normalized.base;
  out.meta = normalized.meta;
  out.ops.reserve(normalized.ops.size());
  for (const patch_op &op : std::views::reverse(normalized.ops)) {
    out.ops.push_back(invert_op(op));
  }
  return out;
}

result<patch> compose(const patch &a, const patch &b) {
  if (a.version.major != b.version.major) {
    return make_error(errc::invalid_argument, std::format("cannot compose patch majors {} and {}",
                                                          a.version.major, b.version.major));
  }
  patch out;
  out.version =
      patch_version{.major = a.version.major, .minor = std::max(a.version.minor, b.version.minor)};
  out.base = a.base;
  out.meta = a.meta;
  std::vector<footprint> prints;
  for (const patch *part : {&a, &b}) {
    for (const patch_op &op : part->ops) {
      if (auto ok = fold(out.ops, prints, op); !ok) {
        return std::unexpected(std::move(ok.error()));
      }
    }
  }
  return out;
}

// ---- footprints ---------------------------------------------------------------------------

std::strong_ordering operator<=>(const footprint_entry &a, const footprint_entry &b) noexcept {
  if (const auto c = a.record <=> b.record; std::is_neq(c)) {
    return c;
  }
  if (const auto c = a.path <=> b.path; std::is_neq(c)) {
    return c;
  }
  return a.mode <=> b.mode;
}

bool conflicts(const footprint_entry &a, const footprint_entry &b) noexcept {
  if (a.record != b.record || (a.mode == access::read && b.mode == access::read)) {
    return false;
  }
  return is_prefix(a.path, b.path) || is_prefix(b.path, a.path);
}

void footprint::add(footprint_entry e) {
  const auto it = std::ranges::lower_bound(entries, e);
  if (it != entries.end() && *it == e) {
    return;
  }
  entries.insert(it, std::move(e));
}

void footprint::merge(const footprint &other) {
  for (const footprint_entry &e : other.entries) {
    add(e);
  }
}

namespace {

/// The entries of `fp` naming `record` (contiguous: entries sort by record first).
std::span<const footprint_entry> of_record(const footprint &fp, const record_id &record) {
  const auto range = std::ranges::equal_range(fp.entries, record, {}, &footprint_entry::record);
  return {range.begin(), range.end()};
}

} // namespace

bool footprint::conflicts_with(const footprint &other) const noexcept {
  for (const footprint_entry &e : entries) {
    for (const footprint_entry &o : of_record(other, e.record)) {
      if (conflicts(e, o)) {
        return true;
      }
    }
  }
  return false;
}

std::vector<footprint_overlap> footprint::overlaps(const footprint &other) const {
  std::vector<footprint_overlap> out;
  for (const footprint_entry &e : entries) {
    for (const footprint_entry &o : of_record(other, e.record)) {
      if (conflicts(e, o)) {
        out.push_back(footprint_overlap{.first = e, .second = o});
      }
    }
  }
  return out;
}

footprint footprint_of(const patch_op &op) {
  footprint fp;
  const auto elem_path = [&op] {
    path p = op.path;
    p.push_back(path_segment::element(op.elem));
    return p;
  };
  switch (op.kind) {
  case op_kind::create_record:
    fp.add({.record = op.target, .path = {}, .mode = access::create});
    break;
  case op_kind::delete_record:
    fp.add({.record = op.target, .path = {}, .mode = access::remove});
    break;
  case op_kind::set:
  case op_kind::unset:
    fp.add({.record = op.target, .path = op.path, .mode = access::write});
    break;
  case op_kind::insert_elem:
    fp.add({.record = op.target, .path = elem_path(), .mode = access::create});
    break;
  case op_kind::remove_elem:
    fp.add({.record = op.target, .path = elem_path(), .mode = access::remove});
    break;
  case op_kind::move_elem:
    fp.add({.record = op.target, .path = elem_path(), .mode = access::write});
    break;
  case op_kind::retype_record:
    fp.add({.record = op.target, .path = {}, .mode = access::write});
    break;
  case op_kind::annotate:
    break;
  }
  return fp;
}

footprint footprint_of(const patch &p) {
  footprint fp;
  for (const patch_op &op : p.ops) {
    fp.merge(footprint_of(op));
  }
  return fp;
}

bool commutes(const footprint &a, const footprint &b) noexcept { return !a.conflicts_with(b); }

result<patch> rebase_if_disjoint(const patch &p, const patch &onto) {
  const footprint mine = footprint_of(p);
  const footprint theirs = footprint_of(onto);
  const std::vector<footprint_overlap> overlap = mine.overlaps(theirs);
  if (!overlap.empty()) {
    return make_error(errc::conflict,
                      std::format("cannot rebase: {} overlaps {} ({} overlapping pairs)",
                                  entry_text(overlap.front().first),
                                  entry_text(overlap.front().second), overlap.size()));
  }
  return p;
}

// ---- order keys ---------------------------------------------------------------------------

result<order_key> between(byte_span lo, byte_span hi) {
  if (!lo.empty() && std::ranges::equal(lo, hi)) {
    if (!is_valid_order_key(lo)) {
      return make_error(errc::invalid_argument, "between: the bound is not a valid order key");
    }
    return order_key(lo.begin(), lo.end());
  }
  return order_key_between(lo, hi);
}

std::strong_ordering compare_order(byte_span key_a, const id_bytes &elem_a, byte_span key_b,
                                   const id_bytes &elem_b) noexcept {
  if (const auto c = std::lexicographical_compare_three_way(key_a.begin(), key_a.end(),
                                                            key_b.begin(), key_b.end());
      std::is_neq(c)) {
    return c;
  }
  return elem_a <=> elem_b;
}

} // namespace sl::fabric
