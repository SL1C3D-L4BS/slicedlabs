---
id: SL-ARCH-001
kind: ARCH
title: "Canonical Project Model (Fabric)"
status: Draft
version: 0.1.0
owner: founder
episodes: [61, 62, 63, 64, 65, 66, 67, 68, 69, 72, 73, 74, 75, 78, 79, 80]
depends_on: [SL-GOV-001, SL-GOV-006]
supersedes: []
decisions: [ADR-0009, ADR-0010, ADR-0011, ADR-0012, ADR-0013, ADR-0014, ADR-0017, ADR-0019, ADR-0020]
max_lines: 440
---
# SL-ARCH-001: Canonical Project Model (Fabric)

## Purpose

The Fabric is the one canonical project model: every asset, entity, system, dependency, build configuration, test
result and runtime observation has one authoritative identity and a traceable history (master plan sections 1.1 and
1.2, principle 1; section 2.2, row 02). This document controls the data side of the Fabric: identity, object ids,
types and values, schemas and their versions, the SLB/1 encoding, the project manifest, ownership and authority,
provenance, event envelopes, the revision-store data model, the dependency graph, the runtime/document boundary, the
reference resolver and schema-generated tooling (Volume 04 episodes 061-069, 072-075, 078-080). How facts change
(transactions, undo, journal, recovery, corruption handling) is SL-ARCH-002; a rule lives in exactly one of the two.

## Status and decisions

Draft 0.1.0, written before any Volume 04 code (master plan, Authoring order: SL-ARCH-001, then SL-ARCH-002). Binding
decisions: ADR-0009 (errors are values), ADR-0010 (error domain), ADR-0011 (identity), ADR-0012 (SLB/1), ADR-0013
(schema source and lock), ADR-0014 (revision store, single CAS), ADR-0017 (ownership), ADR-0019 (collaboration and
replay seams), ADR-0020 (acceptance state). The founder approves through `registry/SL-REG-001.approvals.json`; builders
never change the status. The conformance work order (WO-062) reconciles every `sl-data` table with the code; the
release work order (WO-063) moves the document to InReview. Rule ids are ARCH1-Rnn; invariants are FAB-INV-<area>-nn.

## Normative text

### Identity (episodes 061, 065)

- ARCH1-R01: Every authoritative record MUST have a 16-byte record id laid out as class byte, scheme byte and 112-bit payload, with the text form and classes of the `id_classes` table below (ADR-0011).
- ARCH1-R02: A random id MUST be minted at authoring time through an injected `id_source`, and `mint()` MUST return `sl::result<record_id>` so an entropy failure is a value (ADR-0011; ADR-0009).
- ARCH1-R03: Derived, well-known and imported ids MUST be pure functions of their inputs as defined in `mint_schemes` (ADR-0011).
- ARCH1-R04: An id MUST NOT encode time, host, user or a counter, MUST NOT change class, and MUST NOT be reused (ADR-0011; master plan section 4.2).
- ARCH1-R05: A retired id of any class MUST stay as a tombstone leaf that keeps the last record object id, and MAY be revived only by the undo of the transaction that retired it (ADR-0011; ADR-0018).
- ARCH1-R06: The four identity families MUST be kept apart: asset (reusable authored or imported unit, stable across rename, move, edit and migration), entity (object defined inside a document), instance (a use of an asset, prefab or entity in a context, with a derived id), derived (generated output whose id derives from its derivation key) (master plan, Volume 04, episode 065).
- ARCH1-R07: Instance ids MUST derive from (parent instance or document id, source entity id), and derived-resource ids from the derivation key of ARCH1-R44, so re-instantiation, re-import and re-cook keep ids (ADR-0011).
- ARCH1-R08: An ECS runtime handle MUST NOT be an identity or appear in any schema; Volume 05 maps ids to handles (master plan section 2.2, row 03; episode 074).

```sl-data
{"id_classes": [
{"class": 1, "name": "project", "prefix": "prj"}, {"class": 2, "name": "schema", "prefix": "sch"}, {"class": 3, "name": "type", "prefix": "typ"},
{"class": 4, "name": "asset", "prefix": "ast"}, {"class": 5, "name": "entity", "prefix": "ent"}, {"class": 6, "name": "instance", "prefix": "ins"},
{"class": 7, "name": "derived", "prefix": "drv"}, {"class": 8, "name": "tx", "prefix": "txn"}, {"class": 9, "name": "event", "prefix": "evt"},
{"class": 10, "name": "actor", "prefix": "act"}, {"class": 11, "name": "package", "prefix": "pkg"}, {"class": 12, "name": "element", "prefix": "elm"},
{"class": 13, "name": "namespace", "prefix": "nsp"}, {"class": 14, "name": "operation", "prefix": "ope"}, {"class": 15, "name": "import", "prefix": "imp"}],
"reserved_classes": "0 none; 16-255 reserved",
"text_form": "<prefix>_<26 Crockford base32 of the 16 bytes, first char 0-7>",
"mint_schemes": [
{"scheme": 1, "name": "random", "payload": "14 bytes from id_source"},
{"scheme": 2, "name": "derived", "payload": "SHA-256('slf-derive/1' || class u8 || parent 16 bytes || u32le len(key) || key)[0:14]"},
{"scheme": 3, "name": "well_known", "payload": "bytes 2-11 zero, bytes 12-15 ordinal u32 big-endian"},
{"scheme": 4, "name": "imported", "payload": "SHA-256('slf-import/1' || class u8 || u32le len(system) || system || u32le len(ext) || ext)[0:14]"}],
"well_known": [{"class": "namespace", "ordinal": 1, "name": "project root namespace"}, {"class": "entity", "ordinal": 1, "name": "project manifest record"}],
"deterministic_source": "block i = SHA-256('slf-drbg/1' || seed u64le || u32le len(stream) || stream || i u64le)"}
```

### Content identity and stored objects (episode 068)

- ARCH1-R09: An object id MUST be `{algo u8, 32 bytes}` with algo 1 = SHA-256 over `"slf-obj/1" 0x00`, the frame header without the magic, and the payload, so every header byte is covered (ADR-0011).
- ARCH1-R10: Every stored object MUST be framed as magic `SLFO`, major u8, minor u8, kind u8, flags u8, varint payload length, payload; a newer major is refused (`format_unsupported`) and a newer minor opens read-only (`read_only_newer`) (ADR-0012).
- ARCH1-R11: Reading an object MUST verify its id; a mismatch is `object_corrupt` and never returns bytes (master plan, Volume 04, episode 068).

```sl-data
{"object_frame": {"magic": "SLFO", "major": 1, "minor": 0, "flags": "bit0 compressed (reserved, must be 0 in 1.0); others 0", "length": "LEB128 payload length"},
"object_kinds": [{"kind": 1, "name": "blob"}, {"kind": 2, "name": "record"}, {"kind": 3, "name": "tombstone"}, {"kind": 4, "name": "tree"}, {"kind": 5, "name": "root"},
{"kind": 6, "name": "revision"}, {"kind": 7, "name": "tx"}, {"kind": 8, "name": "patch"}, {"kind": 9, "name": "schema"}, {"kind": 10, "name": "schema_set"}],
"hash_algos": [{"algo": 1, "name": "sha256"}, {"algo": 2, "name": "blake3", "status": "reserved, needs ADR"}]}
```

### Types and values (episode 062)

- ARCH1-R12: Every record body MUST conform to a registered type of a kind listed in `type_kinds`; type ids are derived ids of class type from (project root namespace, qualified type name) (ADR-0013).
- ARCH1-R13: Struct fields MUST be addressed by stable u32 field numbers; names are labels; a removed number goes to the type's reserved list and is never reused (ADR-0013).
- ARCH1-R14: Field presence MUST be required or optional; an optional field MAY declare a default that readers apply when the field is absent, and writers MUST NOT elide or materialize it (ADR-0012).
- ARCH1-R15: Identity-bearing collections MUST be `oset` (element id plus fractional order key); a `list` is atomic and replaced whole (ADR-0019).
- ARCH1-R16: Values MUST be immutable with structural sharing: `with` and `without` return a new value and leave the original bit-identical (master plan section 2.4).
- ARCH1-R17: Every record type MUST declare a domain, a record class equal to the class of its ids, a state class in {document, derived, observation} and whether it is a document root (ADR-0017).
- ARCH1-R18: A `ref` MUST declare its target types and a strength: weak (no dependency edge, may dangle), strong (edge, must be live at commit) or owned (containment edge, cascades on delete) (master plan, Volume 04, episode 072).

```sl-data
{"type_kinds": [
{"kind": "bool", "slb": "0x01/0x02"}, {"kind": "i8|i16|i32|i64", "slb": "uint or nint, range by width"}, {"kind": "u8|u16|u32|u64", "slb": "uint, range by width"},
{"kind": "f32", "slb": "0x05"}, {"kind": "f64", "slb": "0x06"}, {"kind": "string", "slb": "0x07, UTF-8, no NUL"}, {"kind": "bytes", "slb": "0x08"},
{"kind": "id", "slb": "0x09"}, {"kind": "digest", "slb": "0x0A"}, {"kind": "enum", "slb": "uint enumerator number"},
{"kind": "struct", "slb": "0x0D"}, {"kind": "list", "slb": "0x0B, atomic"}, {"kind": "map", "slb": "0x0C, keys string|integer|id"},
{"kind": "oset", "slb": "0x0F, entries {element id, order key bytes, value}"}, {"kind": "variant", "slb": "0x0E, stable case numbers"},
{"kind": "tagged", "slb": "encoding of its base type; named newtype such as meters:f64"}, {"kind": "ref", "slb": "0x09 plus targets and strength in the type"},
{"kind": "blob_ref", "slb": "0x0A naming a blob object"}, {"kind": "external_ref", "slb": "struct {1 system string, 2 locator string, 3 content digest optional}"}],
"state_classes": ["document", "derived", "observation"],
"forbidden": ["handle field types", "transient state class", "default materialization", "positional element addressing"]}
```

### Schemas and versioning (episode 063)

- ARCH1-R19: Schema source MUST be `schemas/fabric/*.sltype.json` and nothing else defines a Fabric type (ADR-0013).
- ARCH1-R20: A released (type, version) MUST be immutable and hash-locked in `schemas/fabric/schema.lock.json` (ADR-0013).
- ARCH1-R21: Every schema change MUST be classified by the `compatibility` table below and carry its bump: identical none, additive minor, backward_only major plus an identity migration step, breaking major plus a migration step; forbidden changes are refused (ADR-0013; master plan, Volume 04, episode 063).
- ARCH1-R22: Every revision MUST record its schema_set digest, and schema objects MUST be stored in the object store so old revisions stay readable (ADR-0014).
- ARCH1-R23: A record written by a newer minor of its type MUST be preserved byte-identically and refused for write by an older reader (ADR-0012).
- ARCH1-R24: Deprecation MUST follow since, deprecated_in and removed_in; a deprecated field still decodes, writing it is a warning diagnostic, removal needs a major bump (master plan, Volume 04, episode 063).
- ARCH1-R25: A migration MUST be a pure deterministic function over values that runs as one transaction by the system actor with provenance calculated; a failure leaves the head unchanged and names the path (master plan, Volume 04, episode 063 "failure semantics"; ADR-0018).

```sl-data
{"compatibility": [
{"change": "rename field label or edit doc", "class": "identical"}, {"change": "deprecate field", "class": "identical"},
{"change": "add optional field", "class": "additive"}, {"change": "add enum value", "class": "additive"}, {"change": "add variant case", "class": "additive"},
{"change": "add ref target type", "class": "additive"}, {"change": "ref strength strong to weak", "class": "additive"},
{"change": "remove optional field (number reserved)", "class": "backward_only"}, {"change": "required to optional", "class": "backward_only"},
{"change": "widen integer range", "class": "backward_only"},
{"change": "add required field", "class": "breaking"}, {"change": "remove required field", "class": "breaking"}, {"change": "change field type", "class": "breaking"},
{"change": "narrow integer range", "class": "breaking"}, {"change": "remove or renumber enum value", "class": "breaking"}, {"change": "remove variant case", "class": "breaking"},
{"change": "change default of optional field", "class": "breaking"}, {"change": "optional to required", "class": "breaking"},
{"change": "ref strength weak to strong", "class": "breaking"}, {"change": "remove ref target type", "class": "breaking"}, {"change": "list to oset or oset to list", "class": "breaking"},
{"change": "change domain, state class or record class", "class": "forbidden"}, {"change": "reuse a reserved field number", "class": "forbidden"}],
"bump": {"identical": "none", "additive": "minor", "backward_only": "major + identity migration step", "breaking": "major + migration step", "forbidden": "refused"},
"type_def_value": "struct {1 name, 2 kind, 3 major, 4 minor, 5 domain, 6 record_class, 7 state_class, 8 document_root, 9 fields list of struct {1 no, 2 name, 3 type, 4 presence, 5 default optional, 6 since_minor, 7 deprecated_in optional, 8 ref optional struct {1 strength, 2 targets}, 9 doc}, 10 reserved, 11 enumerators, 12 cases, 13 element optional, 14 key optional}",
"lock_hash": "object id of the schema object: hash_object(kind 9, major 1, minor 0, flags 0, SLB/1 bytes of type_def_value); computed by tools/fabric/schemalib.py and by the C++ codec"}
```

### Serialization (episode 068)

- ARCH1-R26: Every persisted value MUST be encoded as SLB/1 with the tags of `slb_tags`, canonically, and the decoder MUST reject every non-canonical form with the rule id and byte offset of `decode_rules` (ADR-0012).
- ARCH1-R27: Floats MUST be finite, and -0.0 MUST be encoded as +0.0 (ADR-0012).
- ARCH1-R28: Unknown struct fields MUST be preserved byte-exact under `unknown_policy::preserve` (the storage default) and rejected under `strict` (ADR-0012).
- ARCH1-R29: Canonical text MUST be the schema-directed JSON projection of ADR-0012, bijective with SLB/1, and never a second source of truth (ADR-0012; master plan section 1.3, zero 2).
- ARCH1-R30: Limits MUST be enforced before allocation: depth 64, 2^24 items per container, record body 16 MiB (ADR-0012).
- ARCH1-R31: The Python reference and the C++ codec MUST agree byte for byte on the committed corpus, on clang and on gcc (ADR-0012).

```sl-data
{"slb_tags": [{"tag": "0x01", "name": "false"}, {"tag": "0x02", "name": "true"}, {"tag": "0x03", "name": "uint"}, {"tag": "0x04", "name": "nint"},
{"tag": "0x05", "name": "f32"}, {"tag": "0x06", "name": "f64"}, {"tag": "0x07", "name": "string"}, {"tag": "0x08", "name": "bytes"}, {"tag": "0x09", "name": "id"},
{"tag": "0x0A", "name": "digest"}, {"tag": "0x0B", "name": "list"}, {"tag": "0x0C", "name": "map"}, {"tag": "0x0D", "name": "struct"}, {"tag": "0x0E", "name": "variant"}, {"tag": "0x0F", "name": "oset"}],
"limits": {"depth": 64, "items": 16777216, "record_body_bytes": 16777216, "loose_blob_threshold_bytes": 1048576},
"decode_rules": ["R-SER-TAG", "R-SER-VARINT", "R-SER-ORDER", "R-SER-DUP", "R-SER-UTF8", "R-SER-NAN", "R-SER-NEGZERO", "R-SER-TRUNC", "R-SER-TRAILING", "R-SER-LIMIT", "R-SER-FRAME"]}
```

### Project manifest (episode 064)

- ARCH1-R32: A project MUST have exactly one manifest record at the well-known id (entity, ordinal 1) in domain `fabric.manifest`, holding project id, name, writer and minimum-reader format versions, schema_set digest, required modules with version ranges, capabilities (SL-REG-004 ids), target profiles (SL-GOV-007), package pins and default branch (master plan, Volume 04, episode 064).
- ARCH1-R33: The manifest MUST change only by transaction (master plan section 3, F5).
- ARCH1-R34: Open MUST refuse, with a typed error and nothing opened, a project whose minimum reader is newer than the engine or whose required module, capability or target profile is unmet (master plan, Volume 04, episode 064).
- ARCH1-R35: `project.slp` MUST hold only {format, store path, project id} and no project fact (master plan section 1.3, zero 2).

### Ownership and authority (episode 066, F2)

- ARCH1-R36: Every domain MUST have exactly one owner subsystem, under the reserved prefixes of `domain_registry`; a second claim is `ownership_conflict` naming both claimants (ADR-0017; master plan section 3, F2).
- ARCH1-R37: Every type MUST belong to one domain; a type without one is `ownership_orphan` (ADR-0017).
- ARCH1-R38: The domain and schema registries MUST freeze when the first workspace opens (ADR-0017).
- ARCH1-R39: Authority MUST be checked at the innermost typed value whose domain differs from its parent's (ADR-0017).
- ARCH1-R40: Agent and service actors MUST be denied by default (ADR-0017).
- ARCH1-R41: Every authoritative record MUST have an owner, a stable id and revisions; records violating any of the three over all records MUST be 0, measured by the conformance suite as the release gate 1 measure (master plan, Release gates, gate 1).

```sl-data
{"domain_registry": [
{"prefix": "shell", "owner": "01", "owns": "workspace layouts, command registry, selection", "state_classes": ["document"]},
{"prefix": "fabric", "owner": "02", "owns": "ids, schemas, revisions, transactions, provenance, manifest, tombstones, import records", "state_classes": ["document", "derived"]},
{"prefix": "runtime", "owner": "03", "owns": "captured runtime observations only; live state is transient and never a record", "state_classes": ["observation"]},
{"prefix": "gfx", "owner": "04", "owns": "GPU resource and pipeline records (derived)", "state_classes": ["derived"]},
{"prefix": "content", "owner": "05", "owns": "editable source documents, including game UI (content.ui)", "state_classes": ["document"]},
{"prefix": "sim", "owner": "06", "owns": "gameplay config, nav settings, net topology", "state_classes": ["document", "derived"]},
{"prefix": "code", "owner": "07", "owns": "source files, graph IR", "state_classes": ["document", "derived"]},
{"prefix": "asset", "owner": "08", "owns": "build DAG, cooked artifact records; bytes live in the Fabric object store (ADR-0014)", "state_classes": ["document", "derived"]},
{"prefix": "obs", "owner": "09", "owns": "traces, captures, checkpoints", "state_classes": ["observation", "derived"]},
{"prefix": "collab", "owner": "10", "owns": "accepted revisions, branch policy, locks and leases", "state_classes": ["document"]},
{"prefix": "verify", "owner": "11", "owns": "test results, release manifests", "state_classes": ["observation", "document"]},
{"prefix": "gov", "owner": "12", "owns": "agent permissions, approvals, audit log", "state_classes": ["document", "observation"]}],
"fabric_domains": ["fabric.history", "fabric.schema", "fabric.manifest", "fabric.identity", "fabric.provenance", "fabric.edge"],
"actor_kinds": ["human", "tool", "plugin", "agent", "service"], "default_deny": ["agent", "service"]}
```

### Provenance and epistemic classes (episode 067, F3 seam)

- ARCH1-R42: Every record write MUST carry one epistemic class of `epistemic`, and only the transitions listed there are allowed; promotion to accepted needs an actor holding `accept:<domain>` (master plan, Volume 04, episode 067).
- ARCH1-R43: A derived record MUST carry a provenance record with exactly the five F3 fields: input ids, input revisions (as record content digests), toolchain id, parameters digest, environment hash; a missing field is `provenance_incomplete` (master plan section 3, F3).
- ARCH1-R44: The derivation key MUST be SHA-256 over the SLB/1 encoding of {sorted inputs, toolchain id, parameters digest, environment hash} (master plan section 3, F3).
- ARCH1-R45: Provenance MUST ride on the tx record (origin plus per-op override); per-record blame is derived from history, not copied into records (master plan section 1.3, zero 2).

```sl-data
{"epistemic": {"classes": ["observed", "imported", "asserted", "calculated", "inferred", "accepted"],
"transitions": [{"from": "imported", "to": "accepted"}, {"from": "asserted", "to": "accepted"}, {"from": "calculated", "to": "accepted"}, {"from": "inferred", "to": "accepted"}],
"rules": ["observed records are immutable; a new observation supersedes", "demotion only by a new asserted record that supersedes", "accepted names its underlying class"],
"f3_fields": ["input_ids", "input_revisions", "toolchain_id", "parameters_digest", "environment_hash"]}}
```

### Event envelopes (episode 069, F6)

- ARCH1-R46: Every Fabric event MUST be an envelope with id, schema type and version, type string, time from the injected clock, per-source seq, actor, exactly one of subject id or scope tag, correlation {id, scope, tick} and causality {cause event, tx, revision} (master plan section 3, F6).
- ARCH1-R47: Event ids MUST derive from (tx id, kind, seq) so replay reproduces them, and time and correlation MUST NOT enter any hashed body (ADR-0011; ADR-0012).
- ARCH1-R48: The correlation MUST map field for field to `sl::log::correlation` with an owned scope string (master plan section 3, F6; ADR-0009).
- ARCH1-R49: Every envelope MUST validate against `schemas/sl-fabric-event.schema.json`, checked over a JSON Lines dump by `just fabric-event-schema-check` (master plan section 3, F6).

### Revision-store data model (episode 070, data side)

- ARCH1-R50: A revision object MUST hold parents, root digest, schema_set digest, tx record digest and generation, and no time field (ADR-0014).
- ARCH1-R51: The root MUST hold the records trie keyed by `SHA-256("slf-key/1" || id)[0:16]` (radix 16, leaf bucket of at most 32 entries, a subtree of 32 or fewer entries is one leaf) and the per-document membership map (ADR-0014).
- ARCH1-R52: A record object MUST hold {id, type id, owning document id, written-with type version, body} and no time field (ADR-0014; master plan section 4.2).
- ARCH1-R53: Snapshots MUST be immutable and safe to read from any thread, and diff MUST skip equal subtrees (ADR-0014).
- ARCH1-R54: The Fabric object store MUST be the single content-addressed store of the project (ADR-0014).

### Dependency graph and change impact (episodes 072, 073)

- ARCH1-R55: Edges MUST come only from strong and owned ref fields, provenance inputs of derived records and explicit edge records in `fabric.edge`; kinds are reference, containment, derives_from, builds, validates (master plan section 2.4; episode 072).
- ARCH1-R56: Containment, derives_from and builds MUST be acyclic (`dependency_cycle` with the witness path); reference cycles are allowed and condensed (master plan, Volume 04, episode 072).
- ARCH1-R57: The graph index MUST be derived and rebuildable, and an incremental update MUST equal a full rebuild (master plan section 1.3, zero 4).
- ARCH1-R58: Impact closure MUST be deterministic (ordered by distance, then id), grouped by id class, bounded by closure plus frontier edges, and attached to the commit receipt (master plan section 2.4; episode 073).
- ARCH1-R59: `is_current` MUST return `sl::result<bool>` and be false iff a pinned input digest differs from the head digest (master plan section 3, F4 seam; ADR-0009).

### Runtime versus document boundary (episode 074)

- ARCH1-R60: Document state MUST NOT be mutated by the runtime; the runtime reads a snapshot projection and writes back only proposed patches with epistemic class observed, through the transaction manager (master plan section 2.1).
- ARCH1-R61: `engine/runtime` and `engine/render` MUST NOT include `sl/fabric/internal/**` or the capability header (master plan section 3, F5).
- ARCH1-R62: Transient simulation state MUST NOT be representable in a schema (ADR-0013; master plan section 2.2, row 03).

### Reference resolver (episode 075)

- ARCH1-R63: Resolution MUST be a pure function of (snapshot, manifest package pins) in the total order same document, project, packages in pin order, else unresolved; names never resolve (master plan, Volume 04, episode 075).
- ARCH1-R64: `resolve` MUST return `sl::result<resolution>`: read failures are errors; outcomes are resolved, retired, missing, wrong_type, package_missing (ADR-0009).
- ARCH1-R65: An external reference MUST become a record id only through an import record with an imported-scheme id (ADR-0011).
- ARCH1-R66: A cross-package reference MUST resolve only through a pinned package revision (master plan, Volume 04, episode 075).

### Schema-generated tooling (episode 078)

- ARCH1-R67: Descriptors, typed structs, validator tables and inspector metadata MUST be generated by `tools/fabric/slgen.py` and committed under `engine/fabric/gen/` (ADR-0013).
- ARCH1-R68: Generated output MUST equal the generator output (`just fabric-gen-check`) (ADR-0013).
- ARCH1-R69: Every type MUST have fixtures and pass the generic conformance cases before release (ADR-0013; master plan, Volume 04, episode 079).

### Error codes (ADR-0010)

```sl-data
{"error_codes": [
["fabric_failure", 5000], ["id_parse", 5001], ["id_class_mismatch", 5002], ["id_reuse", 5003], ["entropy_failure", 5004],
["object_corrupt", 5010], ["object_missing", 5011], ["ref_cas_failed", 5012], ["store_io", 5013], ["format_unsupported", 5014], ["read_only_newer", 5015],
["decode_noncanonical", 5020], ["decode_truncated", 5021], ["decode_limit", 5022], ["decode_bad_utf8", 5023], ["decode_bad_float", 5024],
["decode_unknown_tag", 5025], ["decode_unknown_field", 5026], ["encode_invalid", 5027], ["text_syntax", 5028],
["type_mismatch", 5040], ["schema_invalid", 5041], ["schema_frozen", 5042], ["schema_unknown", 5043], ["schema_incompatible", 5044],
["migration_failed", 5050], ["migration_missing_step", 5051], ["migration_lossy", 5052],
["ref_dangling", 5055], ["ref_unresolved", 5056], ["ref_wrong_type", 5057], ["dependency_cycle", 5058],
["ownership_conflict", 5060], ["ownership_orphan", 5061], ["authority_denied", 5062], ["registry_frozen", 5063],
["provenance_incomplete", 5065], ["promotion_denied", 5066],
["validation_failed", 5070], ["stale_base", 5071], ["conflict", 5072], ["locked", 5073], ["tx_aborted", 5074], ["tx_closed", 5075],
["invariant_violated", 5076], ["commit_indeterminate", 5077], ["idempotency_mismatch", 5078],
["undo_empty", 5090], ["undo_barrier", 5091], ["undo_conflict", 5092], ["compensation_failed", 5093],
["journal_corrupt", 5095], ["recovery_required", 5096], ["writer_locked", 5097], ["quarantined", 5098], ["checkpoint_invalid", 5099], ["store_corrupt", 5100],
["manifest_invalid", 5110], ["manifest_missing", 5111], ["module_unmet", 5112], ["reader_too_old", 5113], ["capability_unknown", 5114], ["event_invalid", 5120]],
"reserved": "5125-5999"}
```

### API surface (namespace `sl::fabric`, headers `engine/fabric/include/sl/fabric/`, target `sl_fabric`)

```cpp
// id.hpp
enum class id_class : std::uint8_t; enum class mint_scheme : std::uint8_t;
class record_id; template <id_class C> class typed_id;            // asset_id, entity_id, instance_id, tx_id, ...
class id_source { public: virtual result<void> fill(std::span<std::uint8_t>) = 0; };
class os_entropy_source; static result<os_entropy_source> os_entropy_source::create();
class deterministic_source;                                         // (seed, stream)
result<record_id> mint(id_class, id_source&);
record_id derive(id_class, const record_id& parent, std::string_view key) noexcept;
record_id well_known(id_class, std::uint32_t ordinal) noexcept;
record_id imported(id_class, std::string_view system, std::string_view external_id) noexcept;
// digest.hpp: struct digest; enum class object_kind; digest hash_object(object_kind, frame_header, byte_span) noexcept;
// value.hpp, type.hpp, schema.hpp: class value; struct type_desc; struct field_desc; class schema_registry;
// codec.hpp: result<bytes> encode(const value&); result<value> decode(byte_span, unknown_policy);
//            result<std::string> to_text(const value&, type_id, const schema_registry&); result<value> from_text(...)
// snapshot.hpp: class snapshot { result<record_view> get(const record_id&) const; result<std::vector<record_delta>> diff(...) const;
//               result<void> for_each_in_document(const record_id& doc, F&&) const; digest root_digest() const noexcept; };
// ownership.hpp: class domain_registry { result<void> claim(domain_spec); void freeze() noexcept; f2_report f2() const; };
// provenance.hpp: enum class epistemic_class; struct provenance; result<void> check_provenance(const provenance&);
// events.hpp: struct event_envelope; class event_bus;  deps.hpp: class dependency_index;  impact.hpp: struct impact_set;
// resolver.hpp: class resolver { result<resolution> resolve(const reference&) const; };
// boundary.hpp: class document_projection; result<patch> propose_from_observation(...);
```

### Invariants

Every invariant has exactly one owning work order and one test location; a C++ test case name, or a Python test
method name in `test_fab_inv_<area>_<nn>_` form, starts with the id. `just fabric-contracts-check` (WO-036) binds
both ways; `just fabric-contracts-strict` (WO-062) also requires every test to exist and pass.

```sl-data
{"invariants": [
{"id": "FAB-INV-ID-01", "rule": "ARCH1-R01", "text": "record id bytes and text round trip; parse rejects bad prefix, prefix/class mismatch, length, alphabet, first char above 7", "test": "tests/unit/fabric/id_text_test.cpp", "wo": "WO-037", "verified_by": "just fabric-test id"},
{"id": "FAB-INV-ID-02", "rule": "ARCH1-R02", "text": "deterministic_source mint sequence equals the golden list; os_entropy_source::create self-test passes; 1e5 draws distinct", "test": "tests/unit/fabric/id_mint_test.cpp", "wo": "WO-037", "verified_by": "just fabric-test id"},
{"id": "FAB-INV-ID-03", "rule": "ARCH1-R03", "text": "derive, well_known and imported are pure and domain separated by class and scheme", "test": "tests/unit/fabric/id_derive_test.cpp", "wo": "WO-037", "verified_by": "just fabric-test id"},
{"id": "FAB-INV-ID-04", "rule": "ARCH1-R01", "text": "typed_id from another class is id_class_mismatch; wrong-class assignment does not compile", "test": "tests/unit/fabric/id_typed_test.cpp", "wo": "WO-037", "verified_by": "just fabric-test id"},
{"id": "FAB-INV-ID-05", "rule": "ARCH1-R02", "text": "a failing id_source makes mint return entropy_failure, never a null or repeated id", "test": "tests/unit/fabric/id_mint_failure_test.cpp", "wo": "WO-037", "verified_by": "just fabric-test id"},
{"id": "FAB-INV-ID-06", "rule": "ARCH1-R09", "text": "C++ derive, imported and object ids equal the Python hashlib vectors", "test": "tests/unit/fabric/codec_vectors_test.cpp", "wo": "WO-041", "verified_by": "just fabric-test codec"},
{"id": "FAB-INV-DG-01", "rule": "ARCH1-R09", "text": "SHA-256 NIST vectors including one million 'a'; incremental equals one-shot", "test": "tests/unit/fabric/digest_sha256_test.cpp", "wo": "WO-037", "verified_by": "just fabric-test digest"},
{"id": "FAB-INV-DG-02", "rule": "ARCH1-R09", "text": "changing any frame header byte (major, minor, kind, flags, length) changes the object id", "test": "tests/unit/fabric/digest_oid_test.cpp", "wo": "WO-037", "verified_by": "just fabric-test digest"},
{"id": "FAB-INV-BY-01", "rule": "ARCH1-R26", "text": "LEB128 is shortest form; overlong and over-64-bit varints are rejected", "test": "tests/unit/fabric/bytes_varint_test.cpp", "wo": "WO-037", "verified_by": "just fabric-test bytes"},
{"id": "FAB-INV-BY-02", "rule": "ARCH1-R30", "text": "byte_reader never reads out of bounds; truncation is decode_truncated", "test": "tests/unit/fabric/bytes_reader_test.cpp", "wo": "WO-037", "verified_by": "just fabric-test bytes"},
{"id": "FAB-INV-TY-01", "rule": "ARCH1-R13", "text": "type_desc well-formed: unique field numbers, names, enumerators and cases; reserved numbers unused; recursion only through optional field, list, map or oset", "test": "tests/unit/fabric/type_wellformed_test.cpp", "wo": "WO-039", "verified_by": "just fabric-test type"},
{"id": "FAB-INV-TY-02", "rule": "ARCH1-R12", "text": "check accepts iff the value conforms (presence, width ranges, enumerators, cases, ref targets) with sorted diagnostics", "test": "tests/unit/fabric/type_conformance_test.cpp", "wo": "WO-039", "verified_by": "just fabric-test type"},
{"id": "FAB-INV-TY-03", "rule": "ARCH1-R16", "text": "with and without leave the original bit-identical and share untouched subtrees", "test": "tests/unit/fabric/value_persistent_test.cpp", "wo": "WO-039", "verified_by": "just fabric-test value"},
{"id": "FAB-INV-TY-04", "rule": "ARCH1-R22", "text": "schema_set digest is independent of registration order and equals the schema object ids of the Python lock", "test": "tests/unit/fabric/codec_schema_set_test.cpp", "wo": "WO-041", "verified_by": "just fabric-test codec"},
{"id": "FAB-INV-TY-05", "rule": "ARCH1-R17", "text": "freeze rejects record types with no domain or a state class outside document, derived, observation; add after freeze is schema_frozen", "test": "tests/unit/fabric/schema_freeze_test.cpp", "wo": "WO-039", "verified_by": "just fabric-test schema"},
{"id": "FAB-INV-TY-06", "rule": "ARCH1-R15", "text": "oset element ids unique and order keys valid; a list of identity-bearing structs is ill-formed", "test": "tests/unit/fabric/type_oset_test.cpp", "wo": "WO-039", "verified_by": "just fabric-test type"},
{"id": "FAB-INV-TY-07", "rule": "ARCH1-R14", "text": "absent and present-equal-to-default are distinct values; reading applies the default without storing it", "test": "tests/unit/fabric/value_presence_test.cpp", "wo": "WO-039", "verified_by": "just fabric-test value"},
{"id": "FAB-INV-SCH-01", "rule": "ARCH1-R21", "text": "Python classifier matches every row of the compatibility golden table", "test": "tools/fabric/test_schema_compat.py", "wo": "WO-040", "verified_by": "just fabric-schema-check"},
{"id": "FAB-INV-SCH-02", "rule": "ARCH1-R20", "text": "editing a released type changes its lock hash and fails R-SCH-RELEASED", "test": "tools/fabric/test_schema_lock.py", "wo": "WO-040", "verified_by": "just fabric-schema-check"},
{"id": "FAB-INV-SCH-03", "rule": "ARCH1-R21", "text": "reserved-number reuse and domain, state-class or record-class changes are refused", "test": "tools/fabric/test_schema_rules.py", "wo": "WO-040", "verified_by": "just fabric-schema-check"},
{"id": "FAB-INV-SCH-04", "rule": "ARCH1-R21", "text": "C++ classify equals the Python classifier on the golden table", "test": "tests/unit/fabric/migration_classify_test.cpp", "wo": "WO-057", "verified_by": "just fabric-test migration"},
{"id": "FAB-INV-SCH-05", "rule": "ARCH1-R25", "text": "a migration chain composes and gives identical output bytes for identical input", "test": "tests/unit/fabric/migration_plan_test.cpp", "wo": "WO-057", "verified_by": "just fabric-test migration"},
{"id": "FAB-INV-SCH-06", "rule": "ARCH1-R25", "text": "a failed migration commits nothing and leaves the old revision readable", "test": "tests/unit/fabric/migration_failure_test.cpp", "wo": "WO-057", "verified_by": "just fabric-test migration"},
{"id": "FAB-INV-SCH-07", "rule": "ARCH1-R23", "text": "a record from a newer minor is preserved byte-identically and refused for write", "test": "tests/unit/fabric/migration_newer_test.cpp", "wo": "WO-057", "verified_by": "just fabric-test migration"},
{"id": "FAB-INV-SCH-08", "rule": "ARCH1-R24", "text": "a deprecated field decodes and writing it yields a warning diagnostic", "test": "tests/unit/fabric/migration_deprecation_test.cpp", "wo": "WO-057", "verified_by": "just fabric-test migration"},
{"id": "FAB-INV-SER-01", "rule": "ARCH1-R26", "text": "Python reference: each accept vector encodes to its hex and oid; each reject vector fails with its rule", "test": "tools/fabric/test_slb_ref.py", "wo": "WO-038", "verified_by": "just fabric-golden-check"},
{"id": "FAB-INV-SER-02", "rule": "ARCH1-R29", "text": "Python reference: binary and text are bijective on the corpus", "test": "tools/fabric/test_slb_text.py", "wo": "WO-038", "verified_by": "just fabric-golden-check"},
{"id": "FAB-INV-SER-03", "rule": "ARCH1-R31", "text": "C++ encoder reproduces every accept vector; C++ decoder rejects every reject vector with the same rule", "test": "tests/unit/fabric/codec_golden_test.cpp", "wo": "WO-041", "verified_by": "just fabric-test codec"},
{"id": "FAB-INV-SER-04", "rule": "ARCH1-R31", "text": "clang and gcc builds emit identical corpus bytes equal to the committed corpus", "test": "just fabric-determinism", "wo": "WO-041", "verified_by": "just fabric-determinism"},
{"id": "FAB-INV-SER-05", "rule": "ARCH1-R29", "text": "binary to text to binary is the identity over 2000 seeded random values", "test": "tests/unit/fabric/codec_roundtrip_test.cpp", "wo": "WO-041", "verified_by": "just fabric-test codec"},
{"id": "FAB-INV-SER-06", "rule": "ARCH1-R11", "text": "every byte flip and truncation of 20 samples decodes to an error or re-encodes identically, sanitizer clean", "test": "tests/integration/fabric/codec_corruption_test.cpp", "wo": "WO-041", "verified_by": "just fabric-test codec"},
{"id": "FAB-INV-SER-07", "rule": "ARCH1-R27", "text": "NaN and infinities rejected; -0.0 encoded as +0.0 and its pattern rejected on decode", "test": "tests/unit/fabric/codec_float_test.cpp", "wo": "WO-041", "verified_by": "just fabric-test codec"},
{"id": "FAB-INV-SER-08", "rule": "ARCH1-R28", "text": "unknown fields preserved byte-exact under preserve and rejected under strict", "test": "tests/unit/fabric/codec_unknown_test.cpp", "wo": "WO-041", "verified_by": "just fabric-test codec"},
{"id": "FAB-INV-SER-09", "rule": "ARCH1-R30", "text": "depth, item and size limits fail before allocation", "test": "tests/unit/fabric/codec_limits_test.cpp", "wo": "WO-041", "verified_by": "just fabric-test codec"},
{"id": "FAB-INV-GEN-01", "rule": "ARCH1-R68", "text": "committed generated files equal slgen output; slgen is deterministic", "test": "tools/fabric/test_slgen.py", "wo": "WO-045", "verified_by": "just fabric-gen-check"},
{"id": "FAB-INV-GEN-02", "rule": "ARCH1-R67", "text": "every generated typed struct round trips through to_value and from_value on its fixtures", "test": "tests/unit/fabric/gen_roundtrip_test.cpp", "wo": "WO-045", "verified_by": "just fabric-test gen"},
{"id": "FAB-INV-GEN-03", "rule": "ARCH1-R20", "text": "generated descriptors hash to the lock hash the Python tool computes", "test": "tests/unit/fabric/gen_schema_hash_test.cpp", "wo": "WO-045", "verified_by": "just fabric-test gen"},
{"id": "FAB-INV-GEN-04", "rule": "ARCH1-R67", "text": "inspector metadata lists every field with doc, domain, owner and state class", "test": "tools/fabric/test_slgen_meta.py", "wo": "WO-045", "verified_by": "just fabric-gen-check"},
{"id": "FAB-INV-GEN-05", "rule": "ARCH1-R29", "text": "cut line: generated text JSON Schema accepts exactly what the C++ validator accepts on the corpus", "test": "tests/unit/fabric/gen_text_schema_test.cpp", "wo": "WO-045", "verified_by": "just fabric-test gen"},
{"id": "FAB-INV-OWN-01", "rule": "ARCH1-R36", "text": "a second claim with another owner is ownership_conflict naming both; F2 numerator stays 0", "test": "tests/unit/fabric/ownership_claim_test.cpp", "wo": "WO-048", "verified_by": "just fabric-test ownership"},
{"id": "FAB-INV-OWN-02", "rule": "ARCH1-R37", "text": "a type with no domain is ownership_orphan", "test": "tests/unit/fabric/ownership_orphan_test.cpp", "wo": "WO-048", "verified_by": "just fabric-test ownership"},
{"id": "FAB-INV-OWN-03", "rule": "ARCH1-R36", "text": "the twelve reserved prefixes equal the subsystems of master plan section 2.2", "test": "tests/unit/fabric/ownership_reserved_test.cpp", "wo": "WO-048", "verified_by": "just fabric-test ownership"},
{"id": "FAB-INV-OWN-04", "rule": "ARCH1-R38", "text": "a claim after freeze is registry_frozen", "test": "tests/unit/fabric/ownership_freeze_test.cpp", "wo": "WO-048", "verified_by": "just fabric-test ownership"},
{"id": "FAB-INV-OWN-05", "rule": "ARCH1-R36", "text": "the authority validator denies a non-owner write with authority_denied", "test": "tests/unit/fabric/authority_grant_test.cpp", "wo": "WO-048", "verified_by": "just fabric-test authority"},
{"id": "FAB-INV-OWN-06", "rule": "ARCH1-R39", "text": "a nested value of another domain needs that domain's grant", "test": "tests/unit/fabric/authority_innermost_test.cpp", "wo": "WO-048", "verified_by": "just fabric-test authority"},
{"id": "FAB-INV-OWN-07", "rule": "ARCH1-R40", "text": "agent and service actors are denied by default; an explicit grant allows", "test": "tests/unit/fabric/authority_default_deny_test.cpp", "wo": "WO-048", "verified_by": "just fabric-test authority"},
{"id": "FAB-INV-OWN-08", "rule": "ARCH1-R36", "text": "schema-file domains, the C++ dump and the domain_registry table agree, one owner each", "test": "tools/fabric/test_ownership_check.py", "wo": "WO-048", "verified_by": "just fabric-ownership-check"},
{"id": "FAB-INV-OWN-09", "rule": "ARCH1-R41", "text": "records without owner, stable id or revision over all records is 0 after the conformance workload", "test": "tests/integration/fabric/conformance_gate1_test.cpp", "wo": "WO-062", "verified_by": "just fabric-conformance"},
{"id": "FAB-INV-PRV-01", "rule": "ARCH1-R43", "text": "each missing F3 field makes a derived record provenance_incomplete", "test": "tests/unit/fabric/provenance_fields_test.cpp", "wo": "WO-049", "verified_by": "just fabric-test provenance"},
{"id": "FAB-INV-PRV-02", "rule": "ARCH1-R43", "text": "the provenance record has exactly the five F3 fields, none of the build-record fields schema, target, artifact, built_at", "test": "tests/unit/fabric/provenance_shape_test.cpp", "wo": "WO-049", "verified_by": "just fabric-test provenance"},
{"id": "FAB-INV-PRV-03", "rule": "ARCH1-R42", "text": "the transition matrix is exact over all 36 pairs; promotion needs accept:<domain>; observed is immutable", "test": "tests/unit/fabric/provenance_class_test.cpp", "wo": "WO-049", "verified_by": "just fabric-test provenance"},
{"id": "FAB-INV-PRV-04", "rule": "ARCH1-R44", "text": "derivation key is pure, independent of input order, sensitive to every field", "test": "tests/unit/fabric/provenance_key_test.cpp", "wo": "WO-049", "verified_by": "just fabric-test provenance"},
{"id": "FAB-INV-MAN-01", "rule": "ARCH1-R32", "text": "exactly one manifest at the well-known id; a second is manifest_invalid", "test": "tests/unit/fabric/manifest_open_test.cpp", "wo": "WO-054", "verified_by": "just fabric-test manifest"},
{"id": "FAB-INV-MAN-02", "rule": "ARCH1-R34", "text": "unmet module, capability or target profile is a typed refusal and nothing opens", "test": "tests/unit/fabric/manifest_requirements_test.cpp", "wo": "WO-054", "verified_by": "just fabric-test manifest"},
{"id": "FAB-INV-MAN-03", "rule": "ARCH1-R34", "text": "minimum reader above the engine is reader_too_old; newer writer minor opens read-only", "test": "tests/unit/fabric/manifest_version_test.cpp", "wo": "WO-054", "verified_by": "just fabric-test manifest"},
{"id": "FAB-INV-MAN-04", "rule": "ARCH1-R35", "text": "project.slp accepts only format, store and project id", "test": "tests/unit/fabric/manifest_locator_test.cpp", "wo": "WO-054", "verified_by": "just fabric-test manifest"},
{"id": "FAB-INV-MAN-05", "rule": "ARCH1-R33", "text": "the manifest changes only through a transaction; package pins are unique", "test": "tests/unit/fabric/manifest_tx_test.cpp", "wo": "WO-054", "verified_by": "just fabric-test manifest"},
{"id": "FAB-INV-EVT-01", "rule": "ARCH1-R46", "text": "envelope complete, with exactly one of subject id or scope tag", "test": "tests/unit/fabric/events_envelope_test.cpp", "wo": "WO-047", "verified_by": "just fabric-test events"},
{"id": "FAB-INV-EVT-02", "rule": "ARCH1-R47", "text": "event ids derive from (tx, kind, seq) and exclude time and correlation", "test": "tests/unit/fabric/events_ids_test.cpp", "wo": "WO-047", "verified_by": "just fabric-test events"},
{"id": "FAB-INV-EVT-03", "rule": "ARCH1-R48", "text": "correlation maps field for field to sl::log::correlation with an owned scope", "test": "tests/unit/fabric/events_correlation_test.cpp", "wo": "WO-047", "verified_by": "just fabric-test events"},
{"id": "FAB-INV-EVT-04", "rule": "ARCH1-R46", "text": "delivery is ordered by commit seq", "test": "tests/unit/fabric/events_bus_test.cpp", "wo": "WO-047", "verified_by": "just fabric-test events"},
{"id": "FAB-INV-EVT-05", "rule": "ARCH1-R49", "text": "the JSON Lines dump validates against schemas/sl-fabric-event.schema.json", "test": "tools/fabric/test_event_schema_check.py", "wo": "WO-047", "verified_by": "just fabric-event-schema-check"},
{"id": "FAB-INV-REV-01", "rule": "ARCH1-R51", "text": "trie root depends only on the entry set over 200 permutations", "test": "tests/unit/fabric/tree_history_test.cpp", "wo": "WO-046", "verified_by": "just fabric-test tree"},
{"id": "FAB-INV-REV-02", "rule": "ARCH1-R51", "text": "trie keys are SHA-256('slf-key/1' || id)[0:16]; ids sharing class and scheme spread across the root fan-out", "test": "tests/unit/fabric/tree_key_test.cpp", "wo": "WO-046", "verified_by": "just fabric-test tree"},
{"id": "FAB-INV-REV-03", "rule": "ARCH1-R53", "text": "diff equals the symmetric difference of a map oracle and skips equal subtrees", "test": "tests/unit/fabric/tree_diff_test.cpp", "wo": "WO-046", "verified_by": "just fabric-test tree"},
{"id": "FAB-INV-REV-04", "rule": "ARCH1-R50", "text": "revision oid covers parents, root, schema_set, tx and generation; no time field exists", "test": "tests/unit/fabric/revision_object_test.cpp", "wo": "WO-046", "verified_by": "just fabric-test revision"},
{"id": "FAB-INV-REV-05", "rule": "ARCH1-R51", "text": "changing one record in a 100k-record trie writes at most depth + 4 objects", "test": "tests/integration/fabric/tree_cost_test.cpp", "wo": "WO-046", "verified_by": "just fabric-test tree"},
{"id": "FAB-INV-REV-06", "rule": "ARCH1-R51", "text": "the membership map equals the projection of record document fields", "test": "tests/unit/fabric/snapshot_membership_test.cpp", "wo": "WO-046", "verified_by": "just fabric-test snapshot"},
{"id": "FAB-INV-REV-07", "rule": "ARCH1-R53", "text": "snapshot reads equal the model oracle after seeded random operation sequences", "test": "tests/unit/fabric/snapshot_differential_test.cpp", "wo": "WO-046", "verified_by": "just fabric-test snapshot"},
{"id": "FAB-INV-DEP-01", "rule": "ARCH1-R55", "text": "extracted edges equal strong and owned refs plus provenance inputs; weak refs make no edge", "test": "tests/unit/fabric/deps_extract_test.cpp", "wo": "WO-044", "verified_by": "just fabric-test deps"},
{"id": "FAB-INV-DEP-02", "rule": "ARCH1-R56", "text": "a containment or derivation cycle is dependency_cycle with the witness path; reference cycles condense", "test": "tests/unit/fabric/deps_cycle_test.cpp", "wo": "WO-044", "verified_by": "just fabric-test deps"},
{"id": "FAB-INV-DEP-03", "rule": "ARCH1-R58", "text": "reverse closure equals a brute-force oracle, ordered by distance then id", "test": "tests/unit/fabric/deps_closure_test.cpp", "wo": "WO-044", "verified_by": "just fabric-test deps"},
{"id": "FAB-INV-IMP-01", "rule": "ARCH1-R57", "text": "incremental index equals full rebuild after every commit of a seeded workload", "test": "tests/integration/fabric/impact_incremental_test.cpp", "wo": "WO-056", "verified_by": "just fabric-test impact"},
{"id": "FAB-INV-IMP-02", "rule": "ARCH1-R58", "text": "visited nodes never exceed closure plus frontier edges", "test": "tests/unit/fabric/impact_bounded_test.cpp", "wo": "WO-056", "verified_by": "just fabric-test impact"},
{"id": "FAB-INV-IMP-03", "rule": "ARCH1-R59", "text": "is_current is false iff a pinned input digest differs from the head", "test": "tests/unit/fabric/impact_currency_test.cpp", "wo": "WO-056", "verified_by": "just fabric-test impact"},
{"id": "FAB-INV-IMP-04", "rule": "ARCH1-R58", "text": "the impact summary on the receipt and event groups affected ids by class", "test": "tests/unit/fabric/impact_receipt_test.cpp", "wo": "WO-056", "verified_by": "just fabric-test impact"},
{"id": "FAB-INV-BND-01", "rule": "ARCH1-R60", "text": "runtime changes enter only as proposed patches with epistemic class observed", "test": "tests/unit/fabric/boundary_proposal_test.cpp", "wo": "WO-061", "verified_by": "just fabric-test boundary"},
{"id": "FAB-INV-BND-02", "rule": "ARCH1-R60", "text": "document_projection exposes no mutator (requires-expression static_assert)", "test": "tests/unit/fabric/boundary_projection_test.cpp", "wo": "WO-061", "verified_by": "just fabric-test boundary"},
{"id": "FAB-INV-BND-03", "rule": "ARCH1-R61", "text": "an include of sl/fabric/internal or the capability header from engine/runtime or engine/render fails R-BND-INCLUDE", "test": "tools/fabric/test_boundary_include.py", "wo": "WO-061", "verified_by": "just fabric-surface-check"},
{"id": "FAB-INV-REF-01", "rule": "ARCH1-R63", "text": "resolution follows document, project, pinned packages in order, else unresolved", "test": "tests/unit/fabric/resolver_order_test.cpp", "wo": "WO-060", "verified_by": "just fabric-test resolver"},
{"id": "FAB-INV-REF-02", "rule": "ARCH1-R64", "text": "retired, missing, wrong_type and package_missing are distinct outcomes; a read failure is an error", "test": "tests/unit/fabric/resolver_outcome_test.cpp", "wo": "WO-060", "verified_by": "just fabric-test resolver"},
{"id": "FAB-INV-REF-03", "rule": "ARCH1-R63", "text": "resolution is a pure function of snapshot and pins", "test": "tests/unit/fabric/resolver_determinism_test.cpp", "wo": "WO-060", "verified_by": "just fabric-test resolver"},
{"id": "FAB-INV-REF-04", "rule": "ARCH1-R65", "text": "an external reference resolves only through an import record", "test": "tests/unit/fabric/resolver_external_test.cpp", "wo": "WO-060", "verified_by": "just fabric-test resolver"},
{"id": "FAB-INV-LC-01", "rule": "ARCH1-R05", "text": "a retired id cannot be re-created except by the undo of the retiring transaction", "test": "tests/unit/fabric/identity_lifetime_test.cpp", "wo": "WO-058", "verified_by": "just fabric-test identity"},
{"id": "FAB-INV-LC-02", "rule": "ARCH1-R05", "text": "every class gets a tombstone that keeps the last record oid", "test": "tests/unit/fabric/identity_tombstone_test.cpp", "wo": "WO-058", "verified_by": "just fabric-test identity"},
{"id": "FAB-INV-LC-03", "rule": "ARCH1-R07", "text": "instance ids are stable across re-instantiation and sibling reorder", "test": "tests/unit/fabric/identity_instance_test.cpp", "wo": "WO-058", "verified_by": "just fabric-test identity"},
{"id": "FAB-INV-LC-04", "rule": "ARCH1-R07", "text": "imported ids are stable across re-import and derived ids across re-cook", "test": "tests/unit/fabric/identity_import_test.cpp", "wo": "WO-058", "verified_by": "just fabric-test identity"},
{"id": "FAB-INV-LC-05", "rule": "ARCH1-R17", "text": "a record whose id class differs from its type's record class is rejected", "test": "tests/unit/fabric/identity_class_test.cpp", "wo": "WO-058", "verified_by": "just fabric-test identity"},
{"id": "FAB-INV-CF-01", "rule": "ARCH1-R69", "text": "invariants, owning work orders and test names are in bijection; retired ids are listed", "test": "tools/fabric/test_contracts_check.py", "wo": "WO-036", "verified_by": "just fabric-contracts-check"},
{"id": "FAB-INV-CF-02", "rule": "ARCH1-R69", "text": "the generic suite passes for every registered type: round trip, canonical order, fixtures, ownership, refs, transactional mutation, inverse law, migration fixtures", "test": "tests/integration/fabric/conformance_generic_test.cpp", "wo": "WO-062", "verified_by": "just fabric-conformance"},
{"id": "FAB-INV-CF-03", "rule": "ARCH1-R69", "text": "the contract dump equals the sl-data tables of both documents and the committed contract lock", "test": "tools/fabric/test_contract_lock.py", "wo": "WO-062", "verified_by": "just fabric-contract-lock"},
{"id": "FAB-INV-CF-04", "rule": "ARCH1-R11", "text": "persisted-path code has no throw, try, .at(), std::sto*, filesystem call without error_code, or unordered container", "test": "tools/fabric/test_nothrow_lint.py", "wo": "WO-036", "verified_by": "just fabric-nothrow-check"}],
"retired": []}
```

## Measures and enforcement points

- F2 (ARCH1-R36): numerator domains with more than one registered owner, denominator all registered domains, threshold 0; `just fabric-ownership-check`, `just neg-fabric-f2` (master plan section 3).
- F3 seam (ARCH1-R43): derived records missing any F3 field over derived records, threshold 0; FAB-INV-PRV-01 and the commit-path refusal FAB-INV-TX-13 of SL-ARCH-002; the artifact-store refusal itself is episode 182.
- F6 envelope (ARCH1-R46 to R49): envelopes missing required fields over envelopes, threshold 0; `just fabric-event-schema-check`, `just neg-fabric-event-schema`.
- Release gate 1 measure (ARCH1-R41): FAB-INV-OWN-09 under `just fabric-conformance`.
- Determinism (ARCH1-R31): `just fabric-golden-check`, `just fabric-vectors-check`, `just fabric-determinism`, each with its neg test.
- Schemas (ARCH1-R19 to R21, R67 to R69): `just fabric-schema-check`, `just fabric-gen-check`, `just neg-fabric-schema`, `just neg-fabric-gen`.
- Invariant coverage: invariants with a passing test over invariants, threshold 1.0; `just fabric-contracts-check` is pending-tolerant until WO-062, then `just fabric-contracts-strict`; `just fabric-contract-lock` binds the tables; `just contracts-pending` lists what is still a stub.
- Error behaviour (ADR-0009): `just fabric-nothrow-check` with `just neg-fabric-nothrow`; `just tidy` in the byte-heavy work orders.
- Performance (master plan section 6.1): work-bound proxies only (objects per commit FAB-INV-REV-05, visited nodes FAB-INV-IMP-02); wall-time benchmarks are Volume 16.

## Traceability

Master plan sections 1.1, 1.2, 1.3, 1.5, 2.1, 2.2, 2.4, 3 (F2, F3, F4 seam, F5, F6), 4.2, 6.1, 6.2, Release gates
(gate 1), Volume 04 episodes SL-EP-061 to SL-EP-069, SL-EP-072 to SL-EP-075, SL-EP-078 to SL-EP-080. Requirements:
REQ-DOCTRINE-001, REQ-DOCTRINE-005, REQ-FRAG-002, REQ-FRAG-003 (seam), REQ-FRAG-006 (envelope part),
REQ-RELEASE-001. Decisions ADR-0009 to ADR-0014, ADR-0017, ADR-0019, ADR-0020. Work orders WO-035 to WO-063.
SL-ARCH-002 for transactions, undo, journal, recovery and corruption.

## Open issues

- Founder decisions before the one-way-door ADRs are accepted: SHA-256 versus BLAKE3 (algo 2 is only reserved); the finite-float policy (-0.0 canonicalised, no NaN-carrying domain planned); the epistemic transition matrix and any precedence order (the plan lists six class names only); the reserved prefix names; tombstone retention versus legal deletion.
- Random-id collision probability is bounded by 2^-112 per pair; no detection beyond the trie's duplicate-key refusal exists.
- Package publishing and package-level id guarantees are Volume 38 work; Volume 04 resolves only pinned package revisions.
- Selection churn: selection is `shell` document state, so whether it lives on a per-actor ref outside the undo history is undecided.
- Record clustering for million-record projects and large-blob chunking (episode 348) are not designed here.
- Big-endian hosts are untested; all byte layouts are little-endian by specification.
- Reading master plan section 2.2 row 08 under ADR-0014 needs a registry amendment with an impact note for episodes 182 and 348 (master plan section 6.3 rule 3).
- FAB-INV-GEN-05 is a cut line of WO-045; if cut it moves to `retired` with a note in WO-062.

## Change log

| Version | Date | Change |
| :---- | :---- | :---- |
| 0.1.0 | 2026-10-18 | First draft, before any Volume 04 code (WO-035). |
