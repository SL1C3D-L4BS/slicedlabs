#pragma once

// The SLB/1 codec and canonical text (WO-041, SL-ARCH-001 ARCH1-R22, ARCH1-R26 to ARCH1-R31,
// ADR-0012). The Python reference (tools/fabric/slb_ref.py, slb_text.py) is the specification:
// this codec reproduces its bytes, its text and its rejections (rule and offset) exactly.
//
// encode() writes a value as canonical SLB/1. Values already hold the canonical order
// (value.hpp), so encoding is one size pass and one write pass; a body over 16 MiB is refused
// (`encode_invalid`, R-SER-LIMIT) before the output buffer is allocated.
//
// decode() reads exactly one value that fills the input. Checks run in byte order and the first
// violation wins; every limit is checked before anything is allocated for it. Errors carry the
// rule id and byte offset of the reference ("R-SER-ORDER at offset 7: ..."):
//   R-SER-VARINT, R-SER-ORDER, R-SER-DUP, R-SER-NEGZERO, R-SER-TRAILING  decode_noncanonical
//   R-SER-TRUNC    decode_truncated        R-SER-LIMIT    decode_limit
//   R-SER-UTF8     decode_bad_utf8         R-SER-NAN      decode_bad_float
//   R-SER-TAG      decode_unknown_tag      R-SER-UNKNOWN  decode_unknown_field (strict policy)
// Frames (R-SER-FRAME) are parsed by store.hpp parse_object.
//
// Canonical text (ARCH1-R29) is the schema-directed JSON projection of slb_text.py: compact
// JSON, integers beyond 2^53 - 1 as decimal strings, floats as their shortest round-trip digits,
// bytes as lowercase hex, ids as "<prefix>_<26 Crockford>", digests as "sha256:<hex>", maps as
// [[key, value]...], structs as objects by field name (unknown fields "?<no>": "slb:<hex>"),
// variants as {"<case>": value}, osets as [{"id", "order", "value"}...]. from_text accepts
// exactly the text to_text produces. Text errors: `text_syntax` (R-TXT-SYNTAX) for any other
// spelling, `type_mismatch` (R-TXT-TYPE) for a value that does not fit its type,
// `schema_unknown` for an unregistered type.
//
// Type kinds map onto the text rules as: i8..i64 and u8..u64 are integers of that width, tagged
// is its base type, ref is an id, blob_ref a digest, external_ref its built-in struct.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "sl/core/error.hpp"
#include "sl/fabric/bytes.hpp"
#include "sl/fabric/digest.hpp"
#include "sl/fabric/id.hpp"
#include "sl/fabric/schema.hpp"
#include "sl/fabric/type.hpp"
#include "sl/fabric/value.hpp"

namespace sl::fabric {

/// What decode does with a struct field its schema does not declare (ARCH1-R28): keep it
/// byte-exact (the storage default) or refuse it with R-SER-UNKNOWN.
enum class unknown_policy : std::uint8_t { preserve = 0, strict = 1 };

/// The largest SLB/1 record body (ARCH1-R30): 16 MiB.
inline constexpr std::size_t max_record_body = std::size_t{1} << 24U;

/// Canonical SLB/1 bytes of `v`; `encode_invalid` (R-SER-LIMIT) when they would exceed 16 MiB.
[[nodiscard]] result<std::vector<std::uint8_t>> encode(const value &v);

/// Decodes one canonical value that fills `data`. Without a schema the strict policy has nothing
/// to judge and behaves like preserve.
[[nodiscard]] result<value> decode(byte_span data,
                                   unknown_policy policy = unknown_policy::preserve);

/// Schema-directed decode: under the strict policy a struct field that `type` (or the type of the
/// container holding it) does not declare is R-SER-UNKNOWN. `schema_unknown` when `type` is not
/// registered.
[[nodiscard]] result<value> decode(byte_span data, unknown_policy policy, type_id type,
                                   const schema_registry &schemas);

/// The object id of `payload` as a record frame (kind 2, major 1, minor 0, flags 0).
[[nodiscard]] digest record_object_id(byte_span payload) noexcept;

/// "sha256:<64 hex>" (algo 1) or "blake3:<64 hex>" (algo 2); `type_mismatch` for other algos.
[[nodiscard]] result<std::string> digest_text(const digest &d);

/// Canonical text of `v` as `type`.
[[nodiscard]] result<std::string> to_text(const value &v, type_id type,
                                          const schema_registry &schemas);

/// The value whose canonical text as `type` is exactly `text`.
[[nodiscard]] result<value> from_text(std::string_view text, type_id type,
                                      const schema_registry &schemas);

/// SLB/1 bytes of the SL-ARCH-001 `type_def_value` of `t`, in the encoding of
/// tools/fabric/schemalib.py: names, kinds, classes, presence and strength as strings, type
/// references by qualified name, lists sorted by number, optional parts absent when unset.
/// References resolve to `t` itself and to the built-in types; any other reference is
/// `schema_unknown` (use the overload with a registry).
[[nodiscard]] result<std::vector<std::uint8_t>> encode_type_def(const type_desc &t);
/// As above, resolving references through `schemas` as well.
[[nodiscard]] result<std::vector<std::uint8_t>> encode_type_def(const type_desc &t,
                                                                const schema_registry &schemas);

/// The schema object id (the lock hash): hash_object(kind 9, major 1, minor 0, flags 0,
/// encode_type_def(t)).
[[nodiscard]] result<digest> schema_object_id(const type_desc &t);
[[nodiscard]] result<digest> schema_object_id(const type_desc &t, const schema_registry &schemas);

/// The schema_set digest (ARCH1-R22): SHA-256 over the 33-byte schema object ids (algo byte and
/// 32 bytes) of every registered type that is not built in, in ascending order, so it does not
/// depend on registration order. The registry need not be frozen.
[[nodiscard]] result<digest> schema_set_digest(const schema_registry &schemas);

// ---- JSON documents (the text parser; also reads the corpus files for tests and tools) ----------

struct json_member;

/// A parsed JSON value. Numbers keep the token as written; objects keep their members in
/// document order.
struct json_value {
  enum class kind_type : std::uint8_t { null, boolean, number, string, array, object };

  kind_type kind = kind_type::null;
  bool boolean = false;
  std::string text; // a number token, or a decoded string
  std::vector<json_value> items;
  std::vector<json_member> members;

  /// The member named `key`, or null (also for a value that is not an object).
  [[nodiscard]] const json_value *find(std::string_view key) const noexcept;
};

struct json_member {
  std::string key;
  json_value v;
};

/// Strict RFC 8259 JSON (whitespace allowed, no trailing commas, no NaN, valid UTF-8, at most 512
/// nested arrays and objects); a repeated object key is refused. `text_syntax` otherwise.
[[nodiscard]] result<json_value> parse_json(std::string_view text);

// ---- corpus support (tests/fixtures/fabric/slb; used by sl_fabric_corpus and the codec tests) --

/// A value in the corpus notation of tests/fixtures/fabric/README.md ({"uint": "128"},
/// {"map": [[k, v]...]}, ...); `invalid_argument` for malformed notation, the value constructors'
/// errors otherwise.
[[nodiscard]] result<value> value_from_notation(const json_value &j);

/// The corpus schemas (slb/schemas.json) as a registry: corpus scalars map onto the built-in
/// types (uint bits 64 is u64, "string" is string, ...), every other corpus type is registered as
/// "corpus.<name>". A type the type system cannot express (an int of 65 bits, field number 0) is
/// skipped, and so is every type that reaches it. The registry is not frozen.
struct corpus_schemas {
  schema_registry registry;
  std::map<std::string, type_id> types; // corpus name -> type id, for the expressible types
  std::vector<std::string> skipped;     // corpus names that have no C++ type, sorted
};
[[nodiscard]] result<corpus_schemas> load_corpus_schemas(const json_value &doc);

namespace detail {
/// Strict UTF-8 (no overlong forms, no surrogates, nothing above U+10FFFF), as Python decodes it.
[[nodiscard]] bool utf8_valid(byte_span b) noexcept;
} // namespace detail

/// Lowercase hex of `b`.
[[nodiscard]] std::string to_hex(byte_span b);
/// The bytes of lowercase or uppercase hex text; `invalid_argument` for odd length or a non-hex
/// character.
[[nodiscard]] result<std::vector<std::uint8_t>> from_hex(std::string_view text);

} // namespace sl::fabric
