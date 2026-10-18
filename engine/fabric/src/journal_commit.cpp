// Commit and intent payloads and the commit chain of the log (WO-053, SL-ARCH-002 `journal`
// commit_fields and frame_types 3-4, ARCH2-R30, ADR-0015).

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/codec.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/journal.hpp"
#include "sl/fabric/store.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

namespace {

std::unexpected<error> corrupt(std::string_view what, std::string_view detail) {
  std::string m(what);
  m += ": ";
  m += detail;
  return make_error(errc::journal_corrupt, std::move(m));
}

std::string field_text(std::uint32_t no, std::string_view name) {
  return "field " + std::to_string(no) + " (" + std::string(name) + ")";
}

// Field lookups that judge the kind; `what` names the payload in errors.
struct reader {
  std::string_view what;
  const value &v; // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): a scoped view

  [[nodiscard]] result<const value *> get(std::uint32_t no, std::string_view name, value_kind kind,
                                          bool required) const {
    const value *f = v.find_field(no);
    if (f == nullptr) {
      if (required) {
        return corrupt(what, field_text(no, name) + " is missing");
      }
      return static_cast<const value *>(nullptr);
    }
    if (f->kind() != kind) {
      return corrupt(what, field_text(no, name) + " is " + std::string(to_string(f->kind())) +
                               ", expected " + std::string(to_string(kind)));
    }
    return f;
  }

  [[nodiscard]] result<std::uint64_t> uint(std::uint32_t no, std::string_view name) const {
    return get(no, name, value_kind::uint, true).transform([](const value *f) {
      return f->as_uint();
    });
  }

  [[nodiscard]] result<digest> dig(std::uint32_t no, std::string_view name) const {
    return get(no, name, value_kind::digest, true).transform([](const value *f) {
      return f->as_digest();
    });
  }
};

result<record_id> id_of(const value &v, std::string_view what, std::string_view name) {
  if (v.as_id_bytes() == id_bytes{}) {
    return record_id{};
  }
  result<record_id> id = v.as_record_id();
  if (!id) {
    return corrupt(what, std::string(name) + ": " + id.error().message);
  }
  return id;
}

result<value> ref_value(const ref_update &r) {
  std::vector<field> f;
  result<value> name = value::string(r.name);
  if (!name) {
    return std::unexpected(std::move(name.error()));
  }
  f.push_back(field{.no = 1, .v = std::move(*name)});
  if (r.expected) {
    f.push_back(field{.no = 2, .v = value::digest(*r.expected)});
  }
  f.push_back(field{.no = 3, .v = value::digest(r.desired)});
  return value::struct_(std::move(f));
}

result<value> decoded_struct(byte_span payload, std::string_view what) {
  result<value> v = decode(payload);
  if (!v) {
    return corrupt(what, "payload does not decode: " + v.error().message);
  }
  if (v->kind() != value_kind::struct_) {
    return corrupt(what, "payload is " + std::string(to_string(v->kind())) + ", expected struct");
  }
  return v;
}

} // namespace

result<std::vector<std::uint8_t>> encode_commit(const commit_record &c) {
  std::vector<field> f;
  f.push_back(field{.no = 1, .v = value::uint(c.seq)});
  if (c.prev) {
    f.push_back(field{.no = 2, .v = value::digest(*c.prev)});
  }
  f.push_back(field{.no = 3, .v = value::digest(c.tx_record)});
  f.push_back(field{.no = 4, .v = value::digest(c.revision)});
  std::vector<value> refs;
  refs.reserve(c.refs.size());
  for (const ref_update &r : c.refs) {
    result<value> rv = ref_value(r);
    if (!rv) {
      return std::unexpected(std::move(rv.error()));
    }
    refs.push_back(std::move(*rv));
  }
  result<value> ref_list = value::list(std::move(refs));
  if (!ref_list) {
    return std::unexpected(std::move(ref_list.error()));
  }
  f.push_back(field{.no = 5, .v = std::move(*ref_list)});
  f.push_back(field{.no = 6, .v = value::uint(c.object_count)});
  f.push_back(field{.no = 7, .v = value::digest(c.objects_digest)});
  std::vector<value> loose;
  loose.reserve(c.loose.size());
  for (const digest &d : c.loose) {
    loose.push_back(value::digest(d));
  }
  result<value> loose_list = value::list(std::move(loose));
  if (!loose_list) {
    return std::unexpected(std::move(loose_list.error()));
  }
  f.push_back(field{.no = 8, .v = std::move(*loose_list)});
  f.push_back(field{.no = 9, .v = value::integer(c.commit_time_us)});
  f.push_back(field{.no = 10, .v = value::id(c.actor)});
  result<value> s = value::struct_(std::move(f));
  if (!s) {
    return std::unexpected(std::move(s.error()));
  }
  return encode(*s);
}

result<commit_record> decode_commit(byte_span payload) {
  constexpr std::string_view what = "commit payload";
  const result<value> v = decoded_struct(payload, what);
  if (!v) {
    return std::unexpected(v.error());
  }
  const reader r{.what = what, .v = *v};
  commit_record c;
  const result<std::uint64_t> seq = r.uint(1, "seq");
  if (!seq) {
    return std::unexpected(seq.error());
  }
  c.seq = *seq;
  const result<const value *> prev = r.get(2, "prev", value_kind::digest, false);
  if (!prev) {
    return std::unexpected(prev.error());
  }
  if (*prev != nullptr) {
    c.prev = (*prev)->as_digest();
  }
  const result<digest> tx = r.dig(3, "tx record");
  const result<digest> rev = r.dig(4, "revision");
  const result<const value *> refs = r.get(5, "ref_updates", value_kind::list, true);
  const result<std::uint64_t> count = r.uint(6, "object_count");
  const result<digest> objects = r.dig(7, "objects_digest");
  const result<const value *> loose = r.get(8, "loose blobs", value_kind::list, true);
  const result<const value *> actor = r.get(10, "actor", value_kind::id, true);
  for (const result<digest> *d : {&tx, &rev, &objects}) {
    if (!*d) {
      return std::unexpected(d->error());
    }
  }
  for (const result<const value *> *p : {&refs, &loose, &actor}) {
    if (!*p) {
      return std::unexpected(p->error());
    }
  }
  if (!count) {
    return std::unexpected(count.error());
  }
  c.tx_record = *tx;
  c.revision = *rev;
  c.object_count = *count;
  c.objects_digest = *objects;
  for (const value &item : (*refs)->items()) {
    if (item.kind() != value_kind::struct_) {
      return corrupt(what, "a ref update is not a struct");
    }
    const reader rr{.what = "commit ref update", .v = item};
    const result<const value *> name = rr.get(1, "name", value_kind::string, true);
    if (!name) {
      return std::unexpected(name.error());
    }
    const result<const value *> expected = rr.get(2, "expected", value_kind::digest, false);
    if (!expected) {
      return std::unexpected(expected.error());
    }
    const result<digest> desired = rr.dig(3, "new");
    if (!desired) {
      return std::unexpected(desired.error());
    }
    ref_update u{
        .name = std::string((*name)->as_string()), .expected = std::nullopt, .desired = *desired};
    if (*expected != nullptr) {
      u.expected = (*expected)->as_digest();
    }
    c.refs.push_back(std::move(u));
  }
  for (const value &item : (*loose)->items()) {
    if (item.kind() != value_kind::digest) {
      return corrupt(what, "a loose blob entry is not a digest");
    }
    c.loose.push_back(item.as_digest());
  }
  const value *time = v->find_field(9);
  if (time == nullptr) {
    return corrupt(what, field_text(9, "commit_time_us") + " is missing");
  }
  const std::optional<std::int64_t> us =
      time->kind() == value_kind::uint || time->kind() == value_kind::nint ? time->as_i64()
                                                                           : std::nullopt;
  if (!us) {
    return corrupt(what, field_text(9, "commit_time_us") + " is not an i64");
  }
  c.commit_time_us = *us;
  const result<record_id> who = id_of(**actor, what, "actor");
  if (!who) {
    return std::unexpected(who.error());
  }
  c.actor = *who;
  return c;
}

result<std::vector<std::uint8_t>> encode_intent(const intent_record &in) {
  std::vector<field> f;
  f.push_back(field{.no = 1, .v = value::id(in.operation)});
  if (in.phase == intent_phase::begin) {
    result<value> kind = value::string(in.kind);
    if (!kind) {
      return std::unexpected(std::move(kind.error()));
    }
    f.push_back(field{.no = 2, .v = std::move(*kind)});
  } else {
    f.push_back(field{.no = 2, .v = value::uint(static_cast<std::uint64_t>(in.outcome))});
  }
  result<value> s = value::struct_(std::move(f));
  if (!s) {
    return std::unexpected(std::move(s.error()));
  }
  return encode(*s);
}

result<intent_record> decode_intent(frame_type type, byte_span payload) {
  if (type != frame_type::intent_begin && type != frame_type::intent_end) {
    return make_error(errc::invalid_argument, "decode_intent: not an intent frame type");
  }
  const bool begin = type == frame_type::intent_begin;
  const std::string_view what = begin ? "intent_begin payload" : "intent_end payload";
  const result<value> v = decoded_struct(payload, what);
  if (!v) {
    return std::unexpected(v.error());
  }
  const reader r{.what = what, .v = *v};
  const result<const value *> op = r.get(1, "operation", value_kind::id, true);
  if (!op) {
    return std::unexpected(op.error());
  }
  const result<record_id> operation = id_of(**op, what, "operation");
  if (!operation) {
    return std::unexpected(operation.error());
  }
  if (operation->is_null()) {
    return corrupt(what, "the operation id is null");
  }
  intent_record in{.operation = *operation,
                   .kind = {},
                   .phase = begin ? intent_phase::begin : intent_phase::end,
                   .outcome = intent_outcome::none};
  if (begin) {
    const result<const value *> kind = r.get(2, "kind", value_kind::string, true);
    if (!kind) {
      return std::unexpected(kind.error());
    }
    in.kind = std::string((*kind)->as_string());
    if (in.kind.empty()) {
      return corrupt(what, "the kind is empty");
    }
    return in;
  }
  const result<std::uint64_t> outcome = r.uint(2, "outcome");
  if (!outcome) {
    return std::unexpected(outcome.error());
  }
  if (*outcome < 1 || *outcome > 3) {
    return corrupt(what, "outcome " + std::to_string(*outcome) + " is not 1, 2 or 3");
  }
  in.outcome = static_cast<intent_outcome>(*outcome);
  return in;
}

digest commit_digest(byte_span payload) noexcept {
  return digest{.algo = hash_algo::sha256, .bytes = sha256::hash(payload)};
}

void objects_hasher::add(const digest &id) noexcept {
  const std::array<std::uint8_t, 1> algo{static_cast<std::uint8_t>(id.algo)};
  hash_.update(algo);
  hash_.update(id.bytes);
  ++count_;
}

digest objects_hasher::finish() const noexcept {
  return digest{.algo = hash_algo::sha256, .bytes = hash_.finish()};
}

void objects_hasher::reset() noexcept {
  hash_.reset();
  count_ = 0;
}

result<void> check_commit(const commit_record &c, const chain_state &chain,
                          const objects_hasher &objects) noexcept {
  constexpr std::string_view what = "commit (ARCH2-R30)";
  if (c.seq != chain.seq + 1) {
    return corrupt(what,
                   "seq " + std::to_string(c.seq) + ", expected " + std::to_string(chain.seq + 1));
  }
  if (c.prev != chain.last) {
    const std::string found = c.prev ? c.prev->to_hex() : std::string("absent");
    const std::string want = chain.last ? chain.last->to_hex() : std::string("absent");
    return corrupt(what, "seq " + std::to_string(c.seq) + ": prev " + found + ", expected " + want);
  }
  if (c.object_count != objects.count()) {
    return corrupt(what, "seq " + std::to_string(c.seq) + ": object_count " +
                             std::to_string(c.object_count) + ", found " +
                             std::to_string(objects.count()) + " object frames");
  }
  if (c.objects_digest != objects.finish()) {
    return corrupt(what, "seq " + std::to_string(c.seq) + ": objects_digest " +
                             c.objects_digest.to_hex() + " does not match the object frames (" +
                             objects.finish().to_hex() + ")");
  }
  return {};
}

} // namespace sl::fabric
