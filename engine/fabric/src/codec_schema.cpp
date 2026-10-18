// Schema objects and the schema_set digest (WO-041, SL-ARCH-001 ARCH1-R20 and ARCH1-R22,
// ADR-0013). The type_def_value layout and its value encodings follow tools/fabric/schemalib.py
// (lock hashes in tests/fixtures/fabric/schema/lock_vectors.json).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

namespace {

/// Resolves type ids to qualified names: the type itself, the built-ins, then the registry.
class type_names {
public:
  type_names(const type_desc &self, const schema_registry *schemas) noexcept
      : self_(&self), schemas_(schemas) {}

  [[nodiscard]] result<std::string> name(const type_id &id) const {
    if (id == self_->id) {
      return self_->name;
    }
    for (const type_desc &b : builtin_types()) {
      if (b.id == id) {
        return b.name;
      }
    }
    if (schemas_ != nullptr) {
      if (const type_desc *t = schemas_->find(id); t != nullptr) {
        return t->name;
      }
    }
    return make_error(errc::schema_unknown,
                      std::format("type '{}' refers to {}, which is not registered", self_->name,
                                  id.to_string()));
  }

private:
  const type_desc *self_;
  const schema_registry *schemas_;
};

result<value> str(std::string_view s) { return value::string(s); }

/// Builds a struct from (number, value) pairs; every part is already a valid value.
class struct_builder {
public:
  void add(std::uint32_t no, value v) { fields_.push_back(field{.no = no, .v = std::move(v)}); }
  [[nodiscard]] result<void> add(std::uint32_t no, result<value> v) {
    if (!v) {
      return std::unexpected(v.error());
    }
    add(no, std::move(*v));
    return {};
  }
  [[nodiscard]] result<value> take() { return value::struct_(std::move(fields_)); }

private:
  std::vector<field> fields_;
};

template <class T> std::vector<const T *> sorted_by_no(const std::vector<T> &items) {
  std::vector<const T *> out;
  out.reserve(items.size());
  for (const T &x : items) {
    out.push_back(&x);
  }
  std::ranges::sort(out, {}, [](const T *x) { return x->no; });
  return out;
}

result<value> field_value(const field_desc &f, const type_names &names) {
  struct_builder b;
  b.add(1, value::uint(f.no));
  const auto type = names.name(f.type);
  if (!type) {
    return std::unexpected(type.error());
  }
  if (auto r = b.add(2, str(f.name)); !r) {
    return std::unexpected(r.error());
  }
  if (auto r = b.add(3, str(*type)); !r) {
    return std::unexpected(r.error());
  }
  if (auto r = b.add(4, str(to_string(f.presence))); !r) {
    return std::unexpected(r.error());
  }
  if (f.default_value) {
    b.add(5, *f.default_value);
  }
  b.add(6, value::uint(f.since_minor));
  if (f.deprecated_in) {
    b.add(7, value::uint(*f.deprecated_in));
  }
  if (f.ref) {
    std::vector<std::string> targets;
    for (const type_id &t : f.ref->targets) {
      auto n = names.name(t);
      if (!n) {
        return std::unexpected(n.error());
      }
      targets.push_back(std::move(*n));
    }
    std::ranges::sort(targets);
    std::vector<value> items;
    for (const std::string &t : targets) {
      auto v = str(t);
      if (!v) {
        return v;
      }
      items.push_back(std::move(*v));
    }
    struct_builder ref;
    if (auto r = ref.add(1, str(to_string(f.ref->strength))); !r) {
      return std::unexpected(r.error());
    }
    if (auto r = ref.add(2, value::list(std::move(items))); !r) {
      return std::unexpected(r.error());
    }
    if (auto r = b.add(8, ref.take()); !r) {
      return std::unexpected(r.error());
    }
  }
  if (auto r = b.add(9, str(f.doc)); !r) {
    return std::unexpected(r.error());
  }
  return b.take();
}

// enumerator: {1 no, 2 name, 3 doc, 4 since_minor}; enumerator_desc has no since_minor or
// deprecated_in, so since_minor is 0 and deprecated_in is absent.
result<value> enumerator_value(const enumerator_desc &e) {
  struct_builder b;
  b.add(1, value::uint(e.no));
  if (auto r = b.add(2, str(e.name)); !r) {
    return std::unexpected(r.error());
  }
  if (auto r = b.add(3, str(e.doc)); !r) {
    return std::unexpected(r.error());
  }
  b.add(4, value::uint(0));
  return b.take();
}

// case: {1 no, 2 name, 3 type, 4 doc, 5 since_minor}; as for enumerators, since_minor is 0.
result<value> case_value(const case_desc &c, const type_names &names) {
  struct_builder b;
  b.add(1, value::uint(c.no));
  const auto type = names.name(c.type);
  if (!type) {
    return std::unexpected(type.error());
  }
  if (auto r = b.add(2, str(c.name)); !r) {
    return std::unexpected(r.error());
  }
  if (auto r = b.add(3, str(*type)); !r) {
    return std::unexpected(r.error());
  }
  if (auto r = b.add(4, str(c.doc)); !r) {
    return std::unexpected(r.error());
  }
  b.add(5, value::uint(0));
  return b.take();
}

template <class T, class F> result<value> list_of(const std::vector<T> &items, const F &make) {
  std::vector<value> out;
  out.reserve(items.size());
  for (const T *x : sorted_by_no(items)) {
    auto v = make(*x);
    if (!v) {
      return v;
    }
    out.push_back(std::move(*v));
  }
  return value::list(std::move(out));
}

result<value> type_def_value(const type_desc &t, const schema_registry *schemas) {
  const type_names names(t, schemas);
  struct_builder b;
  if (auto r = b.add(1, str(t.name)); !r) {
    return std::unexpected(r.error());
  }
  if (auto r = b.add(2, str(to_string(t.kind))); !r) {
    return std::unexpected(r.error());
  }
  b.add(3, value::uint(t.version.major));
  b.add(4, value::uint(t.version.minor));
  if (auto r = b.add(5, str(t.domain)); !r) {
    return std::unexpected(r.error());
  }
  if (auto r = b.add(6, str(to_string(t.record_class))); !r) {
    return std::unexpected(r.error());
  }
  if (auto r = b.add(7, str(to_string(t.state_class))); !r) {
    return std::unexpected(r.error());
  }
  b.add(8, value::boolean(t.document_root));
  const auto field = [&names](const field_desc &f) { return field_value(f, names); };
  if (auto r = b.add(9, list_of(t.fields, field)); !r) {
    return std::unexpected(r.error());
  }
  std::vector<std::uint32_t> reserved = t.reserved;
  std::ranges::sort(reserved);
  const auto [dup_begin, dup_end] = std::ranges::unique(reserved);
  reserved.erase(dup_begin, dup_end);
  std::vector<value> reserved_values;
  reserved_values.reserve(reserved.size());
  for (const std::uint32_t r : reserved) {
    reserved_values.push_back(value::uint(r));
  }
  if (auto r = b.add(10, value::list(std::move(reserved_values))); !r) {
    return std::unexpected(r.error());
  }
  if (auto r = b.add(11, list_of(t.enumerators, enumerator_value)); !r) {
    return std::unexpected(r.error());
  }
  const auto kase = [&names](const case_desc &c) { return case_value(c, names); };
  if (auto r = b.add(12, list_of(t.cases, kase)); !r) {
    return std::unexpected(r.error());
  }
  if (t.element) {
    const auto n = names.name(*t.element);
    if (!n) {
      return std::unexpected(n.error());
    }
    if (auto r = b.add(13, str(*n)); !r) {
      return std::unexpected(r.error());
    }
  }
  if (t.key) {
    const auto n = names.name(*t.key);
    if (!n) {
      return std::unexpected(n.error());
    }
    if (auto r = b.add(14, str(*n)); !r) {
      return std::unexpected(r.error());
    }
  }
  return b.take();
}

result<std::vector<std::uint8_t>> type_def_bytes(const type_desc &t,
                                                 const schema_registry *schemas) {
  return type_def_value(t, schemas).and_then([](const value &v) { return encode(v); });
}

result<digest> schema_oid(const type_desc &t, const schema_registry *schemas) {
  return type_def_bytes(t, schemas).transform([](const std::vector<std::uint8_t> &payload) {
    return hash_object(frame_header{.kind = object_kind::schema}, payload);
  });
}

} // namespace

result<std::vector<std::uint8_t>> encode_type_def(const type_desc &t) {
  return type_def_bytes(t, nullptr);
}

result<std::vector<std::uint8_t>> encode_type_def(const type_desc &t,
                                                  const schema_registry &schemas) {
  return type_def_bytes(t, &schemas);
}

result<digest> schema_object_id(const type_desc &t) { return schema_oid(t, nullptr); }

result<digest> schema_object_id(const type_desc &t, const schema_registry &schemas) {
  return schema_oid(t, &schemas);
}

result<digest> schema_set_digest(const schema_registry &schemas) {
  std::vector<digest> ids;
  for (const auto &[id, t] : schemas.types()) {
    if (is_builtin_type(id)) {
      continue;
    }
    auto oid = schema_oid(t, &schemas);
    if (!oid) {
      return std::unexpected(oid.error());
    }
    ids.push_back(*oid);
  }
  std::ranges::sort(ids);
  sha256 h;
  for (const digest &d : ids) {
    const std::array<std::uint8_t, 1> algo = {static_cast<std::uint8_t>(d.algo)};
    h.update(algo);
    h.update(d.bytes);
  }
  return digest{.algo = hash_algo::sha256, .bytes = h.finish()};
}

} // namespace sl::fabric
