# Dependency and supply-chain operations

Seed for SL-OPS-002 (Dependency and Supply-Chain Operations, Planned). Master plan episodes SL-EP-015 (dependency
policy) and SL-EP-058 (secure dependency ingestion: licences, checksums, vulnerability findings); policy in
[SL-GOV-008](../gov/SL-GOV-008-dependency-policy.md) (contracts C01..C12, C08 and C11 here); decisions
[ADR-0002](../adr/ADR-0002-dependency-policy.md) (one policy, one lock, four providers) and
[ADR-0004](../adr/ADR-0004-tooling-stdlib-only.md) (stdlib-only tools); work order WO-032.

## What is enforced where

| Rule | Command | Gate | Fails when |
| :---- | :---- | :---- | :---- |
| `R-DEPS-LICENSE` | `sldeps licenses` | `just deps-licenses` | A lock licence is not a plain SPDX id on the allowlist in the lock's `policy` block (SL-GOV-008 sl-data, C08) |
| `R-DEPS-LICFILE` | `sldeps licenses` | `just deps-licenses` | A vendored dir has no `LICENSE` or `LICENSE.txt` among its hash-manifested files, or the file lacks the licence text |
| `R-DEPS-ATTEST` | `sldeps attest-check` | `just deps-attest-check` | A vendored package is not `verified_upstream` and has no unexpired waiver |
| `R-DEPS-ATTEST-STALE` | `sldeps attest-check` | `just deps-attest-check` | `verified_upstream` is true but `deps/attestations.json` has no record, or a vendored hash changed since |
| `R-DEPS-VULN` | `sldeps vuln-check` | `just deps-check` | An open finding in `deps/vulnerabilities.json` names a locked version, or the register is malformed |
| negative tests | | `just neg-deps-license` | Plants each failure in a copy and asserts the rule id and the package name |

`just deps-check` (and so `check`) runs the vulnerability rule beside the lock, hash and coverage rules. The licence and
attestation gates are separate recipes. No gate touches the network (SL-GOV-008 GOV8-R11).

## Files

| File | Written by | Content |
| :---- | :---- | :---- |
| `deps/deps.lock.json` | `sldeps lock`, `sldeps attest` | Licence (an SPDX id), vendored hashes, `verified_upstream` |
| `deps/attestations.json` | `sldeps attest` | Per package: `upstream_archive_sha256`, archive format, the files compared with their sha256, date (`sl-attest/1`). The lock schema has no member for the archive hash, so it lives here |
| `deps/attestation-waivers.json` | founder | `{"schema": "sl-attest-waivers/1", "waivers": [{"package", "reason", "expires"}]}`; `expires` is YYYY-MM-DD, inclusive; empty while every vendored package is attested |
| `deps/vulnerabilities.json` | founder | `{"schema": "sl-vulns/1", "findings": [{"package", "version", "advisory", "status", "source", "date"}]}`; `status` is `open`, `fixed`, `wontfix` or `not-affected`; `version` may be `*`; starts empty |
| `third_party/<dir>/LICENSE.txt` | founder, at ingestion | The upstream licence text, listed in the lock and in `MANIFEST.json` |

## Ingesting a vendored package (founder)

1. Add the package to `deps/deps.json` with its SPDX `license`; the id must be on the allowlist, else the policy changes
   first (SL-GOV-008 is edited by the founder, ADR-0002).
2. Download the release archive: `SL_DEPS_ALLOW_NETWORK=1 bash tools/deps/fetch_archive.sh <https url> build/deps-attest/<file>`.
   It is the only fetch step; agents never run it (GOV8-R11).
3. Copy the files into `third_party/<dir>`, including the upstream `LICENSE` or `LICENSE.txt` (when the project ships no
   licence file, write one from the licence header and say so in the commit message).
4. `just deps-lock` recomputes hashes and the `MANIFEST.json` files offline.
5. `python3 -I tools/deps/sldeps attest --from-archive <pkg> build/deps-attest/<file>` compares every vendored file byte
   for byte with the same path in the archive (a tar or zip, or a bare release file) and, on a match, sets
   `verified_upstream` and records the archive sha256. A licence file the archive does not carry is skipped.
6. `just deps-licenses`, `just deps-attest-check`, `just deps-check --strict`, then `just deps-register` for the view
   `registry/views/SL-REG-005-dependencies.md`.

If step 5 cannot run, add a waiver with a reason and an expiry; `just deps-attest-check` fails once it expires.

## Vulnerabilities and refresh (founder, C11)

Monthly and at once for a published CVE (GOV8-R15): read the OSV or NVD entries for each locked version by hand, add a
finding with `status: open`, and the next `just deps-check` fails until the lock moves to a fixed version
(`just deps-lock-refresh <pkg>`, `just deps-lock`) or the finding is closed with a status and the advisory id kept.
No automated feed is configured; SL-GOV-008 lists the feed as an open issue.

## Who runs what

| Who | Runs |
| :---- | :---- |
| Agents and CI | `deps-check`, `deps-licenses`, `deps-attest-check`, `neg-deps`, `neg-deps-license`: offline, read-only |
| Founder | `fetch_archive.sh`, `deps-lock-refresh`, `sldeps attest`, waivers and findings edits, the monthly refresh |

## State

Both vendored packages are attested against their upstream release assets (see `deps/attestations.json` for the archive
hashes). doctest's vendored header came from the Arch package; the attestation shows it equals the v2.5.3 upstream file,
so the manifest keeps `provenance: arch-repackage` as history. The vcpkg baseline stays `null` (GOV8-R16).
