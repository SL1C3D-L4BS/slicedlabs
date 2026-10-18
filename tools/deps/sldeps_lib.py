"""sldeps library (WO-017, ADR-0002): the dependency manifest, the single lock and what is generated from them.

Files: deps/deps.json (manifest and policy, edited by hand), deps/deps.lock.json (the only place a hash, a tested
version or a baseline is decided), deps/vcpkg/vcpkg.json and vcpkg-configuration.json and
third_party/<dir>/MANIFEST.json (both generated from the lock, drift-checked).

`pending` and `unpinned` are never stored: provider_state() derives them from the lock at check time.

Rules (each failure line contains the rule id):
  R-DEPS-LOAD       a file is missing, unreadable or not JSON
  R-DEPS-SCHEMA     the manifest or the lock violates its schema (schemas/sl-deps*.schema.json)
  R-DEPS-MANIFEST   lock.manifest_sha256 differs from sha256(deps/deps.json), or the lock is not the manifest's
  R-DEPS-POLICY     lock.policy differs from manifest.policy
  R-DEPS-HASH       a vendored file is missing, or its sha256 differs from the lock (never falls through, C06)
  R-DEPS-UNCOVERED  a file under third_party/ is listed in no manifest, or a listed file is gone (C09)
  R-DEPS-GEN        a generated file differs from what sync would write
  R-DEPS-LICENSE    a licence is not in the allowlist (C08)
  R-DEPS-TIER       tier A without a version or tier B without min and tested (C03, C04)
  R-DEPS-USED       a package no target consumes (C10)
  R-DEPS-UNPINNED   --strict: a provider that is first in ci order is unpinned (C03, C12)
  R-DEPS-VULN       an open finding in deps/vulnerabilities.json names a locked version (WO-032, sldeps_supply.py)
The licence, attestation and vulnerability rules live in sldeps_supply.py (`licenses`, `attest-check`, `vuln-check`).

Stdlib only, no subprocess, no network (ADR-0004).
"""

import hashlib
import json
import os
import shutil
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "governance"))
import slib  # noqa: E402
import sldeps_supply as supply  # noqa: E402

ROOT = slib.ROOT
MANIFEST_REL = "deps/deps.json"
LOCK_REL = "deps/deps.lock.json"
VCPKG_REL = "deps/vcpkg/vcpkg.json"
VCPKG_CONFIG_REL = "deps/vcpkg/vcpkg-configuration.json"
THIRD_PARTY = "third_party"
PART_MANIFEST = "MANIFEST.json"
PROVIDERS = ("vendored", "system", "cpm", "vcpkg")
SYSTEM_INCLUDE = "/usr/include"
VCPKG_SCHEMA_URL = "https://raw.githubusercontent.com/microsoft/vcpkg-tool/main/docs/vcpkg.schema.json"

# Keys a package provider gets from the manifest; everything else in a lock provider is a pin.
PIN_KEYS = {
    "system": ("tested", "accept_sha256"),
    "vendored": ("verified_upstream", "files", "tree_sha256"),
    "cpm": ("url_hash", "git_commit"),
    "vcpkg": ("version", "baseline"),
}
TOP_LEVEL_EXEMPT = ("README.md",)


class DepsError(ValueError):
    """A precondition of a command failed (not a finding)."""


# --- helpers ----------------------------------------------------------------------------------


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def tree_sha256(files):
    """sha256 of the sorted '<sha256>  <relative path>' lines of a files map; None for an empty map."""
    if not files:
        return None
    text = "".join("%s  %s\n" % (files[rel], rel) for rel in sorted(files))
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def dump_json(data):
    return json.dumps(data, indent=2, ensure_ascii=False) + "\n"


def _path(root, rel):
    return os.path.join(root, *rel.split("/"))


def read_text(root, rel):
    with open(_path(root, rel), encoding="utf-8") as fh:
        return fh.read()


def write_text(root, rel, text):
    path = _path(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


def load(root, rel):
    return slib.load_json(_path(root, rel))


def disk_files(root, vendored_dir):
    """Relative paths (posix) and sha256 of every file under a vendored dir, MANIFEST.json left out."""
    base = _path(root, vendored_dir)
    found = {}
    for dirpath, dirnames, names in os.walk(base):
        dirnames.sort()
        for name in sorted(names):
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, base).replace(os.sep, "/")
            if rel == PART_MANIFEST:
                continue
            found[rel] = sha256_file(full)
    return found


# --- derived state ----------------------------------------------------------------------------


def provider_state(pkg, name, prov):
    """'pinned' or 'unpinned', derived from a lock provider (never stored)."""
    tier = pkg.get("tier")
    if name == "vendored":
        return "pinned" if prov.get("files") else "unpinned"
    if name == "system":
        if tier == "B":
            return "pinned" if prov.get("min") and prov.get("tested") else "unpinned"
        return "pinned" if prov.get("accept_sha256") else "unpinned"
    if name == "cpm":
        return "pinned" if prov.get("url_hash") or prov.get("git_commit") else "unpinned"
    if name == "vcpkg":
        return "pinned" if prov.get("baseline") else "unpinned"
    return "unpinned"


def first_in_order(pkg, order):
    """The first provider of `order` that the package declares, or None."""
    declared = pkg.get("providers", {})
    for name in order:
        if name in declared:
            return name
    return None


# --- building the lock ------------------------------------------------------------------------


def build_lock(root, manifest, manifest_bytes, old_lock, tested=None):
    """The lock the manifest and the disk imply: descriptors from the manifest, pins from the old lock or disk."""
    tested = tested or {}
    old_pkgs = (old_lock or {}).get("packages", {}) if isinstance(old_lock, dict) else {}
    packages = {}
    for pname, pkg in manifest["packages"].items():
        old = old_pkgs.get(pname, {}) if isinstance(old_pkgs.get(pname), dict) else {}
        old_prov = old.get("providers", {}) if isinstance(old.get("providers"), dict) else {}
        entry = {k: pkg[k] for k in ("version", "license", "role", "tier", "used_by", "target")}
        provs = {}
        for name in PROVIDERS:
            if name not in pkg["providers"]:
                continue
            desc = dict(pkg["providers"][name])
            prev = old_prov.get(name, {}) if isinstance(old_prov.get(name), dict) else {}
            if name == "system":
                desc["tested"] = tested.get(pname, prev.get("tested"))
                desc["accept_sha256"] = prev.get("accept_sha256")
                header = None
                if desc.get("header"):
                    header = os.path.join(SYSTEM_INCLUDE, *desc["header"].split("/"))
                if header and os.path.isfile(header):
                    desc["accept_sha256"] = sha256_file(header)
            elif name == "vendored":
                files = disk_files(root, desc["dir"]) if os.path.isdir(_path(root, desc["dir"])) else {}
                desc["verified_upstream"] = bool(prev.get("verified_upstream", False))
                desc["files"] = files
                desc["tree_sha256"] = tree_sha256(files)
            elif name == "cpm":
                desc["url_hash"] = prev.get("url_hash")
                desc["git_commit"] = prev.get("git_commit")
            elif name == "vcpkg":
                desc["version"] = pkg["version"] if pkg["version"] is not None else ""
                desc["baseline"] = prev.get("baseline")
            provs[name] = desc
        entry["providers"] = provs
        packages[pname] = entry
    return {
        "schema": "sl-deps-lock/1",
        "manifest_sha256": sha256_bytes(manifest_bytes),
        "policy": manifest["policy"],
        "packages": packages,
    }


def part_manifests(lock):
    """{relative path: text} of every third_party/<dir>/MANIFEST.json the lock implies (vendored with files)."""
    out = {}
    for pname in sorted(lock["packages"]):
        pkg = lock["packages"][pname]
        prov = pkg["providers"].get("vendored")
        if not prov or not prov["files"]:
            continue
        doc = {
            "schema": "sl-third-party-manifest/1",
            "package": pname,
            "version": pkg["version"],
            "license": pkg["license"],
            "upstream": prov["upstream"],
            "provenance": prov["provenance"],
            "verified_upstream": prov["verified_upstream"],
            "tree_sha256": prov["tree_sha256"],
            "files": prov["files"],
        }
        out["%s/%s" % (prov["dir"], PART_MANIFEST)] = dump_json(doc)
    return out


def vcpkg_files(lock):
    """{relative path: text} for deps/vcpkg. Without a baseline there is no builtin-baseline and no overrides."""
    ports = []
    baselines = set()
    for pname in sorted(lock["packages"]):
        prov = lock["packages"][pname]["providers"].get("vcpkg")
        if prov:
            ports.append((prov["port"], prov["version"]))
            baselines.add(prov["baseline"])
    if len(baselines) > 1:
        raise DepsError("vcpkg ports carry different baselines; vcpkg has one baseline")
    baseline = baselines.pop() if baselines else None
    doc = {
        "$schema": VCPKG_SCHEMA_URL,
        "name": "slicedlabs-deps",
        "version": "0.0.0",
        "dependencies": [p for p, _ in ports],
    }
    config = {"registries": []}
    if baseline:
        doc["builtin-baseline"] = baseline
        doc["overrides"] = [{"name": p, "version": v} for p, v in ports]
        config = {"default-registry": {"kind": "builtin", "baseline": baseline}, "registries": []}
    return {VCPKG_REL: dump_json(doc), VCPKG_CONFIG_REL: dump_json(config)}


# --- commands that write ----------------------------------------------------------------------


def load_manifest(root):
    path = _path(root, MANIFEST_REL)
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError as exc:
        raise DepsError("%s: %s" % (MANIFEST_REL, exc.strerror or exc)) from exc
    manifest = slib.loads_json(data.decode("utf-8"), MANIFEST_REL)
    errors = slib.validate(manifest, slib.load_schema("sl-deps", os.path.join(root, "schemas")))
    if errors:
        raise DepsError("; ".join("%s: %s" % (p, m) for p, m in errors[:3]))
    return manifest, data


def cmd_lock(root, tested=None):
    """Recompute the lock offline (manifest hash, policy copy, vendored hashes, system header hash) and the
    third_party MANIFEST.json files. Returns the list of relative paths written."""
    manifest, data = load_manifest(root)
    old = None
    if os.path.exists(_path(root, LOCK_REL)):
        try:
            old = load(root, LOCK_REL)
        except slib.LoadError:
            old = None
    lock = build_lock(root, manifest, data, old, tested)
    written = [LOCK_REL]
    write_text(root, LOCK_REL, dump_json(lock))
    for rel, text in sorted(part_manifests(lock).items()):
        write_text(root, rel, text)
        written.append(rel)
    return written


def cmd_sync_vcpkg(root):
    """Write deps/vcpkg/vcpkg.json and vcpkg-configuration.json from the lock."""
    lock = load(root, LOCK_REL)
    written = []
    for rel, text in sorted(vcpkg_files(lock).items()):
        write_text(root, rel, text)
        written.append(rel)
    return written


def cpm_url(root, pkg):
    """The archive URL the cpm provider fetches for PKG: the lock's `url` member when present, else derived from a
    github.com `git` + `tag` (same rule as cmake/deps/provider_cpm.cmake). Nothing is fetched."""
    lock = load(root, LOCK_REL)
    entry = lock["packages"].get(pkg)
    prov = entry["providers"].get("cpm") if entry else None
    if prov is None:
        raise DepsError("cpm-url: '%s' has no cpm provider in %s" % (pkg, LOCK_REL))
    if prov.get("url"):
        return prov["url"]
    git, tag = prov.get("git", ""), prov.get("tag", "")
    if git.startswith("https://github.com/") and tag:
        base = git[: -len(".git")] if git.endswith(".git") else git
        return "%s/archive/refs/tags/%s.tar.gz" % (base, tag)
    raise DepsError("cpm-url: no url for '%s' and %r is not a github.com repository" % (pkg, git))


def cmd_lock_refresh(root, pkg, archive):
    """Pin the cpm provider of PKG: url_hash = sha256 of ARCHIVE, a file the founder recipe already downloaded.
    Nothing is fetched here (no network in Python). Returns the relative paths written."""
    lock = load(root, LOCK_REL)
    entry = lock["packages"].get(pkg)
    if entry is None:
        raise DepsError("lock-refresh: '%s' is not in %s" % (pkg, LOCK_REL))
    prov = entry["providers"].get("cpm")
    if prov is None:
        raise DepsError("lock-refresh: '%s' has no cpm provider in %s" % (pkg, LOCK_REL))
    if not os.path.isfile(archive):
        hint = "SL_DEPS_ALLOW_NETWORK=1 just deps-lock-refresh %s" % pkg
        raise DepsError("lock-refresh: %s is not a file (download it first: `%s`)" % (archive, hint))
    prov["url_hash"] = sha256_file(archive)
    write_text(root, LOCK_REL, dump_json(lock))
    return [LOCK_REL]


def cmd_attest(root, pkg, archive):
    """Compare PKG's vendored files with the upstream ARCHIVE (already downloaded by the founder); on a match set
    verified_upstream true in the lock and record the archive sha256 in deps/attestations.json. Offline.
    Returns the relative paths written."""
    lock = load(root, LOCK_REL)
    entry = lock["packages"].get(pkg)
    prov = entry["providers"].get("vendored") if entry else None
    if prov is None or not prov["files"]:
        raise DepsError("attest: '%s' has no vendored files in %s" % (pkg, LOCK_REL))
    if not os.path.isfile(archive):
        raise DepsError("attest: %s is not a file (download it first with tools/deps/fetch_archive.sh)" % archive)
    record, problems = supply.attest_record(pkg, prov, archive)
    if problems:
        raise DepsError("attest: not verified: " + "; ".join(problems))
    prov["verified_upstream"] = True
    write_text(root, LOCK_REL, dump_json(lock))
    write_text(root, supply.ATTEST_REL, dump_json(supply.attestations_doc(root, pkg, record)))
    written = [LOCK_REL, supply.ATTEST_REL]
    for rel, text in sorted(part_manifests(lock).items()):
        write_text(root, rel, text)
        written.append(rel)
    return written


def cmd_vendor_from_system(root, pkg, include_dir=SYSTEM_INCLUDE):
    """Copy a package's system header into its vendored dir, then recompute the lock. Offline."""
    manifest, _ = load_manifest(root)
    entry = manifest["packages"].get(pkg)
    if entry is None:
        raise DepsError("unknown package %r" % pkg)
    system = entry["providers"].get("system", {})
    vendored = entry["providers"].get("vendored")
    if "header" not in system or vendored is None:
        raise DepsError("package %r needs a system provider with a header and a vendored provider" % pkg)
    source = os.path.join(include_dir, *system["header"].split("/"))
    if not os.path.isfile(source):
        raise DepsError("%s is not installed (offline: nothing is fetched)" % source)
    dest_rel = "%s/%s" % (vendored["dir"], system["header"])
    dest = _path(root, dest_rel)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    shutil.copyfile(source, dest)
    return [dest_rel] + cmd_lock(root)


# --- check ------------------------------------------------------------------------------------


def _regen_command(rel):
    """The sldeps command that rewrites a generated file."""
    return "sync-vcpkg" if rel.startswith("deps/") else "lock"


def _find(msg, rule, rel, extra=""):
    return "%s:1: %s: %s%s" % (rel, rule, msg, extra)


def check(root, strict=False, ci_order=None):
    """Return (errors, notes). errors fail the gate; notes are honest observations (unpinned providers)."""
    errors, notes = [], []
    schema_dir = os.path.join(root, "schemas")
    try:
        with open(_path(root, MANIFEST_REL), "rb") as fh:
            manifest_bytes = fh.read()
        manifest = slib.loads_json(manifest_bytes.decode("utf-8"), MANIFEST_REL)
        lock = load(root, LOCK_REL)
        s_manifest = slib.load_schema("sl-deps", schema_dir)
        s_lock = slib.load_schema("sl-deps-lock", schema_dir)
    except (OSError, UnicodeDecodeError, slib.LoadError) as exc:
        return [_find(str(exc), "R-DEPS-LOAD", MANIFEST_REL)], notes
    for rel, doc, schema in ((MANIFEST_REL, manifest, s_manifest), (LOCK_REL, lock, s_lock)):
        for ptr, msg in slib.validate(doc, schema):
            errors.append(_find("%s %s" % (ptr, msg), "R-DEPS-SCHEMA", rel))
    if errors:
        return errors, notes

    # the lock belongs to this manifest
    if lock["manifest_sha256"] != sha256_bytes(manifest_bytes):
        errors.append(
            _find(
                "manifest_sha256 differs from sha256(%s); run `sldeps lock`" % MANIFEST_REL,
                "R-DEPS-MANIFEST",
                LOCK_REL,
            )
        )
    if lock["policy"] != manifest["policy"]:
        lp, mp = lock["policy"], manifest["policy"]
        keys = sorted(k for k in set(lp) | set(mp) if lp.get(k) != mp.get(k))
        errors.append(
            _find("lock policy differs from the manifest policy in %s" % ", ".join(keys), "R-DEPS-POLICY", LOCK_REL)
        )
    if sorted(lock["packages"]) != sorted(manifest["packages"]):
        errors.append(_find("package set differs from the manifest; run `sldeps lock`", "R-DEPS-MANIFEST", LOCK_REL))
    allowed = set(manifest["policy"]["licenses_allowed"])

    covered = set()
    for pname in sorted(manifest["packages"]):
        mpkg = manifest["packages"][pname]
        lpkg = lock["packages"].get(pname)
        if lpkg is None:
            continue
        where = "%s (package %s)" % (LOCK_REL, pname)
        if mpkg["license"] not in allowed:
            errors.append(
                _find(
                    "%s: licence %r is not on the allowlist" % (pname, mpkg["license"]),
                    "R-DEPS-LICENSE",
                    MANIFEST_REL,
                )
            )
        if not mpkg["used_by"]:
            errors.append(_find("%s: no target consumes it" % pname, "R-DEPS-USED", MANIFEST_REL))
        if mpkg["tier"] == "A" and mpkg["version"] is None:
            errors.append(_find("%s: tier A needs a version" % pname, "R-DEPS-TIER", MANIFEST_REL))
        for k in ("version", "license", "role", "tier", "used_by", "target"):
            if lpkg[k] != mpkg[k]:
                errors.append(
                    _find(
                        "%s.%s differs from the manifest; run `sldeps lock`" % (pname, k),
                        "R-DEPS-MANIFEST",
                        LOCK_REL,
                    )
                )
        if sorted(lpkg["providers"]) != sorted(mpkg["providers"]):
            errors.append(_find("%s: provider set differs from the manifest" % pname, "R-DEPS-MANIFEST", LOCK_REL))
        for name, lprov in lpkg["providers"].items():
            mprov = mpkg["providers"].get(name)
            if mprov is None:
                continue
            desc = {k: v for k, v in lprov.items() if k not in PIN_KEYS[name]}
            if name == "vcpkg":
                if lprov["version"] != (mpkg["version"] or ""):
                    errors.append(
                        _find("%s.vcpkg.version differs from the manifest version" % pname, "R-DEPS-MANIFEST", LOCK_REL)
                    )
            if desc != mprov:
                errors.append(
                    _find(
                        "%s.%s descriptor differs from the manifest; run `sldeps lock`" % (pname, name),
                        "R-DEPS-MANIFEST",
                        LOCK_REL,
                    )
                )
            if name == "system" and mpkg["tier"] == "B" and not lprov["tested"]:
                errors.append(_find("%s: tier B needs a tested version beside min" % pname, "R-DEPS-TIER", LOCK_REL))
            if name == "vendored":
                covered.update(_check_vendored(root, pname, lprov, errors))
    errors.extend(supply.vuln_check(root, lock))
    # C09: every third_party file is covered
    vendored_dirs = {
        p["providers"]["vendored"]["dir"]
        for p in lock["packages"].values()
        if p["providers"].get("vendored", {}).get("files")
    }
    errors.extend(_uncovered(root, covered, vendored_dirs))
    # generated files equal what sync would write
    expected = dict(part_manifests(lock))
    try:
        expected.update(vcpkg_files(lock))
    except DepsError as exc:
        errors.append(_find(str(exc), "R-DEPS-GEN", LOCK_REL))
    for rel in sorted(expected):
        try:
            have = read_text(root, rel)
        except OSError:
            errors.append(_find("missing; run `sldeps %s`" % _regen_command(rel), "R-DEPS-GEN", rel))
            continue
        if have != expected[rel]:
            msg = "differs from what the lock generates; run `sldeps %s`" % _regen_command(rel)
            errors.append(_find(msg, "R-DEPS-GEN", rel))

    # derived state: unpinned providers, honestly reported
    order = ci_order or manifest["policy"]["provider_order"]["ci"]
    for pname in sorted(lock["packages"]):
        pkg = lock["packages"][pname]
        first = first_in_order(pkg, order)
        for name in PROVIDERS:
            prov = pkg["providers"].get(name)
            if prov is None or provider_state(pkg, name, prov) == "pinned":
                continue
            note = "%s via %s is unpinned" % (pname, name)
            if strict and name == first:
                errors.append(
                    _find(
                        "%s, and %s is first in ci order (%s)" % (note, name, ",".join(order)),
                        "R-DEPS-UNPINNED",
                        LOCK_REL,
                    )
                )
            else:
                notes.append(note)
    # system header drift is a note only: the installed header depends on the machine
    for pname in sorted(lock["packages"]):
        sysprov = lock["packages"][pname]["providers"].get("system")
        if sysprov and sysprov.get("accept_sha256") and sysprov.get("header"):
            header = os.path.join(SYSTEM_INCLUDE, *sysprov["header"].split("/"))
            if os.path.isfile(header) and sha256_file(header) != sysprov["accept_sha256"]:
                notes.append("%s: the installed %s differs from accept_sha256" % (pname, header))
    return errors, notes


def _check_vendored(root, pname, prov, errors):
    """Compare a lock vendored provider with the disk; return the covered third_party-relative paths."""
    covered = set()
    base = prov["dir"]
    if prov["tree_sha256"] != tree_sha256(prov["files"]):
        errors.append(
            _find("%s: tree_sha256 does not match its files; run `sldeps lock`" % pname, "R-DEPS-HASH", LOCK_REL)
        )
    if "file" in prov and prov["file"] not in prov["files"] and os.path.isdir(_path(root, base)):
        errors.append(_find("%s: declared file %s is not in the lock" % (pname, prov["file"]), "R-DEPS-HASH", LOCK_REL))
    for rel, want in sorted(prov["files"].items()):
        full = "%s/%s" % (base, rel)
        covered.add(full)
        path = _path(root, full)
        if not os.path.isfile(path):
            errors.append(_find("listed in the lock but missing on disk", "R-DEPS-UNCOVERED", full))
            continue
        have = sha256_file(path)
        if have != want:
            errors.append(_find("sha256 mismatch: lock has %s, disk has %s" % (want, have), "R-DEPS-HASH", full))
    return covered


def _uncovered(root, covered, vendored_dirs):
    """Every file under third_party/ must be a listed vendored file, a generated MANIFEST.json of a
    vendored dir, or the top-level README.md (C09)."""
    errors = []
    base = _path(root, THIRD_PARTY)
    exempt = {"%s/%s" % (d, PART_MANIFEST) for d in vendored_dirs}
    for dirpath, dirnames, names in os.walk(base):
        dirnames.sort()
        for name in sorted(names):
            rel = os.path.relpath(os.path.join(dirpath, name), base).replace(os.sep, "/")
            full = "%s/%s" % (THIRD_PARTY, rel)
            if rel in TOP_LEVEL_EXEMPT or full in covered or full in exempt:
                continue
            errors.append(_find("under third_party/ but listed in no manifest", "R-DEPS-UNCOVERED", full))
    return errors


# --- register view ----------------------------------------------------------------------------


def register_table(lock):
    """A markdown table of the lock for registry/views/SL-REG-005-dependencies.md (derived state, no pins stored)."""
    lines = [
        "| Package | Version | Licence | Role | Tier | Used by | Providers |",
        "| :---- | :---- | :---- | :---- | :---- | :---- | :---- |",
    ]
    for pname in sorted(lock["packages"]):
        pkg = lock["packages"][pname]
        provs = ", ".join(
            "%s (%s)" % (n, provider_state(pkg, n, pkg["providers"][n])) for n in PROVIDERS if n in pkg["providers"]
        )
        lines.append(
            "| %s | %s | %s | %s | %s | %s | %s |"
            % (
                pname,
                pkg["version"] or "range",
                pkg["license"],
                pkg["role"],
                pkg["tier"],
                ", ".join(pkg["used_by"]),
                provs,
            )
        )
    return "\n".join(lines) + "\n"
