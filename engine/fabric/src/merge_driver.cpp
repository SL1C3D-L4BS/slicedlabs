// The default field-level three-way merge driver and the driver registry (WO-043, SL-ARCH-002
// ARCH2-R47; ADR-0019).

#include <algorithm>
#include <compare>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/validate.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

namespace {

using opt = std::optional<value>;

opt child_field(const value *v, std::uint32_t no) {
  const value *c = v != nullptr ? v->find_field(no) : nullptr;
  return c != nullptr ? opt(*c) : std::nullopt;
}

opt child_key(const value *v, const value &key) {
  const value *c = v != nullptr ? v->find_key(key) : nullptr;
  return c != nullptr ? opt(*c) : std::nullopt;
}

const oset_entry *child_element(const value *v, const id_bytes &elem) {
  return v != nullptr ? v->find_element(elem) : nullptr;
}

bool same_shape(const value &a, const value &b) noexcept {
  if (a.kind() != b.kind()) {
    return false;
  }
  switch (a.kind()) {
  case value_kind::struct_:
  case value_kind::map:
  case value_kind::oset:
    return true;
  case value_kind::variant:
    return a.case_no() == b.case_no();
  default:
    return false;
  }
}

path extend(const path &p, path_segment s) {
  path out = p;
  out.push_back(std::move(s));
  return out;
}

class merger {
public:
  explicit merger(std::vector<merge_conflict> &out) noexcept : out_(&out) {}

  result<opt> merge3(const path &at, const opt &base, const opt &ours, const opt &theirs) {
    if (ours == theirs) {
      return ours;
    }
    if (ours == base) {
      return theirs;
    }
    if (theirs == base) {
      return ours;
    }
    if (ours && theirs && same_shape(*ours, *theirs)) {
      // A base of another shape counts as absent: both sides added the structure.
      const value *b = base && same_shape(*base, *ours) ? &*base : nullptr;
      return recurse(at, b, *ours, *theirs);
    }
    conflict(at, base, ours, theirs);
    return ours;
  }

private:
  void conflict(const path &at, const opt &base, const opt &ours, const opt &theirs) {
    out_->push_back(merge_conflict{.path = at, .base = base, .ours = ours, .theirs = theirs});
  }

  result<opt> recurse(const path &at, const value *base, const value &ours, const value &theirs) {
    switch (ours.kind()) {
    case value_kind::struct_:
      return merge_struct(at, base, ours, theirs);
    case value_kind::map:
      return merge_map(at, base, ours, theirs);
    case value_kind::oset:
      return merge_oset(at, base, ours, theirs);
    case value_kind::variant: {
      const std::uint32_t c = ours.case_no();
      auto payload = merge3(extend(at, path_segment::field(c)),
                            base != nullptr ? opt(base->payload()) : std::nullopt,
                            opt(ours.payload()), opt(theirs.payload()));
      if (!payload) {
        return std::unexpected(std::move(payload.error()));
      }
      if (!*payload) {
        return opt(ours);
      }
      auto v = value::variant(c, std::move(**payload));
      if (!v) {
        return std::unexpected(std::move(v.error()));
      }
      return opt(std::move(*v));
    }
    default:
      return opt(ours);
    }
  }

  result<opt> merge_struct(const path &at, const value *base, const value &ours,
                           const value &theirs) {
    std::vector<std::uint32_t> numbers;
    for (const value *v : {base, &ours, &theirs}) {
      if (v != nullptr) {
        for (const field &f : v->fields()) {
          numbers.push_back(f.no);
        }
      }
    }
    std::ranges::sort(numbers);
    const auto dup = std::ranges::unique(numbers);
    numbers.erase(dup.begin(), dup.end());
    std::vector<field> fields;
    for (const std::uint32_t no : numbers) {
      auto r = merge3(extend(at, path_segment::field(no)), child_field(base, no),
                      child_field(&ours, no), child_field(&theirs, no));
      if (!r) {
        return std::unexpected(std::move(r.error()));
      }
      if (*r) {
        fields.push_back(field{.no = no, .v = std::move(**r)});
      }
    }
    auto v = value::struct_(std::move(fields));
    if (!v) {
      return std::unexpected(std::move(v.error()));
    }
    return opt(std::move(*v));
  }

  result<opt> merge_map(const path &at, const value *base, const value &ours, const value &theirs) {
    std::vector<value> keys;
    for (const value *v : {base, &ours, &theirs}) {
      if (v != nullptr) {
        for (const map_entry &e : v->entries()) {
          keys.push_back(e.key);
        }
      }
    }
    std::ranges::sort(
        keys, [](const value &a, const value &b) { return std::is_lt(compare_keys(a, b)); });
    const auto dup = std::ranges::unique(
        keys, [](const value &a, const value &b) { return std::is_eq(compare_keys(a, b)); });
    keys.erase(dup.begin(), dup.end());
    std::vector<map_entry> entries;
    for (const value &k : keys) {
      auto seg = path_segment::key(k);
      if (!seg) {
        return std::unexpected(std::move(seg.error()));
      }
      auto r = merge3(extend(at, std::move(*seg)), child_key(base, k), child_key(&ours, k),
                      child_key(&theirs, k));
      if (!r) {
        return std::unexpected(std::move(r.error()));
      }
      if (*r) {
        entries.push_back(map_entry{.key = k, .v = std::move(**r)});
      }
    }
    auto v = value::map(std::move(entries));
    if (!v) {
      return std::unexpected(std::move(v.error()));
    }
    return opt(std::move(*v));
  }

  result<opt> merge_oset(const path &at, const value *base, const value &ours,
                         const value &theirs) {
    std::vector<id_bytes> elems;
    for (const value *v : {base, &ours, &theirs}) {
      if (v != nullptr) {
        for (const oset_entry &e : v->elements()) {
          elems.push_back(e.element);
        }
      }
    }
    std::ranges::sort(elems);
    const auto dup = std::ranges::unique(elems);
    elems.erase(dup.begin(), dup.end());
    std::vector<oset_entry> entries;
    for (const id_bytes &el : elems) {
      const oset_entry *eb = child_element(base, el);
      const oset_entry *eo = child_element(&ours, el);
      const oset_entry *et = child_element(&theirs, el);
      const path here = extend(at, path_segment::element(el));
      auto r = merge3(here, eb != nullptr ? opt(eb->v) : std::nullopt,
                      eo != nullptr ? opt(eo->v) : std::nullopt,
                      et != nullptr ? opt(et->v) : std::nullopt);
      if (!r) {
        return std::unexpected(std::move(r.error()));
      }
      if (!*r) {
        continue;
      }
      auto key = merge_key(here, eb, eo, et);
      if (!key) {
        return std::unexpected(std::move(key.error()));
      }
      entries.push_back(oset_entry{.element = el, .order_key = std::move(*key), .v = **r});
    }
    auto v = value::oset(std::move(entries));
    if (!v) {
      return std::unexpected(std::move(v.error()));
    }
    return opt(std::move(*v));
  }

  /// Three-way on the order key of an element that survives the merge; a conflict reports the
  /// keys as bytes values and keeps ours.
  result<std::vector<std::uint8_t>> merge_key(const path &at, const oset_entry *eb,
                                              const oset_entry *eo, const oset_entry *et) {
    using key_opt = std::optional<std::vector<std::uint8_t>>;
    const auto key_of = [](const oset_entry *e) {
      return e != nullptr ? key_opt(e->order_key) : std::nullopt;
    };
    const key_opt kb = key_of(eb);
    const key_opt ko = key_of(eo);
    const key_opt kt = key_of(et);
    key_opt merged;
    if (ko == kt || kt == kb) {
      merged = ko;
    } else if (ko == kb) {
      merged = kt;
    } else if (ko && kt) {
      auto as_value = [](const key_opt &k) -> result<opt> {
        if (!k) {
          return opt();
        }
        auto b = value::bytes(*k);
        if (!b) {
          return std::unexpected(std::move(b.error()));
        }
        return opt(std::move(*b));
      };
      auto vb = as_value(kb);
      auto vo = as_value(ko);
      auto vt = as_value(kt);
      if (!vb || !vo || !vt) {
        return make_error(errc::encode_invalid, "an order key cannot be held as bytes");
      }
      conflict(at, *vb, *vo, *vt);
      merged = ko;
    }
    if (!merged) {
      merged = ko ? ko : kt;
    }
    return merged ? std::move(*merged) : std::vector<std::uint8_t>{};
  }

  std::vector<merge_conflict> *out_;
};

} // namespace

result<merge_result> field_merge_driver::merge(const merge_input &in) const {
  merge_result out;
  merger m(out.conflicts);
  auto merged = m.merge3({}, in.base, in.ours, in.theirs);
  if (!merged) {
    return std::unexpected(std::move(merged.error()));
  }
  out.merged = std::move(*merged);
  std::ranges::stable_sort(out.conflicts, {}, &merge_conflict::path);
  return out;
}

merge_driver_registry::merge_driver_registry()
    : default_(std::make_shared<const field_merge_driver>()) {}

result<void> merge_driver_registry::add(const type_id &type,
                                        std::shared_ptr<const merge_driver> d) {
  if (frozen_) {
    return make_error(errc::registry_frozen, "the merge driver registry is frozen");
  }
  if (!d) {
    return make_error(errc::invalid_argument, "a null merge driver");
  }
  if (drivers_.contains(type)) {
    return make_error(errc::already_exists,
                      std::format("type {} has a merge driver already", type.to_string()));
  }
  drivers_.emplace(type, std::move(d));
  return {};
}

const merge_driver &merge_driver_registry::driver_for(const type_id &type) const noexcept {
  const auto it = drivers_.find(type);
  return it != drivers_.end() ? *it->second : *default_;
}

} // namespace sl::fabric
