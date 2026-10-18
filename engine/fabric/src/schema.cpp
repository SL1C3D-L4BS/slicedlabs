// The schema registry (WO-039, SL-ARCH-001 ARCH1-R12 to ARCH1-R18). See schema.hpp.

#include "sl/fabric/schema.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <set>
#include <utility>

namespace sl::fabric {

namespace {

using type_map = std::map<type_id, type_desc>;

const type_desc *lookup(const type_map &types, const type_id &id) noexcept {
  const auto it = types.find(id);
  return it == types.end() ? nullptr : &it->second;
}

/// `t` with tagged bases followed; null when a base is missing or the chain loops.
const type_desc *resolve(const type_map &types, const type_desc *t) noexcept {
  for (std::size_t steps = 0; t != nullptr && t->kind == type_kind::tagged; ++steps) {
    if (steps > types.size() || !t->element) {
      return nullptr;
    }
    t = lookup(types, *t->element);
  }
  return t;
}

/// True when a ref is reached from `id` through tagged, list, map value and oset element.
bool reaches_ref(const type_map &types, const type_id &id) noexcept {
  const type_desc *t = lookup(types, id);
  for (std::size_t steps = 0; t != nullptr && steps <= types.size(); ++steps) {
    switch (t->kind) {
    case type_kind::ref:
      return true;
    case type_kind::tagged:
    case type_kind::list:
    case type_kind::map:
    case type_kind::oset:
      if (!t->element) {
        return false;
      }
      t = lookup(types, *t->element);
      break;
    default:
      return false;
    }
  }
  return false;
}

/// A record type, or a struct with a field of kind id (through tagged).
bool identity_bearing(const type_map &types, const type_desc &t) noexcept {
  if (t.is_record()) {
    return true;
  }
  if (t.kind != type_kind::struct_) {
    return false;
  }
  return std::ranges::any_of(t.fields, [&](const field_desc &f) {
    const type_desc *ft = resolve(types, lookup(types, f.type));
    return ft != nullptr && ft->kind == type_kind::id;
  });
}

struct int_width {
  bool is_signed = false;
  unsigned bits = 0;
};

int_width width_of(type_kind k) noexcept {
  switch (k) {
  case type_kind::i8:
    return {.is_signed = true, .bits = 8};
  case type_kind::i16:
    return {.is_signed = true, .bits = 16};
  case type_kind::i32:
    return {.is_signed = true, .bits = 32};
  case type_kind::i64:
    return {.is_signed = true, .bits = 64};
  case type_kind::u8:
    return {.is_signed = false, .bits = 8};
  case type_kind::u16:
    return {.is_signed = false, .bits = 16};
  case type_kind::u32:
    return {.is_signed = false, .bits = 32};
  default:
    return {.is_signed = false, .bits = 64};
  }
}

/// The largest uint of the width, and the largest nint magnitude (value -(m + 1)).
std::uint64_t max_uint(int_width w) noexcept {
  const unsigned value_bits = w.is_signed ? w.bits - 1 : w.bits;
  return value_bits >= 64 ? std::numeric_limits<std::uint64_t>::max()
                          : (std::uint64_t{1} << value_bits) - 1;
}

std::string id_text(const id_bytes &raw) {
  const auto id = record_id::from_bytes(raw);
  if (id) {
    return id->to_string();
  }
  std::string out;
  for (const std::uint8_t b : raw) {
    out += std::format("{:02x}", b);
  }
  return out;
}

/// Value conformance (FAB-INV-TY-02).
class checker {
public:
  checker(const type_map &types, std::vector<diagnostic_lite> &out) noexcept
      : types_(&types), out_(&out) {}

  void conform(const value &v, const type_desc *t, const ref_spec *ref) {
    t = resolve(*types_, t);
    if (t == nullptr) {
      report("R-TY-UNRESOLVED", "the type or a tagged base is not registered");
      return;
    }
    switch (t->kind) {
    case type_kind::bool_:
      expect(v, value_kind::bool_, *t);
      return;
    case type_kind::i8:
    case type_kind::i16:
    case type_kind::i32:
    case type_kind::i64:
    case type_kind::u8:
    case type_kind::u16:
    case type_kind::u32:
    case type_kind::u64:
      integer(v, *t);
      return;
    case type_kind::f32:
      expect(v, value_kind::f32, *t);
      return;
    case type_kind::f64:
      expect(v, value_kind::f64, *t);
      return;
    case type_kind::string:
      expect(v, value_kind::string, *t);
      return;
    case type_kind::bytes:
      expect(v, value_kind::bytes, *t);
      return;
    case type_kind::digest:
    case type_kind::blob_ref:
      expect(v, value_kind::digest, *t);
      return;
    case type_kind::id:
      if (expect(v, value_kind::id, *t)) {
        valid_id(v);
      }
      return;
    case type_kind::ref:
      if (expect(v, value_kind::id, *t) && valid_id(v) && ref != nullptr) {
        ref_target(v, *ref);
      }
      return;
    case type_kind::enum_:
      if (expect(v, value_kind::uint, *t) &&
          v.as_uint() <= std::numeric_limits<std::uint32_t>::max() &&
          t->find_enumerator(static_cast<std::uint32_t>(v.as_uint())) != nullptr) {
        return;
      }
      if (v.kind() == value_kind::uint) {
        report("R-TY-ENUM", std::format("{} is no enumerator of {}", v.as_uint(), t->name));
      }
      return;
    case type_kind::struct_:
    case type_kind::external_ref:
      structure(v, *t);
      return;
    case type_kind::list:
      if (expect(v, value_kind::list, *t)) {
        const type_desc *et = lookup(*types_, t->element.value_or(type_id{}));
        for (std::size_t i = 0; i < v.items().size(); ++i) {
          const std::size_t start = out_->size();
          conform(v.items()[i], et, ref);
          for (std::size_t k = start; k < out_->size(); ++k) {
            (*out_)[k].message = std::format("list item {}: {}", i, (*out_)[k].message);
          }
        }
      }
      return;
    case type_kind::map:
      if (expect(v, value_kind::map, *t)) {
        const type_desc *kt = lookup(*types_, t->key.value_or(type_id{}));
        const type_desc *et = lookup(*types_, t->element.value_or(type_id{}));
        for (const map_entry &e : v.entries()) {
          // A map value always holds key kinds, so the segment is always made.
          path_.push_back(*path_segment::key(e.key));
          conform(e.key, kt, nullptr);
          conform(e.v, et, ref);
          path_.pop_back();
        }
      }
      return;
    case type_kind::oset:
      if (expect(v, value_kind::oset, *t)) {
        const type_desc *et = lookup(*types_, t->element.value_or(type_id{}));
        for (const oset_entry &e : v.elements()) {
          path_.push_back(path_segment::element(e.element));
          const auto id = record_id::from_bytes(e.element);
          if (!id || id->cls() != id_class::element) {
            report("R-TY-ELEMENT",
                   std::format("element id {} is not an element id", id_text(e.element)));
          }
          if (!is_valid_order_key(e.order_key)) {
            report("R-TY-ORDER-KEY", "the order key is empty, too long or ends in a zero byte");
          }
          conform(e.v, et, ref);
          path_.pop_back();
        }
      }
      return;
    case type_kind::variant:
      if (expect(v, value_kind::variant, *t)) {
        const case_desc *c = t->find_case(v.case_no());
        if (c == nullptr) {
          report("R-TY-CASE", std::format("case {} is not a case of {}", v.case_no(), t->name));
          return;
        }
        path_.push_back(path_segment::field(c->no));
        conform(v.payload(), lookup(*types_, c->type), nullptr);
        path_.pop_back();
      }
      return;
    case type_kind::tagged:
      return; // resolved above
    }
  }

private:
  void report(std::string_view rule, std::string message) {
    out_->push_back(
        diagnostic_lite{.path = path_, .rule = std::string(rule), .message = std::move(message)});
  }

  bool expect(const value &v, value_kind k, const type_desc &t) {
    if (v.kind() == k) {
      return true;
    }
    report("R-TY-KIND", std::format("a {} where {} ({}) needs a {}", to_string(v.kind()), t.name,
                                    to_string(t.kind), to_string(k)));
    return false;
  }

  void integer(const value &v, const type_desc &t) {
    const int_width w = width_of(t.kind);
    if (v.kind() == value_kind::uint) {
      if (v.as_uint() > max_uint(w)) {
        report("R-TY-RANGE", std::format("{} is outside {}", v.as_uint(), to_string(t.kind)));
      }
      return;
    }
    if (v.kind() == value_kind::nint && w.is_signed) {
      if (v.nint_magnitude() > max_uint(w)) {
        report("R-TY-RANGE",
               std::format("-({} + 1) is outside {}", v.nint_magnitude(), to_string(t.kind)));
      }
      return;
    }
    if (v.kind() == value_kind::nint) {
      report("R-TY-RANGE", std::format("a negative integer is outside {}", to_string(t.kind)));
      return;
    }
    (void)expect(v, value_kind::uint, t);
  }

  bool valid_id(const value &v) {
    if (record_id::from_bytes(v.as_id_bytes())) {
      return true;
    }
    report("R-TY-ID", std::format("{} is not a valid record id", id_text(v.as_id_bytes())));
    return false;
  }

  void ref_target(const value &v, const ref_spec &ref) {
    const auto cls = static_cast<id_class>(v.as_id_bytes()[0]);
    for (const type_id &target : ref.targets) {
      const type_desc *tt = lookup(*types_, target);
      if (tt != nullptr && tt->record_class == cls) {
        return;
      }
    }
    report("R-TY-REF-TARGET",
           std::format("a {} id is the record class of no ref target", fabric::to_string(cls)));
  }

  void structure(const value &v, const type_desc &t) {
    if (!expect(v, value_kind::struct_, t)) {
      return;
    }
    for (const field_desc &f : t.fields) {
      path_.push_back(path_segment::field(f.no));
      if (const value *fv = v.find_field(f.no)) {
        conform(*fv, lookup(*types_, f.type), f.ref ? &*f.ref : nullptr);
      } else if (f.presence == presence::required) {
        report("R-TY-REQUIRED", std::format("required field {} ({}) is absent", f.no, f.name));
      }
      path_.pop_back();
    }
    for (const field &fl : v.fields()) {
      if (t.find_field(fl.no) != nullptr) {
        continue;
      }
      path_.push_back(path_segment::field(fl.no));
      if (std::ranges::find(t.reserved, fl.no) != t.reserved.end()) {
        report("R-TY-RESERVED", std::format("field {} is reserved in {}", fl.no, t.name));
      } else {
        report("R-TY-FIELD", std::format("field {} is not declared by {}", fl.no, t.name));
      }
      path_.pop_back();
    }
  }

  const type_map *types_;
  std::vector<diagnostic_lite> *out_;
  path path_;
};

struct type_violation {
  std::string type;
  std::string rule;
  std::string message;

  friend auto operator<=>(const type_violation &, const type_violation &) = default;
};

/// The hard edges of `t` (FAB-INV-TY-01): required fields, variant cases, tagged base.
std::vector<type_id> hard_edges(const type_desc &t) {
  std::vector<type_id> out;
  if (t.kind == type_kind::struct_ || t.kind == type_kind::external_ref) {
    for (const field_desc &f : t.fields) {
      if (f.presence == presence::required) {
        out.push_back(f.type);
      }
    }
  } else if (t.kind == type_kind::variant) {
    for (const case_desc &c : t.cases) {
      out.push_back(c.type);
    }
  } else if (t.kind == type_kind::tagged && t.element) {
    out.push_back(*t.element);
  }
  return out;
}

/// Every cycle of hard edges, reported once at the type where the depth-first search (in type
/// id order) closes it.
void find_recursion(const type_map &types, std::vector<type_violation> &out) {
  enum class mark : std::uint8_t { white, grey, black };
  std::map<type_id, mark> marks;
  std::vector<type_id> stack;
  const auto visit = [&](const auto &self, const type_id &id) -> void {
    marks[id] = mark::grey;
    stack.push_back(id);
    const type_desc *t = lookup(types, id);
    for (const type_id &next : hard_edges(*t)) {
      if (lookup(types, next) == nullptr) {
        continue;
      }
      const mark m = marks.contains(next) ? marks[next] : mark::white;
      if (m == mark::white) {
        self(self, next);
      } else if (m == mark::grey) {
        std::string cycle;
        const auto start = std::ranges::find(stack, next);
        for (auto it = start; it != stack.end(); ++it) {
          cycle += lookup(types, *it)->name + " -> ";
        }
        cycle += lookup(types, next)->name;
        out.push_back(type_violation{.type = t->name,
                                     .rule = "R-TY-RECURSION",
                                     .message = "a cycle without an optional field, list, map or "
                                                "oset: " +
                                                cycle});
      }
    }
    stack.pop_back();
    marks[id] = mark::black;
  };
  for (const auto &[id, t] : types) {
    if (!marks.contains(id)) {
      visit(visit, id);
    }
  }
}

} // namespace

schema_registry::schema_registry() {
  for (const type_desc &t : builtin_types()) {
    types_.emplace(t.id, t);
  }
}

result<void> schema_registry::add(type_desc t) {
  if (frozen_) {
    return make_error(errc::schema_frozen,
                      std::format("type '{}': the schema registry is frozen", t.name));
  }
  if (auto ok = check_well_formed(t); !ok) {
    return ok;
  }
  if (types_.contains(t.id)) {
    return make_error(errc::schema_invalid,
                      std::format("R-TY-DUP: type '{}' is already registered", t.name));
  }
  const type_id id = t.id;
  types_.emplace(id, std::move(t));
  return {};
}

result<void> schema_registry::freeze() {
  if (frozen_) {
    return {};
  }
  std::vector<type_violation> bad;
  const auto flag = [&bad](const type_desc &t, std::string_view rule, std::string message) {
    bad.push_back(
        type_violation{.type = t.name, .rule = std::string(rule), .message = std::move(message)});
  };
  bool unresolved = false;
  for (const auto &[id, t] : types_) {
    // FAB-INV-TY-05 (ARCH1-R17).
    if (t.is_record()) {
      if (!is_valid(t.record_class)) {
        flag(t, "R-TY-RECORD", "a record type needs an assigned record class");
      }
      if (t.domain.empty()) {
        flag(t, "R-TY-DOMAIN", "a record type needs a domain");
      }
      const auto sc = static_cast<std::uint8_t>(t.state_class);
      if (sc < 1 || sc > 3) {
        flag(t, "R-TY-STATE",
             std::format("state class {} is not document, derived or observation", sc));
      }
    }
    // R-TY-UNRESOLVED.
    const auto need = [&](const type_id &ref, std::string_view what) {
      if (lookup(types_, ref) == nullptr) {
        flag(t, "R-TY-UNRESOLVED", std::format("{} {} is not registered", what, ref.to_string()));
        unresolved = true;
      }
    };
    for (const field_desc &f : t.fields) {
      need(f.type, std::format("field {} type", f.no));
      if (f.ref) {
        for (const type_id &target : f.ref->targets) {
          need(target, std::format("field {} ref target", f.no));
        }
      }
    }
    for (const case_desc &c : t.cases) {
      need(c.type, std::format("case {} type", c.no));
    }
    if (t.element) {
      need(*t.element, "element type");
    }
    if (t.key) {
      need(*t.key, "key type");
    }
    // R-TY-KEY.
    if (t.key) {
      const type_desc *kt = resolve(types_, lookup(types_, *t.key));
      if (kt != nullptr && !is_integer(kt->kind) && kt->kind != type_kind::string &&
          kt->kind != type_kind::id) {
        flag(t, "R-TY-KEY", std::format("key type {} is not a string, integer or id", kt->name));
      }
    }
    // R-TY-REF (ARCH1-R18).
    for (const field_desc &f : t.fields) {
      const bool has_ref = reaches_ref(types_, f.type);
      if (has_ref && !f.ref) {
        flag(t, "R-TY-REF", std::format("field {} holds refs without targets and strength", f.no));
      }
      if (!has_ref && f.ref) {
        flag(t, "R-TY-REF", std::format("field {} has a ref spec but holds no ref", f.no));
      }
      if (f.ref) {
        for (const type_id &target : f.ref->targets) {
          const type_desc *tt = lookup(types_, target);
          if (tt != nullptr && !tt->is_record()) {
            flag(t, "R-TY-REF",
                 std::format("field {} ref target {} is not a record type", f.no, tt->name));
          }
        }
      }
    }
    for (const case_desc &c : t.cases) {
      if (reaches_ref(types_, c.type)) {
        flag(t, "R-TY-REF", std::format("case {} holds refs, which need a field", c.no));
      }
    }
    // R-TY-LIST-ID (FAB-INV-TY-06, ARCH1-R15).
    if (t.kind == type_kind::list && t.element) {
      const type_desc *et = resolve(types_, lookup(types_, *t.element));
      if (et != nullptr && identity_bearing(types_, *et)) {
        flag(t, "R-TY-LIST-ID",
             std::format("a list of identity-bearing {} must be an oset", et->name));
      }
    }
  }
  // R-TY-RECURSION (FAB-INV-TY-01).
  const std::size_t before = bad.size();
  find_recursion(types_, bad);
  // R-TY-DEFAULT: only once every type resolves and no tagged chain loops.
  if (!unresolved && bad.size() == before) {
    for (const auto &[id, t] : types_) {
      for (const field_desc &f : t.fields) {
        if (!f.default_value) {
          continue;
        }
        std::vector<diagnostic_lite> diags;
        checker c(types_, diags);
        c.conform(*f.default_value, lookup(types_, f.type), f.ref ? &*f.ref : nullptr);
        if (!diags.empty()) {
          std::ranges::sort(diags);
          flag(t, "R-TY-DEFAULT",
               std::format("field {} default: {}: {}", f.no, diags.front().rule,
                           diags.front().message));
        }
      }
    }
  }
  if (!bad.empty()) {
    std::ranges::sort(bad);
    std::string message = std::format("freeze refused, {} violation(s):", bad.size());
    for (const type_violation &v : bad) {
      message += std::format(" {}: type '{}': {};", v.rule, v.type, v.message);
    }
    return make_error(errc::schema_invalid, std::move(message));
  }
  frozen_ = true;
  return {};
}

const type_desc *schema_registry::find(type_id id) const noexcept { return lookup(types_, id); }

const type_desc *schema_registry::find(std::string_view qualified_name) const noexcept {
  const type_desc *t = lookup(types_, type_id_of(qualified_name));
  return t != nullptr && t->name == qualified_name ? t : nullptr;
}

result<void> schema_registry::check(const value &v, type_id type,
                                    std::vector<diagnostic_lite> &out) const {
  if (!frozen_) {
    return make_error(errc::schema_invalid, "check() needs a frozen schema registry");
  }
  const type_desc *t = find(type);
  if (t == nullptr) {
    return make_error(errc::schema_unknown,
                      std::format("type {} is not registered", type.to_string()));
  }
  std::vector<diagnostic_lite> diags;
  checker c(types_, diags);
  c.conform(v, t, nullptr);
  if (diags.empty()) {
    return {};
  }
  std::ranges::sort(diags);
  const diagnostic_lite first = diags.front();
  const std::size_t n = diags.size();
  out.insert(out.end(), std::make_move_iterator(diags.begin()),
             std::make_move_iterator(diags.end()));
  return make_error(
      errc::type_mismatch,
      std::format("value does not conform to {}: {} diagnostic(s), first {} at {}: {}", t->name, n,
                  first.rule, to_string(first.path), first.message));
}

} // namespace sl::fabric
