"""Generate registry/SL-REG-001-documents.json (the document register) from the master plan.

`python3 -I cmd_gen_docs.py [--root DIR] [--check] [--derived]`

  (no flag)   write registry/SL-REG-001-documents.json
  --check     exit 1 when the committed register differs from the generator output
  --derived   print the derived view (status, version, sha256 read from each document) as JSON

Inputs: the table parse of master plan section "Required documents and registries" (62 named
documents), the 42 volume headings (SL-VOL-nn-SPEC) and registry/vol01-documents.json (the 20 Volume 01
documents and the episode map). The committed register is identity only: status, version and sha256
are static placeholders ("Planned", null, null) so that a document landing never makes it stale. The
real values are derived at check time by derive_view(): front matter of the document file (Draft when
the file is absent) plus registry/SL-REG-001.approvals.json (the only place a document becomes
Approved). Output is deterministic. Stdlib only (ADR-0004).
"""

import hashlib
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slgov  # noqa: E402

PLAN_REL = "docs/plan/SL-PLAN-001-master-plan.md"
VOL01_REL = "registry/vol01-documents.json"
APPROVALS_REL = "registry/SL-REG-001.approvals.json"
REGISTER_REL = "registry/SL-REG-001-documents.json"
SCHEMA_ID = "sl-reg-001/1"
SECTION_HEADING = "## **Required documents and registries**"

KINDS = ("GOV", "ARCH", "PLAN", "OPS", "REG")
NAMED_COUNTS = {"GOV": 10, "ARCH": 30, "PLAN": 5, "OPS": 10, "REG": 7}
VOLUMES = 42
EPISODES_PER_VOLUME = 20
STATUSES = ("Draft", "InReview", "Approved", "Superseded")
# The master plan is a pinned byte copy (SL-PLAN-001-master-plan.sha256); it carries no front matter.
NO_FRONT_MATTER = ("SL-PLAN-001",)

# File-name slugs fixed for the 20 Volume 01 documents (other work orders write exactly these files).
VOL01_SLUGS = {
    "SL-GOV-001": "constitution",
    "SL-GOV-002": "requirements-catalogue",
    "SL-GOV-003": "capability-ontology",
    "SL-GOV-004": "adr-log",
    "SL-GOV-005": "risk-register",
    "SL-GOV-006": "fragmentation-budget",
    "SL-GOV-007": "release-gates",
    "SL-GOV-008": "dependency-policy",
    "SL-GOV-009": "quality-model",
    "SL-GOV-010": "parity-matrix",
    "SL-PLAN-001": "master-plan",
    "SL-PLAN-002": "episode-dependency-graph",
    "SL-PLAN-003": "work-order-protocol",
    "SL-PLAN-004": "vertical-slices",
    "SL-PLAN-005": "kohi-alignment-map",
    "SL-REG-001": "documents",
    "SL-REG-002": "episodes",
    "SL-REG-003": "traceability",
    "SL-REG-004": "capabilities",
    "SL-REG-006": "exceptions",
}
TITLE_OVERRIDES = {"SL-GOV-001": "Platform Constitution and Engineering Doctrine"}
ALIASES = {"SL-GOV-001": ["Engine Constitution"]}
# kinds whose location is decided when the document is written (ADR-0001 names only docs/gov, plan, vol)
UNPLACED_KINDS = ("ARCH", "OPS")
# ARCH documents placed under docs/arch when written (WO-035); the slug is fixed here because a file name need not
# equal slugify(title). Every other ARCH id stays unplaced (path null) until its own work order adds it.
ARCH_SLUGS = {
    "SL-ARCH-001": "canonical-project-model",
    "SL-ARCH-002": "transaction-undo-recovery",
    "SL-ARCH-003": "runtime-execution",  # WO-075 wrote it, WO-102 placed it
}

_ROW = re.compile(r"^\|\s*(SL-(GOV|ARCH|PLAN|OPS|REG)-(\d{3}))\s*\|(.+)\|(.+)\|\s*$")
_VOL_HEADING = re.compile(r"^### \*\*Volume (\d{2}) — (.+?)\*\*\s*$")


class DocsError(ValueError):
    """The master plan or registry/vol01-documents.json does not have the expected shape."""


def unescape(text):
    return re.sub(r"\\([\\`*_{}\[\]()#+.!|-])", r"\1", text).strip()


def slugify(title):
    words = re.sub(r"\([^)]*\)", " ", title.lower())
    words = re.findall(r"[a-z0-9]+", words)
    return "-".join(words[:6])


def volume_episodes(volume):
    first = EPISODES_PER_VOLUME * (volume - 1) + 1
    return ["SL-EP-%03d" % n for n in range(first, first + EPISODES_PER_VOLUME)]


def parse_volumes_written(text):
    """'Vol 11–14' -> [11..14]; 'Vol 01, Vol 26' -> [1, 26]; 'Now, maintained' -> []."""
    if not text.strip().lower().startswith("vol"):
        return []
    found = set()
    for m in re.finditer(r"(\d+)(?:\s*[–-]\s*(\d+))?", text):
        low = int(m.group(1))
        high = int(m.group(2)) if m.group(2) else low
        found.update(range(low, high + 1))
    return sorted(found)


def parse_plan(text):
    """Return (named, volumes): named = [(id, kind, title, written_in_text)], volumes = {n: name}."""
    lines = text.splitlines()
    start = None
    for i, line in enumerate(lines):
        if line.strip() == SECTION_HEADING:
            start = i
            break
    if start is None:
        raise DocsError("%s: section %r not found" % (PLAN_REL, SECTION_HEADING))
    named = []
    for line in lines[start + 1 :]:
        if line.startswith("## "):
            break
        m = _ROW.match(line.strip())
        if m:
            named.append((m.group(1), m.group(2), unescape(m.group(4)), unescape(m.group(5))))
    counts = {k: sum(1 for d in named if d[1] == k) for k in KINDS}
    if counts != NAMED_COUNTS:
        raise DocsError("%s: named documents per kind are %s, expected %s" % (PLAN_REL, counts, NAMED_COUNTS))
    for kind in KINDS:
        numbers = [int(d[0].rsplit("-", 1)[1]) for d in named if d[1] == kind]
        if numbers != list(range(1, NAMED_COUNTS[kind] + 1)):
            raise DocsError("%s: SL-%s numbers are not 001..%03d in order" % (PLAN_REL, kind, NAMED_COUNTS[kind]))
    volumes = {}
    for line in lines:
        m = _VOL_HEADING.match(line)
        if m:
            n = int(m.group(1))
            if n in volumes:
                raise DocsError("%s: volume %02d heading appears twice" % (PLAN_REL, n))
            volumes[n] = unescape(m.group(2))
    if sorted(volumes) != list(range(1, VOLUMES + 1)):
        raise DocsError("%s: volume headings are %s, expected 01..%02d" % (PLAN_REL, sorted(volumes), VOLUMES))
    return named, volumes


def load_vol01(root):
    path = os.path.join(root, *VOL01_REL.split("/"))
    try:
        with open(path, encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, ValueError) as exc:
        raise DocsError("%s: %s" % (VOL01_REL, exc)) from exc
    if (
        not isinstance(data, dict)
        or not isinstance(data.get("documents"), list)
        or not isinstance(data.get("episodes"), list)
    ):
        raise DocsError("%s: expected an object with 'documents' and 'episodes' lists" % VOL01_REL)
    return data


def doc_path(doc_id, kind, title):
    if kind == "VOL":
        return "docs/vol/%s.md" % doc_id
    if doc_id in ARCH_SLUGS:
        return "docs/arch/%s-%s.md" % (doc_id, ARCH_SLUGS[doc_id])
    if kind in UNPLACED_KINDS:
        return None
    number = doc_id.rsplit("-", 1)[1]
    slug = VOL01_SLUGS.get(doc_id) or slugify(title)
    if kind == "REG":
        return "registry/SL-REG-%s-%s.json" % (number, slug)
    return "docs/%s/SL-%s-%s-%s.md" % (kind.lower(), kind, number, slug)


def build(root):
    """Return the register (dict) for the tree at root."""
    with open(os.path.join(root, *PLAN_REL.split("/")), encoding="utf-8") as fh:
        named, volumes = parse_plan(fh.read())
    vol01 = load_vol01(root)
    authoring = {}
    for entry in vol01["documents"]:
        if isinstance(entry, dict) and isinstance(entry.get("id"), str):
            authoring.setdefault(entry["id"], entry.get("authoring_wo"))
    mapped = {}
    for entry in vol01["episodes"]:
        if isinstance(entry, dict) and isinstance(entry.get("document"), str) and isinstance(entry.get("episode"), str):
            mapped.setdefault(entry["document"], []).append(entry["episode"])
    documents = []
    for doc_id, kind, title, written in named:
        eps = set(mapped.get(doc_id, []))
        for volume in parse_volumes_written(written):
            if volume != 1:
                eps.update(volume_episodes(volume))
        documents.append(_entry(doc_id, kind, TITLE_OVERRIDES.get(doc_id, title), eps, authoring.get(doc_id)))
    for n in range(1, VOLUMES + 1):
        doc_id = "SL-VOL-%02d-SPEC" % n
        title = "Volume %02d Specification: %s" % (n, volumes[n])
        documents.append(_entry(doc_id, "VOL", title, volume_episodes(n), authoring.get(doc_id)))
    return {"schema": SCHEMA_ID, "documents": documents}


def _entry(doc_id, kind, title, episodes, authoring_wo):
    return {
        "id": doc_id,
        "title": title,
        "aliases": list(ALIASES.get(doc_id, [])),
        "kind": kind,
        "path": doc_path(doc_id, kind, title),
        "status": "Planned",
        "version": None,
        "sha256": None,
        "episodes": sorted(episodes),
        "authoring_wo": authoring_wo,
    }


def render(register):
    return json.dumps(register, indent=2, ensure_ascii=False) + "\n"


# --- derived view (used by registry-docs-check) -----------------------------------------------


def load_approvals(root):
    """Return (approvals dict, error or None). A missing file counts as {}."""
    path = os.path.join(root, *APPROVALS_REL.split("/"))
    if not os.path.exists(path):
        return {}, None
    try:
        with open(path, encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, ValueError) as exc:
        return {}, "%s: %s" % (APPROVALS_REL, exc)
    if not isinstance(data, dict):
        return {}, "%s: must be a JSON object keyed by document id" % APPROVALS_REL
    return data, None


def derive_one(root, entry, approvals):
    """Return (status, version, sha256, problems) for one register entry."""
    path = entry.get("path")
    if not path:
        return "Draft", None, None, []
    full = os.path.join(root, *path.split("/"))
    if not os.path.isfile(full):
        return "Draft", None, None, []
    with open(full, "rb") as fh:
        raw = fh.read()
    sha = hashlib.sha256(raw).hexdigest()
    problems = []
    status, version = "Draft", None
    if path.endswith(".md") and entry["id"] not in NO_FRONT_MATTER:
        try:
            meta = slgov.parse_front_matter(raw.decode("utf-8"))
        except (slgov.FrontMatterError, UnicodeDecodeError) as exc:
            meta = None
            problems.append("%s:1: R-DOCS-FRONT: %s" % (path, exc))
        if meta is not None:
            if meta.get("id") != entry["id"]:
                problems.append(
                    "%s:1: R-DOCS-FRONT: front matter id %r is not %s" % (path, meta.get("id"), entry["id"])
                )
            if meta.get("status") in STATUSES:
                status = meta["status"]
            else:
                problems.append(
                    "%s:1: R-DOCS-FRONT: status %r must be one of %s" % (path, meta.get("status"), "|".join(STATUSES))
                )
            if isinstance(meta.get("version"), str) and meta["version"]:
                version = meta["version"]
    approval = approvals.get(entry["id"])
    if isinstance(approval, dict) and approval.get("sha256") == sha:
        status = "Approved"
        if isinstance(approval.get("version"), str) and approval["version"]:
            version = approval["version"]
    elif status == "Approved":
        status = "InReview"  # claimed, not granted: see R-DOCS-APPROVAL
    return status, version, sha, problems


def derive_view(root, register, approvals):
    """Return (documents with derived status/version/sha256, problems)."""
    view, problems = [], []
    for entry in register["documents"]:
        status, version, sha, probs = derive_one(root, entry, approvals)
        problems.extend(probs)
        row = dict(entry)
        row["status"], row["version"], row["sha256"] = status, version, sha
        view.append(row)
    return view, problems


def main(argv):
    root = slgov.ROOT
    args = list(argv[1:])
    if len(args) >= 2 and args[0] == "--root":
        root = os.path.abspath(args[1])
        args = args[2:]
    flags = set(args)
    if not flags <= {"--check", "--derived"} or len(args) != len(flags):
        print("usage: cmd_gen_docs.py [--root DIR] [--check] [--derived]", file=sys.stderr)
        return 2
    try:
        register = build(root)
    except (DocsError, OSError) as exc:
        print("gen-docs: FAIL: %s" % exc, file=sys.stderr)
        return 1
    if "--derived" in flags:
        approvals, _ = load_approvals(root)
        view, _ = derive_view(root, register, approvals)
        sys.stdout.write(render({"schema": SCHEMA_ID, "documents": view}))
        return 0
    text = render(register)
    target = os.path.join(root, *REGISTER_REL.split("/"))
    if "--check" in flags:
        try:
            with open(target, encoding="utf-8") as fh:
                current = fh.read()
        except OSError:
            current = None
        if current != text:
            print(
                "gen-docs: FAIL: %s is stale or missing; run python3 -I tools/governance/cmd_gen_docs.py" % REGISTER_REL
            )
            return 1
        print("gen-docs: ok")
        return 0
    with open(target, "w", encoding="utf-8") as fh:
        fh.write(text)
    print("gen-docs: wrote %s (%d documents)" % (REGISTER_REL, len(register["documents"])))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
