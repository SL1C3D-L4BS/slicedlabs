"""plan-check: the four plan documents agree with SL-REG-002 (WO-011).

`python3 -I cmd_plan_check.py [--root DIR]`

Rules (each failure line starts with the rule id):
  R-PLAN-DOC         a document is missing or unreadable
  R-PLAN-EP          an SL-EP-nnn cited in a document is not in the registry
  R-PLAN-GATE        a gate episode of master plan section 4.3 is missing from SL-PLAN-004, or is not the last
                     episode of its phase's final volume
  R-PLAN-GRAPH       SL-PLAN-002 does not reference registry/views/volume-graph.mmd, or the file is missing or
                     differs from the registry-generated graph
  R-PLAN-VOL03       SL-VOL-03-SPEC does not cover exactly episodes 041-060, a record differs from the registry,
                     or a listed work order is missing or does not trace the episode
  R-PLAN-VOL04       SL-VOL-04-SPEC does not cover exactly episodes 061-080, a record differs from the registry,
                     work_orders differs from the WO Traces lines (both ways), or primary_wo is not exactly one
                     member of work_orders whose front-matter acceptance holds the record's acceptance command (a
                     `just` recipe in the justfile), or a work order is primary for more than one episode
  R-PLAN-KOHI        the SL-PLAN-005 arc table differs from the generated one, or clean_room: true is not stated
  R-PLAN-CLEAN-ROOM  SL-PLAN-005 contains a code fence (clean room: Kohi is a guide only)

Stdlib only, no subprocess, no network (ADR-0004).
Volume specifications are table driven (VOLUME_SPECS); the justfile is read as an input of R-PLAN-VOL04.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmd_views  # noqa: E402
import cmd_wo_check  # noqa: E402
import slgov  # noqa: E402
import slib  # noqa: E402

DOCS = {
    "SL-PLAN-002": "docs/plan/SL-PLAN-002-episode-dependency-graph.md",
    "SL-PLAN-004": "docs/plan/SL-PLAN-004-vertical-slices.md",
    "SL-PLAN-005": "docs/plan/SL-PLAN-005-kohi-alignment-map.md",
    "SL-VOL-03-SPEC": "docs/vol/SL-VOL-03-SPEC.md",
    "SL-VOL-04-SPEC": "docs/vol/SL-VOL-04-SPEC.md",
    "justfile": "justfile",
}
# documents scanned for SL-EP ids (R-PLAN-EP); the justfile is an input, not a plan document
EP_SCANNED = ("SL-PLAN-002", "SL-PLAN-004", "SL-PLAN-005", "SL-VOL-03-SPEC", "SL-VOL-04-SPEC")
GRAPH_REF = "registry/views/volume-graph.mmd"
# master plan section 4.3: (phase, final volume of the phase, gate episode)
PHASES = (
    ("P0", "SL-VOL-05", 100),
    ("P1", "SL-VOL-10", 200),
    ("P2", "SL-VOL-20", 400),
    ("P3", "SL-VOL-25", 500),
    ("P4", "SL-VOL-32", 640),
    ("P5", "SL-VOL-42", 840),
)
# one row per volume specification whose sl-data records plan-check verifies
VOLUME_SPECS = (
    {
        "doc": "SL-VOL-03-SPEC",
        "rule": "R-PLAN-VOL03",
        "first": 41,
        "last": 60,
        "keys": ("work_orders", "acceptance", "kohi_ref", "seam"),
        "named": (41, 48, "WO-013 to WO-021"),  # episodes that must name work orders, and which
        "exact_traces": False,
        "primary": False,
    },
    {
        "doc": "SL-VOL-04-SPEC",
        "rule": "R-PLAN-VOL04",
        "first": 61,
        "last": 80,
        "keys": ("work_orders", "primary_wo", "acceptance", "kohi_ref", "seam", "performance", "release"),
        "named": (61, 80, "WO-035 to WO-063"),
        "exact_traces": True,
        "primary": True,
    },
)
EP_RE = re.compile(r"SL-EP-(\d+)")
FENCE_RE = re.compile(r"^\s*(```|~~~)")
WO_RE = re.compile(r"^WO-\d{3}$")


def read_text(root, rel):
    try:
        with open(os.path.join(root, *rel.split("/")), encoding="utf-8", newline="") as fh:
            return fh.read()
    except (OSError, UnicodeDecodeError):
        return None


def episode_errors(texts, reg):
    known = {e["id"] for e in reg["episodes"]}
    out = []
    for key in EP_SCANNED:
        for n, line in enumerate(texts[key].splitlines(), 1):
            for m in EP_RE.finditer(line):
                if m.group(0) not in known:
                    out.append("%s:%d: R-PLAN-EP: %s is not in SL-REG-002" % (DOCS[key], n, m.group(0)))
    return out


def gate_errors(texts, reg):
    out = []
    path = DOCS["SL-PLAN-004"]
    text = texts["SL-PLAN-004"]
    last = {v["id"]: v["last"] for v in reg["volumes"]}
    for phase, vol, gate in PHASES:
        ep = "SL-EP-%03d" % gate
        if ep not in text:
            out.append(
                "%s:1: R-PLAN-GATE: gate episode %s of phase %s (master plan section 4.3) is not cited"
                % (path, ep, phase)
            )
        if last.get(vol) != gate:
            out.append("%s:1: R-PLAN-GATE: phase %s gate %s is not the last episode of %s in SL-REG-002 (%s)"
                       % (path, phase, ep, vol, last.get(vol)))
    return out


def graph_errors(root, texts, reg):
    out = []
    path = DOCS["SL-PLAN-002"]
    if GRAPH_REF not in texts["SL-PLAN-002"]:
        out.append("%s:1: R-PLAN-GRAPH: does not reference %s" % (path, GRAPH_REF))
    have = read_text(root, GRAPH_REF)
    if have is None:
        out.append("%s:1: R-PLAN-GRAPH: %s is missing; run `just registry-views`" % (path, GRAPH_REF))
    elif have != cmd_views.volume_graph_mmd(reg):
        out.append(
            "%s:1: R-PLAN-GRAPH: %s does not match the volume graph of SL-REG-002; run `just registry-views`"
            % (path, GRAPH_REF)
        )
    return out


def sl_data(text):
    """The parsed JSON of the first ```sl-data fence, or None."""
    lines = text.splitlines()
    for i, line in enumerate(lines):
        if line.strip() == "```sl-data":
            body = []
            for rest in lines[i + 1 :]:
                if rest.strip() == "```":
                    try:
                        return slib.loads_json("\n".join(body), "sl-data")
                    except slib.LoadError:
                        return None
                body.append(rest)
    return None


def traces(root):
    """{WO id: set of SL-EP ids} from the `Traces:` line of every work-orders/WO-nnn.md."""
    out = {}
    directory = os.path.join(root, "work-orders")
    if not os.path.isdir(directory):
        return out
    for name in sorted(os.listdir(directory)):
        m = re.match(r"^(WO-\d{3})\.md$", name)
        if not m:
            continue
        text = read_text(root, "work-orders/" + name) or ""
        ids = set()
        for line in text.splitlines():
            if line.startswith("Traces:"):
                ids.update(x.group(0) for x in EP_RE.finditer(line))
        out[m.group(1)] = ids
    return out


def wo_acceptance(root, wo):
    """(list of acceptance cmd strings, error or None) from work-orders/<wo>.md front matter."""
    text = read_text(root, "work-orders/%s.md" % wo)
    if text is None:
        return [], "has no file in work-orders/"
    try:
        meta, _ = cmd_wo_check.parse_work_order(text)
    except (cmd_wo_check.WoParseError, slgov.FrontMatterError) as exc:
        return [], "front matter does not parse: %s" % exc
    acc = meta.get("acceptance")
    if not isinstance(acc, list):
        return [], "front matter has no acceptance list"
    return [a.get("cmd") for a in acc if isinstance(a, dict)], None


def primary_errors(root, path, rule, rec, eid, wos, recipes, primaries):
    """R-PLAN-VOL04 primary_wo checks for one record; records the primary in `primaries`."""
    out = []
    primary = rec.get("primary_wo")
    if not isinstance(primary, str) or not WO_RE.match(primary):
        return ["%s:1: %s: %s primary_wo must be exactly one work order id, got %r" % (path, rule, eid, primary)]
    primaries.setdefault(primary, []).append(eid)
    if primary not in wos:
        out.append("%s:1: %s: %s primary_wo %s is not in its work_orders" % (path, rule, eid, primary))
    acceptance = rec.get("acceptance")
    words = acceptance.split() if isinstance(acceptance, str) else []
    if len(words) < 2 or words[0] != "just" or words[1] not in recipes:
        out.append(
            "%s:1: %s: %s acceptance %r is not `just <recipe>` for a recipe in the justfile"
            % (path, rule, eid, acceptance)
        )
    cmds, error = wo_acceptance(root, primary)
    if error:
        out.append("%s:1: %s: %s primary_wo %s %s" % (path, rule, eid, primary, error))
    elif acceptance not in cmds:
        out.append(
            "%s:1: %s: %s acceptance %r is not in the front-matter acceptance of its primary_wo %s"
            % (path, rule, eid, acceptance, primary)
        )
    return out


def vol_errors(root, texts, reg, spec, tr, recipes):
    """Errors for one row of VOLUME_SPECS (R-PLAN-VOL03 output is unchanged from WO-011)."""
    out = []
    rule, first, last = spec["rule"], spec["first"], spec["last"]
    path = DOCS[spec["doc"]]
    data = sl_data(texts[spec["doc"]])
    if not isinstance(data, dict) or not isinstance(data.get("episodes"), list):
        return ["%s:1: %s: no parsable sl-data block with an episodes list" % (path, rule)]
    by_id = {e["id"]: e for e in reg["episodes"]}
    want = ["SL-EP-%03d" % n for n in range(first, last + 1)]
    got = [r.get("episode") for r in data["episodes"] if isinstance(r, dict)]
    if got != want:
        missing = [w for w in want if w not in got]
        extra = [g for g in got if g not in want]
        out.append(
            "%s:1: %s: records must cover exactly SL-EP-%03d to SL-EP-%03d in order (missing %s; unexpected "
            "%s)"
            % (path, rule, first, last, missing or "none", extra or "none")
        )
    listed, primaries = {}, {}
    for rec in data["episodes"]:
        if not isinstance(rec, dict) or rec.get("episode") not in by_id:
            continue
        ep = by_id[rec["episode"]]
        eid = ep["id"]
        for field, reg_value in (("title", ep["title"]), ("purpose", ep["summary"])):
            if rec.get(field) != reg_value:
                out.append(
                    "%s:1: %s: %s %s %r differs from the registry %r"
                    % (path, rule, eid, field, rec.get(field), reg_value)
                )
        for key in spec["keys"]:
            if key not in rec:
                out.append("%s:1: %s: %s lacks '%s'" % (path, rule, eid, key))
        wos = rec.get("work_orders", [])
        if not isinstance(wos, list):
            out.append("%s:1: %s: %s work_orders must be a list" % (path, rule, eid))
            continue
        low, high, which = spec["named"]
        if low <= ep["number"] <= high and not wos:
            out.append("%s:1: %s: %s must name its work orders (%s)" % (path, rule, eid, which))
        if rec.get("kohi_ref", "none") != (ep["kohi_ref"] or "none"):
            out.append("%s:1: %s: %s kohi_ref differs from the registry" % (path, rule, eid))
        listed[eid] = set(wos)
        for wo in wos:
            if not isinstance(wo, str) or not WO_RE.match(wo) or wo not in tr:
                out.append("%s:1: %s: %s lists %s which has no file in work-orders/" % (path, rule, eid, wo))
            elif eid not in tr[wo]:
                out.append(
                    "%s:1: %s: %s lists %s but its Traces line does not name %s" % (path, rule, eid, wo, eid)
                )
        if spec["primary"]:
            out += primary_errors(root, path, rule, rec, eid, wos, recipes, primaries)
    for wo in sorted(tr):
        for eid in sorted(tr[wo]):
            in_range = spec["exact_traces"] and eid in want
            if (eid in listed or in_range) and wo not in listed.get(eid, ()):
                out.append("%s:1: %s: %s traces %s but the %s record does not list it" % (path, rule, wo, eid, eid))
    for wo in sorted(primaries):
        if len(primaries[wo]) > 1:
            out.append(
                "%s:1: %s: %s is primary_wo of more than one episode (%s); a work order is primary for at most one"
                % (path, rule, wo, ", ".join(primaries[wo]))
            )
    return out


def table_block(text, header):
    lines = text.splitlines()
    for i, line in enumerate(lines):
        if line == header:
            j = i
            while j < len(lines) and lines[j].startswith("|"):
                j += 1
            return lines[i:j]
    return None


def kohi_errors(texts, reg):
    out = []
    path = DOCS["SL-PLAN-005"]
    text = texts["SL-PLAN-005"]
    want = table_block(cmd_views.kohi_map_md(reg), cmd_views.ARC_TABLE_HEADER)
    have = table_block(text, cmd_views.ARC_TABLE_HEADER)
    if have is None:
        out.append("%s:1: R-PLAN-KOHI: arc table (header '%s') not found" % (path, cmd_views.ARC_TABLE_HEADER))
    elif have != want:
        out.append(
            "%s:1: R-PLAN-KOHI: arc table differs from the one generated from kohi_ref; copy it from "
            "registry/views/kohi-map.md"
            % path
        )
    if "clean_room: true" not in text:
        out.append("%s:1: R-PLAN-KOHI: must state `clean_room: true` for Kohi-referenced episodes" % path)
    for n, line in enumerate(text.splitlines(), 1):
        if FENCE_RE.match(line):
            out.append(
                "%s:%d: R-PLAN-CLEAN-ROOM: code fence in a clean-room document; Kohi is a guide only" % (path, n)
            )
    return out


def plan_errors(root):
    errors, texts = [], {}
    for key, rel in DOCS.items():
        text = read_text(root, rel)
        if text is None:
            errors.append("%s:1: R-PLAN-DOC: document is missing or unreadable" % rel)
        else:
            texts[key] = text
    if errors:
        return errors
    reg = cmd_views.load_registry(root)
    errors += episode_errors(texts, reg)
    errors += gate_errors(texts, reg)
    errors += graph_errors(root, texts, reg)
    tr = traces(root)
    recipes = set(slgov.parse_justfile(texts["justfile"]))
    for spec in VOLUME_SPECS:
        errors += vol_errors(root, texts, reg, spec, tr, recipes)
    errors += kohi_errors(texts, reg)
    return errors


def main(argv):
    root = slgov.ROOT
    if len(argv) == 3 and argv[1] == "--root":
        root = os.path.abspath(argv[2])
    elif len(argv) != 1:
        print("usage: cmd_plan_check.py [--root DIR]", file=sys.stderr)
        return 2
    try:
        errors = plan_errors(root)
    except slib.LoadError as exc:
        print("plan-check: %s" % exc, file=sys.stderr)
        return 2
    return slgov.report("plan-check", errors)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
