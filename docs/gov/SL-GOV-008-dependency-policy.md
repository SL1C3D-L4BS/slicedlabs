---
id: SL-GOV-008
kind: GOV
title: "Build-vs-Adopt and Dependency Policy"
status: Draft
version: 0.1.0
owner: founder
episodes: [014, 015, 048, 058]
depends_on: []
supersedes: []
decisions: [ADR-0002, ADR-0004]
max_lines: 200
---
# Build-vs-Adopt and Dependency Policy

## Purpose

SL-GOV-008 is the one human-readable dependency policy of the monorepo. It records when Sliced Labs builds and when it
adopts (master plan episode 014, "proprietary advantages vs mature infrastructure"), and the contracts C01..C12 that
`deps/deps.lock.json` and its tools enforce (master plan episodes 015, 048 and 058; ADR-0002). The machine-readable
policy is the `sl-data` block under Normative text; the lockfile carries a copy of it and `just gov008-lock-check`
compares the two.

## Status and decisions

Draft, owner founder, approver founder. ADR-0002 binds the one-policy, one-lock, four-provider decision and ADR-0004
binds the stdlib-only tooling. The lockfile, `sl_require` and the providers arrive in WO-017, WO-018 and WO-019
(ADR-0002, Consequences); until then the recipes that enforce C02 to C12 are stubs that exit 3, and the contracts
are specified, not yet enforced.

## Normative text

- GOV8-R01: A capability MUST be built in house when it carries a proprietary advantage of the product and MUST be adopted when mature infrastructure exists (master plan episode 014).
- GOV8-R02: Adopted middleware SHOULD sit behind an adapter, as the Physics row does (master plan section 1.5; section 1.3 on middleware behind adapters).
- GOV8-R03: A choice locked in section 1.5 MUST NOT be reopened without an ADR (master plan section 6.3, rule 3).
- GOV8-R04: A new dependency MUST be judged on licensing, security, support, reproducibility and supply chain (master plan episode 015).
- GOV8-R05: There MUST be one policy, this document, and one lockfile `deps/deps.lock.json` (ADR-0002, Decision).
- GOV8-R06: Four providers (vendored, system, cpm, vcpkg) MUST resolve a package through `sl_require(<pkg>)` to the target `sl::<pkg>` (ADR-0002, Decision).
- GOV8-R07: Tier A packages MUST be content-addressed: vendored files by sha256, cpm by `url_hash` or `git_commit`, vcpkg by baseline plus override, system by an `accept_sha256` of the header (master plan episode 048, "lockfiles, manifests"; ADR-0002).
- GOV8-R08: Tier B packages MUST be range-pinned with a minimum and a tested version; Vulkan, slangc, SDL3 and the compilers are tier B (master plan episode 048; engine pins table of the toolchain spec).
- GOV8-R09: CI presets MUST resolve vendored, cpm, system, vcpkg in that order and verify hashes; dev presets SHOULD resolve system, vendored, cpm, vcpkg (ADR-0002, Decision).
- GOV8-R10: An integrity failure (hash mismatch) MUST be fatal and MUST NOT fall through to the next provider (ADR-0002, Consequences).
- GOV8-R11: Agents MUST NOT fetch or install anything; networked recipes are founder-only (ADR-0002, Decision; master plan section 6.3, rule 4).
- GOV8-R12: Every dependency licence MUST be on the allowlist in the `sl-data` block (master plan episode 058, "licenses, checksums, vulnerability findings").
- GOV8-R13: Every file under `third_party/` MUST be listed in a manifest generated from the lock (master plan episode 048; ADR-0002).
- GOV8-R14: The lock MUST contain only packages that a target consumes (master plan episode 048; WO-017 scope note, no early pins).
- GOV8-R15: Locks SHOULD be refreshed monthly and at once for a published CVE, by the founder (master plan episodes 048 and 058, update process and vulnerability findings).
- GOV8-R16: vcpkg MUST remain an optional provider the founder installs; the committed baseline stays `null` and `unpinned` until the founder pins it, and episode 048 records that (ADR-0002, Decision).

```sl-data
{
  "contracts": [
    {"id": "C01", "text": "One policy (SL-GOV-008) and one lockfile deps/deps.lock.json; the lock carries a copy of the policy block.", "enforced_by": "just gov008-lock-check", "episodes": [15, 48]},
    {"id": "C02", "text": "Four providers resolve a package through sl_require(<pkg>) to the target sl::<pkg>.", "enforced_by": "just deps-provider-test", "episodes": [47, 48]},
    {"id": "C03", "text": "Tier A is content-addressed: vendored sha256, cpm url_hash or git_commit, vcpkg baseline plus override, system header accept_sha256.", "enforced_by": "just deps-check", "episodes": [48, 58]},
    {"id": "C04", "text": "Tier B is range-pinned with min and tested versions: Vulkan, slangc, SDL3, compilers.", "enforced_by": "just deps-check", "episodes": [45, 48]},
    {"id": "C05", "text": "CI resolves vendored, cpm, system, vcpkg and hash-verifies; dev resolves system, vendored, cpm, vcpkg.", "enforced_by": "just deps-provider-test", "episodes": [47, 48]},
    {"id": "C06", "text": "An integrity failure is fatal and never falls through to the next provider.", "enforced_by": "just neg-deps", "episodes": [48, 58]},
    {"id": "C07", "text": "Agents never fetch or install; networked recipes are founder-only and outside check.", "enforced_by": "just gates-check", "episodes": [15, 48]},
    {"id": "C08", "text": "Every dependency licence is on the allowlist.", "enforced_by": "just deps-check", "episodes": [15, 58]},
    {"id": "C09", "text": "Every file under third_party is listed in a manifest generated from the lock.", "enforced_by": "just deps-check", "episodes": [48, 58]},
    {"id": "C10", "text": "Only packages that a target consumes are locked.", "enforced_by": "just deps-check", "episodes": [48]},
    {"id": "C11", "text": "Locks are refreshed monthly or on a published CVE, by the founder.", "enforced_by": "pending", "episodes": [48, 58]},
    {"id": "C12", "text": "vcpkg is optional and founder-installed; its baseline stays null and unpinned until the founder pins it, recorded honestly.", "enforced_by": "just neg-deps-unpinned-provider", "episodes": [48]}
  ],
  "providers": ["vendored", "system", "cpm", "vcpkg"],
  "provider_order": {
    "ci": ["vendored", "cpm", "system", "vcpkg"],
    "dev": ["system", "vendored", "cpm", "vcpkg"]
  },
  "tiers": {
    "A": "content-addressed: vendored sha256, cpm url_hash or git_commit, vcpkg baseline plus override, system header accept_sha256",
    "B": "range-pinned: minimum and tested version (Vulkan, slangc, SDL3, compilers)"
  },
  "licenses_allowed": ["MIT", "BSD-2-Clause", "BSD-3-Clause", "Zlib", "BSL-1.0", "Apache-2.0", "MPL-2.0", "ISC", "Unlicense"]
}
```

## Measures and enforcement points

- C01 to C12 above name the enforcing recipe or `pending`; `just policy-contracts-check` asserts the block parses, ids are unique and contiguous, every `enforced_by` is `pending` or a recipe in the justfile, and each provider order is a permutation of the providers.
- C01: `just gov008-lock-check` compares providers, provider_order and licenses_allowed with the lock `policy` block (numerator: differing keys, denominator: three, threshold 0). It passes when the lock does not exist yet.
- C02 to C06, C08 to C10 and C12: `deps-check`, `deps-provider-test`, `neg-deps` and `neg-deps-unpinned-provider` are owned by WO-017 and WO-018 and are stubs until those land.
- C07: `just gates-check` scans recipes for network fetches (tools/governance/slgov.py, FORBIDDEN_FETCH).
- C11: calendar review by the founder; no automated enforcement.
- Review of this file: `just doc-check` and `just registry-docs-check`.

## Traceability

Master plan sections 1.3, 1.5 and 6.3; episodes 014 (build-vs-adopt), 015 (dependency policy), 048 (lock dependencies
reproducibly), 058 (secure dependency ingestion); ADR-0002 (policy, lock, providers) and ADR-0004 (stdlib-only tools).
Related work orders: WO-010 (this document), WO-017 (lock and sldeps), WO-018 (`sl_require`), WO-019 (cpm and vcpkg).
Engine pins (SDL3, Vulkan headers, volk, VMA, Dear ImGui, Tracy client, slangc) come from the toolchain spec, engine
pins table; none is locked until a target consumes it.

## Open issues

- The monthly refresh interval is a founder decision in the work order (WO-010), not in the master plan; the plan names only an update process (episode 048).
- Which tier-B compilers and floors are pinned is left to WO-017 and `toolchain/environments.json`; this policy names only the tiers.
- vcpkg is not installed on the workstation, so its baseline is `null` and `unpinned` and tier A for vcpkg cannot be verified yet (ADR-0002, Context).
- The enforcing recipes for C02 to C06, C08 to C10 and C12 are stubs until WO-017 to WO-019 land; C11 is `pending` by design.
- The vulnerability source for CVE-driven refresh is not chosen; episode 058 names the finding, not the feed.

## Change log

| Version | Date | Change |
| :---- | :---- | :---- |
| 0.1.0 | 2026-10-17 | First draft: contracts C01..C12, providers, tiers, licence allowlist (WO-010). |
