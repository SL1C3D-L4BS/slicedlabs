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
  R-PLAN-KOHI        the SL-PLAN-005 arc table differs from the generated one, or clean_room: true is not stated
  R-PLAN-CLEAN-ROOM  SL-PLAN-005 contains a code fence (clean room: Kohi is a guide only)

Stdlib only, no subprocess, no network (ADR-0004).
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmd_views  # noqa: E402
import slgov  # noqa: E402
import slib  # noqa: E402

DOCS = {
    "SL-PLAN-002": "docs/plan/SL-PLAN-002-episode-dependency-graph.md",
    "SL-PLAN-004": "docs/plan/SL-PLAN-004-vertical-slices.md",
    "SL-PLAN-005": "docs/plan/SL-PLAN-005-kohi-alignment-map.md",
    "SL-VOL-03-SPEC": "docs/vol/SL-VOL-03-SPEC.md",
}
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
VOL03_FIRST, VOL03_LAST = 41, 60
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
    for key, text in texts.items():
        for n, line in enumerate(text.splitlines(), 1):
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


def vol03_errors(root, texts, reg):
    out = []
    path = DOCS["SL-VOL-03-SPEC"]
    data = sl_data(texts["SL-VOL-03-SPEC"])
    if not isinstance(data, dict) or not isinstance(data.get("episodes"), list):
        return ["%s:1: R-PLAN-VOL03: no parsable sl-data block with an episodes list" % path]
    by_id = {e["id"]: e for e in reg["episodes"]}
    want = ["SL-EP-%03d" % n for n in range(VOL03_FIRST, VOL03_LAST + 1)]
    got = [r.get("episode") for r in data["episodes"] if isinstance(r, dict)]
    if got != want:
        missing = [w for w in want if w not in got]
        extra = [g for g in got if g not in want]
        out.append(
            "%s:1: R-PLAN-VOL03: records must cover exactly SL-EP-%03d to SL-EP-%03d in order (missing %s; unexpected "
            "%s)"
            % (path, VOL03_FIRST, VOL03_LAST, missing or "none", extra or "none")
        )
    tr = traces(root)
    listed = {}
    for rec in data["episodes"]:
        if not isinstance(rec, dict) or rec.get("episode") not in by_id:
            continue
        ep = by_id[rec["episode"]]
        eid = ep["id"]
        for field, reg_value in (("title", ep["title"]), ("purpose", ep["summary"])):
            if rec.get(field) != reg_value:
                out.append(
                    "%s:1: R-PLAN-VOL03: %s %s %r differs from the registry %r"
                    % (path, eid, field, rec.get(field), reg_value)
                )
        for key in ("work_orders", "acceptance", "kohi_ref", "seam"):
            if key not in rec:
                out.append("%s:1: R-PLAN-VOL03: %s lacks '%s'" % (path, eid, key))
        wos = rec.get("work_orders", [])
        if not isinstance(wos, list):
            out.append("%s:1: R-PLAN-VOL03: %s work_orders must be a list" % (path, eid))
            continue
        if 41 <= ep["number"] <= 48 and not wos:
            out.append("%s:1: R-PLAN-VOL03: %s must name its work orders (WO-013 to WO-021)" % (path, eid))
        if rec.get("kohi_ref", "none") != (ep["kohi_ref"] or "none"):
            out.append("%s:1: R-PLAN-VOL03: %s kohi_ref differs from the registry" % (path, eid))
        listed[eid] = set(wos)
        for wo in wos:
            if not isinstance(wo, str) or not WO_RE.match(wo) or wo not in tr:
                out.append("%s:1: R-PLAN-VOL03: %s lists %s which has no file in work-orders/" % (path, eid, wo))
            elif eid not in tr[wo]:
                out.append(
                    "%s:1: R-PLAN-VOL03: %s lists %s but its Traces line does not name %s" % (path, eid, wo, eid)
                )
    for wo in sorted(tr):
        for eid in sorted(tr[wo]):
            if eid in listed and wo not in listed[eid]:
                out.append("%s:1: R-PLAN-VOL03: %s traces %s but the %s record does not list it" % (path, wo, eid, eid))
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
    errors += vol03_errors(root, texts, reg)
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
