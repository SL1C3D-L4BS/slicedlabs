// Revision-store objects: record, root, revision, tx and tombstone codecs and their links
// (WO-046, SL-ARCH-001 ARCH1-R05, ARCH1-R50 to ARCH1-R52, ADR-0014).

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/gen/fabric.history.gen.hpp"
#include "sl/fabric/gen/fabric.identity.gen.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/revision.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/tree.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

namespace {

constexpr std::uint32_t r_id = 1;
constexpr std::uint32_t r_type = 2;
constexpr std::uint32_t r_document = 3;
constexpr std::uint32_t r_major = 4;
constexpr std::uint32_t r_minor = 5;
constexpr std::uint32_t r_body = 6;
constexpr std::uint32_t root_records = 1;
constexpr std::uint32_t root_membership = 2;

std::unexpected<error> corrupt(std::string_view what, std::string_view detail = {}) {
  std::string m(what);
  if (!detail.empty()) {
    m += ": ";
    m += detail;
  }
  return make_error(errc::object_corrupt, std::move(m));
}

result<object> frame_value(object_kind kind, const result<value> &v) {
  if (!v) {
    return std::unexpected(v.error());
  }
  auto b = encode(*v);
  if (!b) {
    return std::unexpected(b.error());
  }
  return make_object(kind, *b);
}

result<value> payload_of(const object &o, object_kind kind) {
  if (o.header.kind != kind) {
    return corrupt(std::string(to_string(kind)) + " object expected, got " +
                   std::string(to_string(o.header.kind)));
  }
  auto v = decode(o.payload);
  if (!v) {
    return corrupt(std::string(to_string(kind)) + " payload", v.error().message);
  }
  return v;
}

template <class T, class F> result<T> decode_gen(const object &o, object_kind kind, F &&from) {
  auto v = payload_of(o, kind);
  if (!v) {
    return std::unexpected(v.error());
  }
  auto t = std::forward<F>(from)(*v);
  if (!t) {
    return corrupt(std::string(to_string(kind)) + " layout", t.error().message);
  }
  return t;
}

result<record_id> id_field(const value *v, bool nullable) {
  if (v == nullptr || v->kind() != value_kind::id) {
    return corrupt("record object: id field malformed");
  }
  if (nullable && v->as_id_bytes() == id_bytes{}) {
    return record_id{};
  }
  auto id = record_id::from_bytes(v->as_id_bytes());
  if (!id) {
    return corrupt("record object: invalid id", id.error().message);
  }
  return id;
}

result<std::uint32_t> u32_field(const value *v) {
  if (v == nullptr || v->kind() != value_kind::uint ||
      v->as_uint() > std::numeric_limits<std::uint32_t>::max()) {
    return corrupt("record object: version field malformed");
  }
  return static_cast<std::uint32_t>(v->as_uint());
}

void push_link(std::vector<digest> &out, const digest &d) {
  if (d != empty_tree()) {
    out.push_back(d);
  }
}

} // namespace

result<object> encode_record_object(const record_object &r) {
  if (r.id.is_null() || r.type.is_null()) {
    return make_error(errc::invalid_argument, "record object needs an id and a type");
  }
  std::vector<field> fs;
  fs.push_back(field{.no = r_id, .v = value::id(r.id)});
  fs.push_back(field{.no = r_type, .v = value::id(r.type.id())});
  if (!r.document.is_null()) {
    fs.push_back(field{.no = r_document, .v = value::id(r.document)});
  }
  fs.push_back(field{.no = r_major, .v = value::uint(r.version.major)});
  fs.push_back(field{.no = r_minor, .v = value::uint(r.version.minor)});
  fs.push_back(field{.no = r_body, .v = r.body});
  return frame_value(object_kind::record, value::struct_(std::move(fs)));
}

result<record_object> decode_record_object(const object &o) {
  auto v = payload_of(o, object_kind::record);
  if (!v) {
    return std::unexpected(v.error());
  }
  if (v->kind() != value_kind::struct_) {
    return corrupt("record object: not a struct");
  }
  const value *doc = v->find_field(r_document);
  const value *body = v->find_field(r_body);
  if (body == nullptr || v->fields().size() != (doc != nullptr ? 6U : 5U)) {
    return corrupt("record object: fields are not {1 id, 2 type, 3 document, 4 major, 5 minor, "
                   "6 body}");
  }
  record_object r;
  auto id = id_field(v->find_field(r_id), false);
  auto type = id_field(v->find_field(r_type), false);
  auto major = u32_field(v->find_field(r_major));
  auto minor = u32_field(v->find_field(r_minor));
  for (const result<record_id> *part : {&id, &type}) {
    if (!*part) {
      return std::unexpected(part->error());
    }
  }
  for (const result<std::uint32_t> *part : {&major, &minor}) {
    if (!*part) {
      return std::unexpected(part->error());
    }
  }
  auto typed = type_id::from(*type);
  if (!typed) {
    return corrupt("record object: type is not a type id");
  }
  if (doc != nullptr) {
    auto d = id_field(doc, false);
    if (!d) {
      return std::unexpected(d.error());
    }
    r.document = *d;
  }
  r.id = *id;
  r.type = *typed;
  r.version = type_version{.major = *major, .minor = *minor};
  r.body = *body;
  return r;
}

result<object> encode_root_object(const root_object &r) {
  std::vector<field> fs;
  fs.push_back(field{.no = root_records, .v = value::digest(r.records)});
  fs.push_back(field{.no = root_membership, .v = value::digest(r.membership)});
  return frame_value(object_kind::root, value::struct_(std::move(fs)));
}

result<root_object> decode_root_object(const object &o) {
  auto v = payload_of(o, object_kind::root);
  if (!v) {
    return std::unexpected(v.error());
  }
  const value *rec = v->kind() == value_kind::struct_ ? v->find_field(root_records) : nullptr;
  const value *mem = v->kind() == value_kind::struct_ ? v->find_field(root_membership) : nullptr;
  if (rec == nullptr || mem == nullptr || v->fields().size() != 2 ||
      rec->kind() != value_kind::digest || mem->kind() != value_kind::digest) {
    return corrupt("root object: fields are not {1 records digest, 2 membership digest}");
  }
  return root_object{.records = rec->as_digest(), .membership = mem->as_digest()};
}

result<object> encode_revision(const gen::fabric_history::revision &r) {
  return frame_value(object_kind::revision, r.to_value());
}

result<gen::fabric_history::revision> decode_revision(const object &o) {
  return decode_gen<gen::fabric_history::revision>(o, object_kind::revision,
                                                   gen::fabric_history::revision_from_value);
}

result<object> encode_tx_record(const gen::fabric_history::tx_record &t) {
  return frame_value(object_kind::tx, t.to_value());
}

result<gen::fabric_history::tx_record> decode_tx_record(const object &o) {
  return decode_gen<gen::fabric_history::tx_record>(o, object_kind::tx,
                                                    gen::fabric_history::tx_record_from_value);
}

result<object> encode_tombstone(const gen::fabric_identity::tombstone &t) {
  return frame_value(object_kind::tombstone, t.to_value());
}

result<gen::fabric_identity::tombstone> decode_tombstone(const object &o) {
  return decode_gen<gen::fabric_identity::tombstone>(o, object_kind::tombstone,
                                                     gen::fabric_identity::tombstone_from_value);
}

result<void> revision_links::append_links(const object &obj,
                                          std::vector<digest> &out) const noexcept {
  switch (obj.header.kind) {
  case object_kind::revision: {
    auto r = decode_revision(obj);
    if (!r) {
      return std::unexpected(r.error());
    }
    for (const digest &p : r->parents) {
      out.push_back(p);
    }
    push_link(out, r->root);
    out.push_back(r->tx);
    return {};
  }
  case object_kind::root: {
    auto r = decode_root_object(obj);
    if (!r) {
      return std::unexpected(r.error());
    }
    push_link(out, r->records);
    push_link(out, r->membership);
    return {};
  }
  case object_kind::tree: {
    auto n = decode_tree_node(obj);
    if (!n) {
      return std::unexpected(n.error());
    }
    for (const digest &c : n->children) {
      out.push_back(c);
    }
    for (const tree_entry &e : n->entries) {
      if (e.kind != entry_kind::member) {
        push_link(out, e.target);
      }
    }
    return {};
  }
  case object_kind::tombstone: {
    auto t = decode_tombstone(obj);
    if (!t) {
      return std::unexpected(t.error());
    }
    out.push_back(t->last_record);
    return {};
  }
  case object_kind::tx: {
    auto t = decode_tx_record(obj);
    if (!t) {
      return std::unexpected(t.error());
    }
    out.push_back(t->patch);
    return {};
  }
  case object_kind::blob:
  case object_kind::record:
  case object_kind::patch:
  case object_kind::schema:
  case object_kind::schema_set:
    return {};
  }
  return {};
}

} // namespace sl::fabric
