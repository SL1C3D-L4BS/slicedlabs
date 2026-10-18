"""docs-check: links and ids of the documentation tree (WO-031).

Rules (each failure line contains the rule id):
  R-DOCS-LINK  a relative Markdown link in docs/**/*.md, README.md, **/README.md or build/docs/*.md does not
               resolve to an existing file or directory, or its #fragment is not a heading of the target
  R-DOCS-ID    an id cited under docs/ does not exist: SL-GOV/PLAN/REG/ARCH/OPS-nnn and SL-VOL-nn-SPEC must be in
               SL-REG-001, SL-EP-nnn in SL-REG-002, ADR-nnnn must be a file in docs/adr

Skipped: docs/plan/SL-PLAN-001-master-plan.md (verbatim copy), ids in docs/templates (placeholders), fenced code blocks,
inline code spans (links only), external links. Known third-party breakage is listed in tools/docs/allow-links.txt as
`<file>|<target>|<reason>` lines (a reason is required; a stale entry is itself a failure).

Usage: python3 -I tools/docs/check.py [--root DIR]. Stdlib only (ADR-0004).
"""

import argparse
import json
import os
import posixpath
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mdlib  # noqa: E402

ID_EXEMPT = ("docs/templates/",)  # placeholder ids such as SL-GOV-000 and ADR-0000
SKIP = {"docs/plan/SL-PLAN-001-master-plan.md"}
ID_RES = (
    re.compile(r"\bSL-(?:GOV|PLAN|REG|ARCH|OPS)-\d{3}\b"),
    re.compile(r"\bSL-VOL-\d{2}-SPEC\b"),
)
EP_RE = re.compile(r"\bSL-EP-\d{3}\b")
ADR_RE = re.compile(r"\bADR-\d{4}\b")
ALLOW_REL = "tools/docs/allow-links.txt"


def md_files(root):
    found = []
    top = os.path.join(root, "README.md")
    if os.path.isfile(top):
        found.append("README.md")
    for base in ("docs", "build/docs"):
        for dp, dns, fns in os.walk(os.path.join(root, *base.split("/"))):
            dns.sort()
            if posixpath.relpath(dp.replace(os.sep, "/"), root.replace(os.sep, "/")) == "build/docs/api":
                dns[:] = []
                continue
            for fn in sorted(fns):
                if fn.endswith(".md"):
                    found.append(os.path.relpath(os.path.join(dp, fn), root).replace(os.sep, "/"))
    return sorted(set(found) - SKIP)


def load_allow(root):
    allow, errors = {}, []
    path = os.path.join(root, *ALLOW_REL.split("/"))
    if not os.path.isfile(path):
        return allow, errors
    for n, line in enumerate(mdlib.read(path).split("\n"), 1):
        if not line.strip() or line.startswith("#"):
            continue
        parts = [p.strip() for p in line.split("|", 2)]
        if len(parts) != 3 or not all(parts):
            errors.append("%s:%d: R-DOCS-LINK: allow entry needs <file>|<target>|<reason>" % (ALLOW_REL, n))
            continue
        allow[(parts[0], parts[1])] = n
    return allow, errors


def registry_ids(root):
    def load(rel):
        with open(os.path.join(root, *rel.split("/")), encoding="utf-8") as fh:
            return json.load(fh)

    docs = {d["id"] for d in load("registry/SL-REG-001-documents.json")["documents"]}
    eps = {e["id"] for e in load("registry/SL-REG-002-episodes.json")["episodes"]}
    adr_dir = os.path.join(root, "docs", "adr")
    adrs = {re.match(r"(ADR-\d{4})", n).group(1) for n in os.listdir(adr_dir) if re.match(r"ADR-\d{4}-.*\.md$", n)}
    return docs, eps, adrs


_anchor_cache = {}


def target_anchors(root, rel):
    if rel not in _anchor_cache:
        _anchor_cache[rel] = mdlib.anchors(mdlib.read(os.path.join(root, *rel.split("/"))))
    return _anchor_cache[rel]


def check_links(root, files, allow, used):
    errors = []
    for rel in files:
        text = mdlib.read(os.path.join(root, *rel.split("/")))
        here = posixpath.dirname(rel)
        for n, target in mdlib.links(text):
            if not target or mdlib.is_external(target):
                continue
            path, _, frag = target.partition("#")
            path = path.split("?", 1)[0]
            if (rel, target) in allow:
                used.add((rel, target))
                continue
            if path.startswith("/"):
                errors.append("%s:%d: R-DOCS-LINK: absolute link %s" % (rel, n, target))
                continue
            dest = posixpath.normpath(posixpath.join(here, path)) if path else rel
            full = os.path.join(root, *dest.split("/"))
            if dest.startswith("..") or not os.path.exists(full):
                errors.append("%s:%d: R-DOCS-LINK: %s does not exist" % (rel, n, target))
                continue
            if frag and os.path.isfile(full) and dest.endswith(".md"):
                if frag.lower() not in target_anchors(root, dest):
                    errors.append("%s:%d: R-DOCS-LINK: %s has no heading #%s" % (rel, n, dest, frag))
    return errors


def check_ids(root, files, docs, eps, adrs):
    errors = []
    for rel in files:
        if not rel.startswith("docs/") or rel.startswith(ID_EXEMPT):
            continue
        text = mdlib.read(os.path.join(root, *rel.split("/")))
        for n, line in mdlib.lines_outside_fences(text):
            for rx in ID_RES:
                for m in rx.finditer(line):
                    if m.group(0) not in docs:
                        errors.append("%s:%d: R-DOCS-ID: %s is not in SL-REG-001" % (rel, n, m.group(0)))
            for m in EP_RE.finditer(line):
                if m.group(0) not in eps:
                    errors.append("%s:%d: R-DOCS-ID: %s is not in SL-REG-002" % (rel, n, m.group(0)))
            for m in ADR_RE.finditer(line):
                if m.group(0) not in adrs:
                    errors.append("%s:%d: R-DOCS-ID: %s is not in docs/adr" % (rel, n, m.group(0)))
    return errors


def run(root):
    files = md_files(root)
    allow, errors = load_allow(root)
    used = set()
    errors += check_links(root, files, allow, used)
    for key, n in sorted(allow.items(), key=lambda kv: kv[1]):
        if key not in used:
            errors.append("%s:%d: R-DOCS-LINK: stale allow entry %s|%s" % (ALLOW_REL, n, key[0], key[1]))
    docs, eps, adrs = registry_ids(root)
    errors += check_ids(root, files, docs, eps, adrs)
    return files, errors


def main(argv=None):
    ap = argparse.ArgumentParser(prog="check.py")
    ap.add_argument("--root", default=mdlib.repo_root())
    args = ap.parse_args(argv)
    try:
        files, errors = run(args.root)
    except (OSError, ValueError, KeyError) as exc:
        print("docs-check: FAIL: %s" % exc)
        return 1
    if errors:
        for e in errors:
            print("docs-check: FAIL: %s" % e)
        print("docs-check: %d finding(s) in %d file(s)" % (len(errors), len(files)))
        return 1
    print("docs-check: ok (%d files)" % len(files))
    return 0


if __name__ == "__main__":
    sys.exit(main())
