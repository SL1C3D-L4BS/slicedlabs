// Seeded random types and values (WO-039). See value_gen.hpp.

#include "value_gen.hpp"

#include "sl/core/assert.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace sl::fabric::testing {

namespace {

/// The value of a construction that cannot fail for the generator's inputs.
value ok(result<value> r) {
  SL_ASSERT(r.has_value(), "value_gen built a value SLB/1 cannot hold");
  return r ? std::move(*r) : value{};
}

std::unexpected<error> unresolved(const type_id &id) {
  return make_error(errc::schema_unknown,
                    std::format("value_gen: type {} is not registered", id.to_string()));
}

constexpr std::array<std::string_view, 16> scalar_names = {
    "bool", "i8",  "i16", "i32",    "i64",   "u8", "u16",    "u32",
    "u64",  "f32", "f64", "string", "bytes", "id", "digest", "blob_ref"};

constexpr std::array<std::string_view, 5> key_names = {"string", "u64", "i32", "id", "u8"};

/// Code points of 1 to 4 UTF-8 bytes, a noncharacter included.
constexpr std::array<std::string_view, 12> glyphs = {"a",
                                                     "b",
                                                     "z",
                                                     "A",
                                                     "Q",
                                                     "0",
                                                     "7",
                                                     " ",
                                                     "\xC3\xA9",
                                                     "\xE2\x82\xAC",
                                                     "\xF0\x9F\x98\x80",
                                                     "\xEF\xBF\xBF"};

struct int_limits {
  bool is_signed = false;
  std::uint64_t max = 0; // largest uint, and largest nint magnitude when signed
};

int_limits limits_of(type_kind k) noexcept {
  switch (k) {
  case type_kind::i8:
    return {.is_signed = true, .max = 127};
  case type_kind::i16:
    return {.is_signed = true, .max = 32767};
  case type_kind::i32:
    return {.is_signed = true, .max = 2147483647};
  case type_kind::i64:
    return {.is_signed = true, .max = 9223372036854775807ULL};
  case type_kind::u8:
    return {.is_signed = false, .max = 255};
  case type_kind::u16:
    return {.is_signed = false, .max = 65535};
  case type_kind::u32:
    return {.is_signed = false, .max = 4294967295ULL};
  default:
    return {.is_signed = false, .max = std::numeric_limits<std::uint64_t>::max()};
  }
}

} // namespace

std::uint64_t rng::next() noexcept {
  state_ += 0x9E3779B97F4A7C15ULL;
  std::uint64_t z = state_;
  z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31U);
}

std::uint64_t rng::below(std::uint64_t n) noexcept {
  if (n == 0) {
    return 0;
  }
  const std::uint64_t threshold = (0 - n) % n; // 2^64 mod n: reject below it, no modulo bias
  while (true) {
    const std::uint64_t r = next();
    if (r >= threshold) {
      return r % n;
    }
  }
}

record_id value_gen::random_id(id_class c) {
  id_bytes b{};
  for (auto &x : b) {
    x = static_cast<std::uint8_t>(rng_.next());
  }
  b[0] = static_cast<std::uint8_t>(c);
  b[1] = static_cast<std::uint8_t>(1 + rng_.below(4));
  b[15] |= 1U; // never an all-zero payload
  const auto id = record_id::from_bytes(b);
  SL_ASSERT(id.has_value(), "value_gen made an invalid id");
  return id ? *id : record_id{};
}

std::vector<std::uint8_t> value_gen::random_order_key() {
  std::vector<std::uint8_t> key(1 + rng_.below(4));
  for (auto &x : key) {
    x = static_cast<std::uint8_t>(rng_.next());
  }
  key.back() = static_cast<std::uint8_t>(1 + rng_.below(255));
  return key;
}

namespace {

std::string random_text(rng &r, std::size_t max_chars) {
  std::string s;
  const std::uint64_t n = r.below(max_chars + 1);
  for (std::uint64_t i = 0; i < n; ++i) {
    s += std::span(glyphs)[r.below(glyphs.size())];
  }
  return s;
}

std::vector<std::uint8_t> random_bytes(rng &r, std::size_t max_len) {
  std::vector<std::uint8_t> b(r.below(max_len + 1));
  for (auto &x : b) {
    x = static_cast<std::uint8_t>(r.next());
  }
  return b;
}

value random_f32(rng &r) {
  switch (r.below(4)) {
  case 0:
    return ok(value::f32(0.0F));
  case 1:
    return ok(value::f32(-0.0F)); // stored as +0.0
  case 2:
    return ok(value::f32(1.5F));
  default:
    break;
  }
  auto bits = static_cast<std::uint32_t>(r.next());
  if ((bits & 0x7F800000U) == 0x7F800000U) {
    bits &= 0xBFFFFFFFU; // clear an exponent bit: finite
  }
  return ok(value::f32(std::bit_cast<float>(bits)));
}

value random_f64(rng &r) {
  switch (r.below(4)) {
  case 0:
    return ok(value::f64(0.0));
  case 1:
    return ok(value::f64(-0.0));
  case 2:
    return ok(value::f64(0.1));
  default:
    break;
  }
  std::uint64_t bits = r.next();
  if ((bits & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL) {
    bits &= 0xBFFFFFFFFFFFFFFFULL;
  }
  return ok(value::f64(std::bit_cast<double>(bits)));
}

value random_digest(rng &r, std::uint8_t algo) {
  digest d;
  d.algo = static_cast<hash_algo>(algo);
  for (auto &x : d.bytes) {
    x = static_cast<std::uint8_t>(r.next());
  }
  return value::digest(d);
}

/// An integer of the kind's width, boundaries often.
value random_integer(rng &r, type_kind k) {
  const int_limits l = limits_of(k);
  const bool negative = l.is_signed && r.chance(40);
  std::uint64_t m = 0;
  switch (r.below(4)) {
  case 0:
    m = 0;
    break;
  case 1:
    m = l.max;
    break;
  default:
    m = l.max == std::numeric_limits<std::uint64_t>::max() ? r.next() : r.below(l.max + 1);
    break;
  }
  return negative ? value::nint(m) : value::uint(m);
}

} // namespace

value value_gen::any_scalar() {
  switch (rng_.below(9)) {
  case 0:
    return value::boolean(rng_.chance(50));
  case 1: {
    constexpr std::array<std::uint64_t, 6> edges = {
        0, 127, 128, 16383, 16384, std::numeric_limits<std::uint64_t>::max()};
    return value::uint(rng_.chance(50) ? std::span(edges)[rng_.below(edges.size())] : rng_.next());
  }
  case 2:
    return value::nint(rng_.chance(30) ? std::numeric_limits<std::uint64_t>::max()
                                       : rng_.below(1U << 20U));
  case 3:
    return random_f32(rng_);
  case 4:
    return random_f64(rng_);
  case 5:
    return ok(value::string(random_text(rng_, options_.max_chars)));
  case 6:
    return ok(value::bytes(random_bytes(rng_, options_.max_chars)));
  case 7: {
    if (rng_.chance(50)) {
      return value::id(random_id(static_cast<id_class>(1 + rng_.below(15))));
    }
    id_bytes raw{};
    for (auto &x : raw) {
      x = static_cast<std::uint8_t>(rng_.next());
    }
    return value::id(raw);
  }
  default:
    return random_digest(rng_, static_cast<std::uint8_t>(rng_.next()));
  }
}

value value_gen::any() { return any_at(0); }

value value_gen::any_at(std::size_t depth) {
  if (depth >= options_.max_depth || rng_.chance(45)) {
    return any_scalar();
  }
  const std::uint64_t n = rng_.below(options_.max_items + 1);
  switch (rng_.below(5)) {
  case 0: {
    std::vector<value> items;
    items.reserve(n);
    for (std::uint64_t i = 0; i < n; ++i) {
      items.push_back(any_at(depth + 1));
    }
    return ok(value::list(std::move(items)));
  }
  case 1: {
    std::vector<map_entry> entries;
    for (std::uint64_t i = 0; i < n; ++i) {
      value key = any_scalar();
      while (!is_key_kind(key.kind())) {
        key = any_scalar();
      }
      const bool dup = std::ranges::any_of(
          entries, [&](const map_entry &e) { return std::is_eq(compare_keys(e.key, key)); });
      if (!dup) {
        entries.push_back(map_entry{.key = std::move(key), .v = any_at(depth + 1)});
      }
    }
    return ok(value::map(std::move(entries)));
  }
  case 2: {
    std::vector<field> fields;
    for (std::uint64_t i = 0; i < n; ++i) {
      const auto no =
          static_cast<std::uint32_t>(rng_.chance(70) ? rng_.below(20) : rng_.below(1ULL << 32U));
      if (std::ranges::find(fields, no, &field::no) == fields.end()) {
        fields.push_back(field{.no = no, .v = any_at(depth + 1)});
      }
    }
    return ok(value::struct_(std::move(fields)));
  }
  case 3: {
    const auto c =
        static_cast<std::uint32_t>(rng_.chance(70) ? rng_.below(8) : rng_.below(1ULL << 32U));
    return ok(value::variant(c, any_at(depth + 1)));
  }
  default: {
    std::vector<oset_entry> entries;
    for (std::uint64_t i = 0; i < n; ++i) {
      id_bytes element{};
      for (auto &x : element) {
        x = static_cast<std::uint8_t>(rng_.next());
      }
      if (std::ranges::find(entries, element, &oset_entry::element) == entries.end()) {
        entries.push_back(oset_entry{
            .element = element, .order_key = random_bytes(rng_, 3), .v = any_at(depth + 1)});
      }
    }
    return ok(value::oset(std::move(entries)));
  }
  }
}

result<type_id> value_gen::add_type(schema_registry &reg) {
  return gen_type(reg, options_.max_depth, false);
}

result<type_id> value_gen::gen_type(schema_registry &reg, std::size_t depth, bool in_struct) {
  if (depth == 0 || rng_.chance(35)) {
    std::string_view name = std::span(scalar_names)[rng_.below(scalar_names.size())];
    if (in_struct && name == "id") {
      name = "u64"; // a struct with an id field is identity-bearing (no lists of it)
    }
    return type_id_of(name);
  }
  std::string name;
  do {
    name = std::format("gen.t{}", next_name_++);
  } while (reg.find(name) != nullptr);
  type_desc t;
  t.name = name;
  t.id = type_id_of(name);
  t.doc = "generated";
  const std::uint64_t n = rng_.below(options_.max_items + 1);
  switch (rng_.below(7)) {
  case 0: {
    t.kind = type_kind::struct_;
    std::uint32_t no = 0;
    for (std::uint64_t i = 0; i < n; ++i) {
      no += static_cast<std::uint32_t>(1 + rng_.below(3));
      auto ft = gen_type(reg, depth - 1, true);
      if (!ft) {
        return ft;
      }
      field_desc f;
      f.no = no;
      f.name = std::format("f{}", no);
      f.type = *ft;
      if (rng_.chance(40)) {
        f.presence = presence::optional;
        if (rng_.chance(50)) {
          auto d = gen_value(reg, reg.find(*ft), nullptr, options_.max_depth);
          if (!d) {
            return std::unexpected(std::move(d.error()));
          }
          f.default_value = std::move(*d);
        }
      }
      t.fields.push_back(std::move(f));
    }
    if (rng_.chance(30)) {
      t.reserved.push_back(no + 1);
    }
    break;
  }
  case 1: {
    t.kind = type_kind::enum_;
    auto no = static_cast<std::uint32_t>(rng_.below(2));
    for (std::uint64_t i = 0; i <= n; ++i) {
      enumerator_desc e;
      e.no = no;
      e.name = std::format("e{}", no);
      t.enumerators.push_back(std::move(e));
      no += static_cast<std::uint32_t>(1 + rng_.below(3));
    }
    break;
  }
  case 2: {
    t.kind = type_kind::variant;
    std::uint32_t no = 0;
    for (std::uint64_t i = 0; i <= std::min<std::uint64_t>(n, 2); ++i) {
      no += static_cast<std::uint32_t>(1 + rng_.below(3));
      auto ct = gen_type(reg, depth - 1, false);
      if (!ct) {
        return ct;
      }
      case_desc c;
      c.no = no;
      c.name = std::format("c{}", no);
      c.type = *ct;
      t.cases.push_back(std::move(c));
    }
    break;
  }
  case 3:
  case 5: {
    t.kind = rng_.chance(50) ? type_kind::list : type_kind::oset;
    auto et = gen_type(reg, depth - 1, false);
    if (!et) {
      return et;
    }
    t.element = *et;
    break;
  }
  case 4: {
    t.kind = type_kind::map;
    t.key = type_id_of(std::span(key_names)[rng_.below(key_names.size())]);
    auto et = gen_type(reg, depth - 1, false);
    if (!et) {
      return et;
    }
    t.element = *et;
    break;
  }
  default: {
    t.kind = type_kind::tagged;
    auto et = gen_type(reg, 0, in_struct);
    if (!et) {
      return et;
    }
    t.element = *et;
    break;
  }
  }
  const type_id id = t.id;
  if (auto added = reg.add(std::move(t)); !added) {
    return std::unexpected(std::move(added.error()));
  }
  return id;
}

result<value> value_gen::conforming(const schema_registry &reg, type_id type) {
  const type_desc *t = reg.find(type);
  if (t == nullptr) {
    return unresolved(type);
  }
  return gen_value(reg, t, nullptr, 0);
}

result<value> value_gen::gen_value(const schema_registry &reg, const type_desc *t,
                                   const ref_spec *ref, std::size_t depth) {
  for (std::size_t steps = 0; t != nullptr && t->kind == type_kind::tagged; ++steps) {
    if (steps > reg.size()) {
      return make_error(errc::schema_invalid, "value_gen: a tagged chain loops");
    }
    const type_id base = t->element.value_or(type_id{});
    t = reg.find(base);
    if (t == nullptr) {
      return unresolved(base);
    }
  }
  if (t == nullptr) {
    return make_error(errc::schema_unknown, "value_gen: no type");
  }
  const bool room = depth < options_.max_depth;
  const std::uint64_t n = room ? rng_.below(options_.max_items + 1) : 0;
  switch (t->kind) {
  case type_kind::bool_:
    return value::boolean(rng_.chance(50));
  case type_kind::i8:
  case type_kind::i16:
  case type_kind::i32:
  case type_kind::i64:
  case type_kind::u8:
  case type_kind::u16:
  case type_kind::u32:
  case type_kind::u64:
    return random_integer(rng_, t->kind);
  case type_kind::f32:
    return random_f32(rng_);
  case type_kind::f64:
    return random_f64(rng_);
  case type_kind::string:
    return value::string(random_text(rng_, options_.max_chars));
  case type_kind::bytes:
    return value::bytes(random_bytes(rng_, options_.max_chars));
  case type_kind::id:
    return value::id(random_id(static_cast<id_class>(1 + rng_.below(15))));
  case type_kind::digest:
  case type_kind::blob_ref:
    return random_digest(rng_, static_cast<std::uint8_t>(hash_algo::sha256));
  case type_kind::ref: {
    id_class cls = id_class::entity;
    if (ref != nullptr && !ref->targets.empty()) {
      const type_desc *target = reg.find(ref->targets[rng_.below(ref->targets.size())]);
      if (target == nullptr) {
        return make_error(errc::schema_unknown, "value_gen: a ref target is not registered");
      }
      cls = target->record_class;
    }
    return value::id(random_id(cls));
  }
  case type_kind::enum_:
    return value::uint(t->enumerators[rng_.below(t->enumerators.size())].no);
  case type_kind::struct_:
  case type_kind::external_ref: {
    std::vector<field> fields;
    for (const field_desc &f : t->fields) {
      if (f.presence == presence::optional && !(room && rng_.chance(50))) {
        continue;
      }
      const type_desc *ft = reg.find(f.type);
      if (ft == nullptr) {
        return unresolved(f.type);
      }
      auto v = gen_value(reg, ft, f.ref ? &*f.ref : nullptr, depth + 1);
      if (!v) {
        return v;
      }
      fields.push_back(field{.no = f.no, .v = std::move(*v)});
    }
    return value::struct_(std::move(fields));
  }
  case type_kind::list: {
    std::vector<value> items;
    for (std::uint64_t i = 0; i < n; ++i) {
      auto v = gen_value(reg, reg.find(t->element.value_or(type_id{})), ref, depth + 1);
      if (!v) {
        return v;
      }
      items.push_back(std::move(*v));
    }
    return value::list(std::move(items));
  }
  case type_kind::map: {
    std::vector<map_entry> entries;
    for (std::uint64_t i = 0; i < n; ++i) {
      auto k = gen_value(reg, reg.find(t->key.value_or(type_id{})), nullptr, depth + 1);
      if (!k) {
        return k;
      }
      if (std::ranges::any_of(
              entries, [&](const map_entry &e) { return std::is_eq(compare_keys(e.key, *k)); })) {
        continue;
      }
      auto v = gen_value(reg, reg.find(t->element.value_or(type_id{})), ref, depth + 1);
      if (!v) {
        return v;
      }
      entries.push_back(map_entry{.key = std::move(*k), .v = std::move(*v)});
    }
    return value::map(std::move(entries));
  }
  case type_kind::oset: {
    std::vector<oset_entry> entries;
    for (std::uint64_t i = 0; i < n; ++i) {
      const id_bytes element = random_id(id_class::element).bytes();
      if (std::ranges::find(entries, element, &oset_entry::element) != entries.end()) {
        continue;
      }
      auto v = gen_value(reg, reg.find(t->element.value_or(type_id{})), ref, depth + 1);
      if (!v) {
        return v;
      }
      entries.push_back(
          oset_entry{.element = element, .order_key = random_order_key(), .v = std::move(*v)});
    }
    return value::oset(std::move(entries));
  }
  case type_kind::variant: {
    const case_desc &c = t->cases[rng_.below(t->cases.size())];
    auto v = gen_value(reg, reg.find(c.type), nullptr, depth + 1);
    if (!v) {
      return v;
    }
    return value::variant(c.no, std::move(*v));
  }
  case type_kind::tagged:
    break;
  }
  return make_error(errc::schema_invalid, "value_gen: unhandled kind");
}

} // namespace sl::fabric::testing
