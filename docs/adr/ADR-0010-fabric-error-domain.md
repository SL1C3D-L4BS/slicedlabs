---
id: ADR-0010
title: "Fabric error domain: sl::errc 5000-5999, every code reserved up front"
status: Proposed
date: 2026-10-18
decision_refs: []
supersedes: []
---
# ADR-0010: Fabric error domain: sl::errc 5000-5999, every code reserved up front

## Context

ADR-0009 makes errors values (`sl::result<T>`) and fixes the code ranges core 1000-1999, platform 2000-2999,
graphics 3000-3999 and asset 4000-4999 (machine fact: `engine/core/include/sl/core/error.hpp`). Volume 04 builds the
Fabric, which owns IDs, schemas, revisions, transactions and provenance (master plan section 2.2, row 02), and whose
rejections must be structured (master plan section 2.4, "Structured rejection, nothing written"). No range exists
for it, and ADR-0009 lists the domains, so a new range needs an ADR. Codes keep their number for life (master plan
section 6.3 rule 2, applied to codes by ADR-0009).

## Decision

- `sl::errc` MUST gain the range 5000-5999 and `sl::error_domain` MUST gain `fabric` (case 5 of `domain()`), extending ADR-0009 without changing any existing code (master plan section 2.2, row 02).
- Every code below MUST be added in one change (work order WO-037) together with `to_string`, `describe` and `tests/unit/error_test.cpp`, so no later work order edits `error.hpp` (master plan section 6.3 rule 4: declared files).
- A code number MUST NOT be reused or renumbered; 5125-5999 stay reserved for later Fabric volumes (master plan section 6.3 rule 2).
- Detail that is not a code (rule id such as R-SER-ORDER, byte offset, record id, path) MUST travel in the diagnostic or the message, never as a new code per rule (master plan section 2.4).

Codes (name = number), grouped by sub-range:

- general: fabric_failure 5000.
- identity 5001-5009: id_parse 5001, id_class_mismatch 5002, id_reuse 5003, entropy_failure 5004.
- objects and store 5010-5019: object_corrupt 5010, object_missing 5011, ref_cas_failed 5012, store_io 5013,
  format_unsupported 5014, read_only_newer 5015.
- codec 5020-5039: decode_noncanonical 5020, decode_truncated 5021, decode_limit 5022, decode_bad_utf8 5023,
  decode_bad_float 5024, decode_unknown_tag 5025, decode_unknown_field 5026, encode_invalid 5027, text_syntax 5028.
- types and schemas 5040-5049: type_mismatch 5040, schema_invalid 5041, schema_frozen 5042, schema_unknown 5043,
  schema_incompatible 5044.
- migration 5050-5054: migration_failed 5050, migration_missing_step 5051, migration_lossy 5052.
- references 5055-5059: ref_dangling 5055, ref_unresolved 5056, ref_wrong_type 5057, dependency_cycle 5058.
- ownership and authority 5060-5064: ownership_conflict 5060, ownership_orphan 5061, authority_denied 5062,
  registry_frozen 5063.
- provenance 5065-5069: provenance_incomplete 5065, promotion_denied 5066.
- transactions 5070-5089: validation_failed 5070, stale_base 5071, conflict 5072, locked 5073, tx_aborted 5074,
  tx_closed 5075, invariant_violated 5076, commit_indeterminate 5077, idempotency_mismatch 5078.
- undo 5090-5094: undo_empty 5090, undo_barrier 5091, undo_conflict 5092, compensation_failed 5093.
- recovery and corruption 5095-5109: journal_corrupt 5095, recovery_required 5096, writer_locked 5097,
  quarantined 5098, checkpoint_invalid 5099, store_corrupt 5100.
- manifest 5110-5119: manifest_invalid 5110, manifest_missing 5111, module_unmet 5112, reader_too_old 5113,
  capability_unknown 5114.
- events 5120-5124: event_invalid 5120.

## Consequences

- WO-037 edits `engine/core/include/sl/core/error.hpp`, `engine/core/src/error.cpp` and `tests/unit/error_test.cpp`;
  it is the only Volume 04 work order allowed to touch sl_core.
- SL-ARCH-001 repeats this table in an `sl-data` block; the conformance work order (WO-062) compares that block with a
  dump of the compiled enum and with the committed contract lock, so the ADR, the document and the code cannot drift.
- Storage code wraps `std::error_code` from the platform as `store_io` with the errno text in the message (ADR-0009,
  Consequences: throwers are wrapped at the boundary).

## Alternatives

- One code per rule id (about 120 codes): rejected, rule ids already carry the detail and would exhaust sub-ranges.
- Reusing the asset range 4000-4999 for corruption: rejected, the Fabric and the asset pipeline (Volume 10) are
  different owners under master plan section 2.2 and F2 forbids sharing a namespace of facts.
- Adding codes as each work order needs them: rejected, parallel work orders would edit the same enum and renumber
  by accident.
