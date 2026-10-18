"""sldeps supply-chain rules (WO-032, SL-GOV-008 C08 and C11, master plan episodes 015 and 058): licences, upstream
attestation and the vulnerability-findings register. Stdlib only, no subprocess, no network (ADR-0004).

Files: deps/vulnerabilities.json (sl-vulns/1, filled by the founder from OSV/NVD), deps/attestations.json (sl-attest/1,
written by `sldeps attest`), deps/attestation-waivers.json (sl-attest-waivers/1, written by the founder).

Rules (each failure line contains the rule id):
  R-DEPS-LICENSE       a lock licence is not an SPDX id on the lock's allowlist (C08)
  R-DEPS-LICFILE       a vendored package has no LICENSE or LICENSE.txt in its hash-manifested files
  R-DEPS-ATTEST        a vendored package is not verified_upstream and has no valid waiver, or a file is malformed
  R-DEPS-ATTEST-STALE  verified_upstream is true but deps/attestations.json lacks it or no longer matches the lock
  R-DEPS-VULN          an open finding names a locked version, or the register is malformed
"""

import datetime
import hashlib
import os
import re
import sys
import tarfile
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "governance"))
import slib  # noqa: E402

LOCK_REL = "deps/deps.lock.json"
VULNS_REL = "deps/vulnerabilities.json"
ATTEST_REL = "deps/attestations.json"
WAIVERS_REL = "deps/attestation-waivers.json"
VULNS_SCHEMA = "sl-vulns/1"
ATTEST_SCHEMA = "sl-attest/1"
WAIVERS_SCHEMA = "sl-attest-waivers/1"
LICENSE_FILES = ("LICENSE", "LICENSE.txt")
SPDX_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9.+-]*$")
DATE = re.compile(r"^[0-9]{4}-[0-9]{2}-[0-9]{2}$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")
STATUSES = ("open", "fixed", "wontfix", "not-affected")
# A phrase every genuine text of the licence contains: a LICENSE file that lacks it is a placeholder.
SNIFF = {
    "MIT": "Permission is hereby granted, free of charge",
    "Apache-2.0": "Apache License",
    "BSL-1.0": "Boost Software License",
    "Zlib": "This software is provided 'as-is'",
    "ISC": "Permission to use, copy, modify, and/or distribute",
}


def _finding(msg, rule, rel):
    return "%s:1: %s: %s" % (rel, rule, msg)


def _shaped(doc, schema, key, kind):
    return isinstance(doc, dict) and doc.get("schema") == schema and isinstance(doc.get(key), kind)


def _read(root, rel):
    return os.path.join(root, *rel.split("/"))


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def today():
    return datetime.date.today().isoformat()


def valid_date(text):
    if not isinstance(text, str) or not DATE.match(text):
        return False
    try:
        datetime.date.fromisoformat(text)
    except ValueError:
        return False
    return True


def load_optional(root, rel):
    """(document, None) or (None, error text); a missing file is an error here, callers decide what it means."""
    try:
        return slib.load_json(_read(root, rel)), None
    except slib.LoadError as exc:
        return None, str(exc)


def vendored(lock):
    """{package: vendored provider} for every package whose lock lists vendored files."""
    out = {}
    for pname in sorted(lock["packages"]):
        prov = lock["packages"][pname]["providers"].get("vendored")
        if prov and prov["files"]:
            out[pname] = prov
    return out


# --- licences -----------------------------------------------------------------------------------


def licenses(root, lock):
    """Every package licence is an SPDX id on the lock's allowlist; every vendored package has a hash-manifested
    LICENSE or LICENSE.txt that carries the licence text. Returns findings."""
    errors = []
    allowed = set(lock["policy"]["licenses_allowed"])
    for pname in sorted(lock["packages"]):
        lic = lock["packages"][pname]["license"]
        if not SPDX_ID.match(lic):
            errors.append(
                _finding("%s: %r is not a plain SPDX licence id" % (pname, lic), "R-DEPS-LICENSE", LOCK_REL)
            )
        elif lic not in allowed:
            errors.append(
                _finding("%s: licence %s is not on the allowlist" % (pname, lic), "R-DEPS-LICENSE", LOCK_REL)
            )
    for pname, prov in vendored(lock).items():
        lic = lock["packages"][pname]["license"]
        names = [n for n in LICENSE_FILES if n in prov["files"]]
        if not names:
            errors.append(
                _finding("%s: no LICENSE or LICENSE.txt in its manifested files" % pname, "R-DEPS-LICFILE", prov["dir"])
            )
            continue
        rel = "%s/%s" % (prov["dir"], names[0])
        try:
            with open(_read(root, rel), encoding="utf-8") as fh:
                text = " ".join(fh.read().split())
        except (OSError, UnicodeDecodeError) as exc:
            errors.append(_finding("%s: cannot read the licence file (%s)" % (pname, exc), "R-DEPS-LICFILE", rel))
            continue
        if lic in SNIFF and SNIFF[lic] not in text:
            errors.append(_finding("%s: does not look like the %s text" % (pname, lic), "R-DEPS-LICFILE", rel))
    return errors


# --- vulnerability register ---------------------------------------------------------------------


def locked_versions(pkg):
    """The versions a package is locked at: its version and the tested version of its system provider."""
    out = set()
    if pkg["version"]:
        out.add(pkg["version"])
    tested = pkg["providers"].get("system", {}).get("tested")
    if tested:
        out.add(tested)
    return out


def vuln_check(root, lock):
    """An open finding against a locked version fails. A finding whose version is '*' names every version."""
    doc, err = load_optional(root, VULNS_REL)
    if err:
        return [_finding(err, "R-DEPS-VULN", VULNS_REL)]
    errors = []
    if not isinstance(doc, dict) or doc.get("schema") != VULNS_SCHEMA or not isinstance(doc.get("findings"), list):
        return [_finding('want {"schema": "%s", "findings": [...]}' % VULNS_SCHEMA, "R-DEPS-VULN", VULNS_REL)]
    for i, fnd in enumerate(doc["findings"]):
        where = "findings[%d]" % i
        if not isinstance(fnd, dict):
            errors.append(_finding("%s is not an object" % where, "R-DEPS-VULN", VULNS_REL))
            continue
        bad = [k for k in ("package", "version", "advisory", "source") if not isinstance(fnd.get(k), str) or not fnd[k]]
        if bad:
            errors.append(_finding("%s lacks %s" % (where, ", ".join(bad)), "R-DEPS-VULN", VULNS_REL))
            continue
        if fnd.get("status") not in STATUSES:
            errors.append(
                _finding("%s: status must be one of %s" % (where, ", ".join(STATUSES)), "R-DEPS-VULN", VULNS_REL)
            )
            continue
        if not valid_date(fnd.get("date")):
            errors.append(_finding("%s: date must be YYYY-MM-DD" % where, "R-DEPS-VULN", VULNS_REL))
            continue
        pkg = lock["packages"].get(fnd["package"])
        if fnd["status"] != "open" or pkg is None:
            continue
        if fnd["version"] == "*" or fnd["version"] in locked_versions(pkg):
            msg = "%s %s has open advisory %s (%s); update the lock, or set status fixed, wontfix or not-affected"
            text = msg % (fnd["package"], fnd["version"], fnd["advisory"], fnd["source"])
            errors.append(_finding(text, "R-DEPS-VULN", VULNS_REL))
    return errors


# --- attestation --------------------------------------------------------------------------------


def _archive_members(archive):
    """(format, {member name: bytes}) of the regular files of a tar or zip archive, or (raw, None).
    Members are read in memory; nothing is extracted to disk."""
    if tarfile.is_tarfile(archive):
        out = {}
        with tarfile.open(archive, "r:*") as tf:
            for member in tf:
                if member.isfile():
                    out[member.name] = member.size
        return "tar", out
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as zf:
            return "zip", {n: 0 for n in zf.namelist() if not n.endswith("/")}
    return "raw", None


def _member_bytes(archive, kind, name):
    if kind == "tar":
        with tarfile.open(archive, "r:*") as tf:
            return tf.extractfile(name).read()
    with zipfile.ZipFile(archive) as zf:
        return zf.read(name)


def _resolve(members, rel):
    """The archive member for a vendored path: exact, or under the archive's single top-level directory."""
    if rel in members:
        return rel
    tops = {n.split("/", 1)[0] for n in members if "/" in n}
    for top in sorted(tops):
        if "%s/%s" % (top, rel) in members:
            return "%s/%s" % (top, rel)
    return None


def attest_record(pkg, prov, archive):
    """Compare the vendored files of PKG byte for byte with the same paths inside ARCHIVE (a tar or zip release
    archive, or a bare file for a single-file release asset). Returns (record, problems). A LICENSE file the
    archive does not carry is a local transcript of the header and is skipped; everything else must match."""
    problems, compared = [], {}
    with open(archive, "rb") as fh:
        archive_hash = sha256_bytes(fh.read())
    kind, members = _archive_members(archive)
    for rel, want in sorted(prov["files"].items()):
        is_license = rel in LICENSE_FILES
        if kind == "raw":
            if is_license:
                continue
            if rel != prov.get("file", rel):
                problems.append("%s: a bare file can only attest %s, not %s" % (pkg, prov.get("file"), rel))
                continue
            with open(archive, "rb") as fh:
                have = sha256_bytes(fh.read())
        else:
            name = _resolve(members, rel)
            if name is None:
                if not is_license:
                    problems.append("%s: %s is not in the archive" % (pkg, rel))
                continue
            have = sha256_bytes(_member_bytes(archive, kind, name))
        if have != want:
            problems.append("%s: %s differs from upstream (vendored %s, upstream %s)" % (pkg, rel, want, have))
        else:
            compared[rel] = want
    if not compared and not problems:
        problems.append("%s: no vendored file could be compared with the archive" % pkg)
    record = {"upstream_archive_sha256": archive_hash, "archive_format": kind, "files": compared, "attested": today()}
    return record, problems


def attest_check(root, lock, on=None):
    """A vendored package must be verified_upstream with a matching record in deps/attestations.json, or be named in
    deps/attestation-waivers.json with a reason and an unexpired date (YYYY-MM-DD, inclusive). Returns findings."""
    on = on or today()
    errors = []
    attested, err = load_optional(root, ATTEST_REL)
    records = {}
    if err is None:
        if not _shaped(attested, ATTEST_SCHEMA, "packages", dict):
            errors.append(
                _finding('want {"schema": "%s", "packages": {...}}' % ATTEST_SCHEMA, "R-DEPS-ATTEST", ATTEST_REL)
            )
        else:
            records = attested["packages"]
    waivers, werr = load_optional(root, WAIVERS_REL)
    waived = {}
    if werr is None:
        if not _shaped(waivers, WAIVERS_SCHEMA, "waivers", list):
            errors.append(
                _finding('want {"schema": "%s", "waivers": [...]}' % WAIVERS_SCHEMA, "R-DEPS-ATTEST", WAIVERS_REL)
            )
        else:
            for w in waivers["waivers"]:
                if isinstance(w, dict) and isinstance(w.get("package"), str):
                    waived[w["package"]] = w
    for pname, prov in vendored(lock).items():
        if prov["verified_upstream"]:
            errors.extend(_stale(pname, prov, records.get(pname)))
            continue
        errors.extend(_waiver_findings(pname, waived.get(pname), on))
    return errors


def _stale(pname, prov, rec):
    msg = None
    if not isinstance(rec, dict) or not SHA256.match(str(rec.get("upstream_archive_sha256", ""))):
        msg = "verified_upstream is true but %s has no attestation record for it; run `sldeps attest`" % ATTEST_REL
    elif not isinstance(rec.get("files"), dict) or not rec["files"]:
        msg = "its attestation record lists no files; run `sldeps attest`"
    else:
        for rel, want in sorted(rec["files"].items()):
            if prov["files"].get(rel) != want:
                msg = "%s changed since it was attested; run `sldeps attest`" % rel
                break
    return [_finding("%s: %s" % (pname, msg), "R-DEPS-ATTEST-STALE", ATTEST_REL)] if msg else []


def _waiver_findings(pname, waiver, on):
    head = "%s is vendored but not verified_upstream" % pname
    if waiver is None:
        return [
            _finding("%s and has no waiver; attest it or add one" % head, "R-DEPS-ATTEST", WAIVERS_REL)
        ]
    reason, expires = waiver.get("reason"), waiver.get("expires")
    if not isinstance(reason, str) or not reason.strip():
        return [_finding("%s; its waiver has no reason" % head, "R-DEPS-ATTEST", WAIVERS_REL)]
    if not valid_date(expires):
        return [_finding("%s; its waiver needs expires as YYYY-MM-DD" % head, "R-DEPS-ATTEST", WAIVERS_REL)]
    if expires < on:
        return [_finding("%s; its waiver expired on %s" % (head, expires), "R-DEPS-ATTEST", WAIVERS_REL)]
    return []


def attestations_doc(root, pkg, record):
    """The new content of deps/attestations.json with PKG's record set."""
    doc, err = load_optional(root, ATTEST_REL)
    if err or not isinstance(doc, dict) or not isinstance(doc.get("packages"), dict):
        doc = {"schema": ATTEST_SCHEMA, "packages": {}}
    doc["packages"][pkg] = record
    doc["packages"] = {k: doc["packages"][k] for k in sorted(doc["packages"])}
    return doc

