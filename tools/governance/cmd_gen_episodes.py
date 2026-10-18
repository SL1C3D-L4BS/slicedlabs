"""Generate registry/SL-REG-002-episodes.json from the pinned master plan (SL-PLAN-001).

    python3 -I cmd_gen_episodes.py [--root DIR] [--force]    write the registry, the lock and the accepted stub
    python3 -I cmd_gen_episodes.py --check [--root DIR]      re-parse the plan and compare with the file

The registry is authoritative once written (master plan 6.3 rule 1). The generator therefore refuses to
overwrite an existing registry unless --force is given; --check never writes. --check re-parses the plan,
requires the plan to still hash to the pinned sha256, and compares id, number, volume, title, summary and
meaning_sha256 of every episode (and the part and volume tiling) with the file. A difference is accepted
only when the episode carries an amendment chain that starts at the plan's own meaning hash and ends at
the registry's (master plan 6.3 rule 3).

Parsing is deterministic: volume headings, `* **NNN** Title — summary.` bullets (split on the FIRST " — ";
one trailing "." is the sentence terminator and is dropped; 301 bullets are title-only, summary null),
section 4.1 (ranges), 4.2 (mermaid edges, `\\-\\-\\>` unescaped), 5.1 (Kohi arcs), 1.4 (milestones) and 6.2
(lifecycle). Google-Docs escapes (backslash before ASCII punctuation) are removed. Stdlib only (ADR-0004).
"""

import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slib  # noqa: E402

PLAN_REL = "docs/plan/SL-PLAN-001-master-plan.md"
PIN_REL = "docs/plan/SL-PLAN-001-master-plan.sha256"
REG_REL = "registry/SL-REG-002-episodes.json"
LOCK_REL = "registry/SL-REG-002-episodes.lock"
ACCEPTED_REL = "registry/SL-REG-002.accepted.json"
SEED_RELS = ("tools/governance/seed/episodes-001-020.json", "tools/governance/seed/episodes-041-060.json")
SEED_SCHEMA = "sl-episodes-seed/1"
GENERATED_BY = "WO-004"

ROMAN = {"I": 1, "II": 2, "III": 3, "IV": 4, "V": 5, "VI": 6, "VII": 7, "VIII": 8, "IX": 9, "X": 10}
MILESTONE_NAMES = {"Sliced Labs 1.0": "1.0"}
MILESTONES = ("Foundation", "Core loop", "Production", "Early Access", "1.0")
DEFAULT_DECISION_STATUS = "Unresolved"

# ADR-0003 waiver: episodes 041-048 may start before Volume 01 is Accepted, but stop at Demonstrated.
WAIVERS = (
    {"rule": "R-DEP-ACCEPTED", "adr": "ADR-0003", "first": 41, "last": 48, "max_status": "Demonstrated"},
)

_ESCAPE_RE = re.compile(r"\\([!-/:-@\[-`{-~])")
_PART_RE = re.compile(r"^## \*\*Part ([IVX]+) \u2014 (.+?) \(episodes (\d{3})\u2013(\d{3})\)\*\*\s*$")
_VOL_RE = re.compile(r"^### \*\*Volume (\d{2}) \u2014 (.+?)\*\*\s*$")
_BULLET_RE = re.compile(r"^\* \*\*(\d{3})\*\* (.+?)\s*$")
_GOAL_RE = re.compile(r"^Goal: (.+?)\s*$")
_RANGE_RE = re.compile(r"^(\d{3})\u2013(\d{3})$")
_NODE_RE = re.compile(r"\bV(\d{2})\b")
_LABEL_RE = re.compile(r"\[[^\]]*\]")


class ParseError(ValueError):
    """The plan does not have the structure the generator relies on."""


def unescape(text):
    return _ESCAPE_RE.sub(r"\1", text)


def _section(lines, start_prefix):
    """Lines of the `### **<start_prefix>` section, up to the next heading."""
    out, inside = [], False
    for line in lines:
        if line.startswith("### ") or line.startswith("## ") or line.startswith("# "):
            if inside:
                break
            inside = line.startswith("### **%s" % start_prefix)
            continue
        if inside:
            out.append(line)
    if not out:
        raise ParseError("section %s not found" % start_prefix)
    return out


def _table_rows(section_lines):
    rows = []
    for line in section_lines:
        line = line.strip()
        if line.startswith("|") and not re.match(r"^\|[\s:\-|]+\|?$", line):
            rows.append([c.strip() for c in line.strip("|").split("|")])
    return rows[1:]  # drop the header row


def parse_episode_text(text):
    """'Title \u2014 summary.' -> (title, summary); a bullet without ' \u2014 ' is title-only (summary None)."""
    text = unescape(text.strip())
    if text.endswith("."):
        text = text[:-1]
    if " \u2014 " in text:
        title, summary = text.split(" \u2014 ", 1)
        return title.strip(), summary.strip()
    return text.strip(), None


def _expand(spec):
    """'041\u2013048' or '097, 108' or '130\u2013133, 147, 151' -> list of ints."""
    nums = []
    for piece in unescape(spec).split(","):
        piece = piece.strip()
        m = _RANGE_RE.match(piece)
        if m:
            nums.extend(range(int(m.group(1)), int(m.group(2)) + 1))
        elif re.match(r"^\d{3}$", piece):
            nums.append(int(piece))
        else:
            raise ParseError("cannot read episode list %r" % spec)
    return nums


def parse_volume_edges(lines):
    """Section 4.2 mermaid: returns sorted unique (source_number, target_number) edges."""
    edges = set()
    for raw in _section(lines, "4.2"):
        line = _LABEL_RE.sub("", unescape(raw)).strip()
        if "-->" not in line:
            continue
        segments = [seg for seg in line.split("-->")]
        groups = []
        for seg in segments:
            nodes = [int(n) for n in _NODE_RE.findall(seg)]
            if not nodes:
                raise ParseError("4.2: no volume node in %r" % raw.strip())
            groups.append(nodes)
        for src_group, dst_group in zip(groups, groups[1:]):
            for s in src_group:
                for d in dst_group:
                    edges.add((s, d))
    return sorted(edges)


def parse_kohi(lines):
    """Section 5.1: {episode number: [arc label, ...]} in table order."""
    arcs = {}
    for row in _table_rows(_section(lines, "5.1")):
        if len(row) != 4:
            raise ParseError("5.1: a row has %d cells, expected 4" % len(row))
        arc, kohi_eps, sl_eps = unescape(row[0]), unescape(row[1]), row[2]
        label = "%s (%s)" % (arc, kohi_eps)
        for n in _expand(sl_eps):
            arcs.setdefault(n, []).append(label)
    return arcs


def parse_milestones(lines):
    """Section 1.4: [(milestone, first, last)] with 'Sliced Labs 1.0' mapped to '1.0'."""
    out = []
    for row in _table_rows(_section(lines, "1.4")):
        name = MILESTONE_NAMES.get(unescape(row[0]), unescape(row[0]))
        m = _RANGE_RE.match(unescape(row[1]))
        if name not in MILESTONES or not m:
            raise ParseError("1.4: cannot read row %r" % row)
        out.append((name, int(m.group(1)), int(m.group(2))))
    return out


def parse_transitions(lines):
    """Section 6.2 stateDiagram: [(from, to)] in plan order (the [*] entry is not a transition)."""
    out = []
    for raw in _section(lines, "6.2"):
        line = unescape(raw).strip()
        m = re.match(r"^(\w+) --> (\w+)", line)
        if m and m.group(1) != "[*]":
            out.append((m.group(1), m.group(2)))
    if not out:
        raise ParseError("6.2: no transitions found")
    return out


def parse_plan(text):
    """Parse the master plan into {parts, volumes, episodes, edges, kohi, milestones, transitions, table_ranges}."""
    lines = text.split("\n")
    parts, volumes, episodes = [], [], []
    cur_vol = None
    part_no = None
    for lineno, line in enumerate(lines, 1):
        m = _PART_RE.match(line)
        if m:
            part_no = ROMAN[m.group(1)]
            parts.append(
                {"number": part_no, "title": unescape("Part %s \u2014 %s" % (m.group(1), m.group(2))),
                 "first": int(m.group(3)), "last": int(m.group(4))}
            )
            cur_vol = None
            continue
        if line.startswith("## "):
            cur_vol = None
            continue
        m = _VOL_RE.match(line)
        if m:
            if part_no is None:
                raise ParseError("line %d: volume before the first part" % lineno)
            cur_vol = {
                "number": int(m.group(1)),
                "title": unescape(m.group(2)),
                "part": part_no,
                "goal": None,
                "numbers": [],
            }
            volumes.append(cur_vol)
            continue
        if line.startswith("### "):
            cur_vol = None
            continue
        if cur_vol is None:
            continue
        m = _GOAL_RE.match(line)
        if m and not cur_vol["numbers"] and cur_vol["goal"] is None:
            cur_vol["goal"] = unescape(m.group(1))
            continue
        m = _BULLET_RE.match(line)
        if m:
            title, summary = parse_episode_text(m.group(2))
            number = int(m.group(1))
            cur_vol["numbers"].append(number)
            episodes.append({"number": number, "volume_number": cur_vol["number"], "title": title, "summary": summary})
    table_ranges = {}
    for row in _table_rows(_section(lines, "4.1")):
        m = _RANGE_RE.match(unescape(row[2]))
        if not m or not row[1].isdigit():
            raise ParseError("4.1: cannot read row %r" % row)
        table_ranges[int(row[1])] = (int(m.group(1)), int(m.group(2)))
    for v in volumes:
        nums = v.pop("numbers")
        if not nums:
            raise ParseError("volume %02d has no episodes" % v["number"])
        v["first"], v["last"] = nums[0], nums[-1]
        if nums != list(range(v["first"], v["last"] + 1)):
            raise ParseError("volume %02d episode numbers are not contiguous" % v["number"])
        if table_ranges.get(v["number"]) != (v["first"], v["last"]):
            raise ParseError(
                "volume %02d range %d-%d differs from section 4.1 %s"
                % (v["number"], v["first"], v["last"], table_ranges.get(v["number"]))
            )
    if sorted(table_ranges) != [v["number"] for v in volumes]:
        raise ParseError("section 4.1 and the volume headings list different volumes")
    return {
        "parts": parts,
        "volumes": volumes,
        "episodes": episodes,
        "edges": parse_volume_edges(lines),
        "kohi": parse_kohi(lines),
        "milestones": parse_milestones(lines),
        "transitions": parse_transitions(lines),
        "table_ranges": table_ranges,
    }


def vol_id(number):
    return "SL-VOL-%02d" % number


def ep_id(number):
    return "SL-EP-%03d" % number


def volume_dependencies(edges):
    """{volume number: sorted list of SL-VOL ids it depends on} from the 4.2 edge list."""
    deps = {}
    for src, dst in edges:
        deps.setdefault(dst, set()).add(src)
    return {n: [vol_id(s) for s in sorted(srcs)] for n, srcs in deps.items()}


# --- seeds ------------------------------------------------------------------------------------


def load_seeds(root):
    """Merge tools/governance/seed/*.json: {episode id: {decision_status, kohi_ref, release: {milestone}}}."""
    merged = {}
    for rel in SEED_RELS:
        data = slib.load_json(os.path.join(root, rel))
        if data.get("schema") != SEED_SCHEMA or not isinstance(data.get("episodes"), dict):
            raise ParseError("%s: expected schema %s with an 'episodes' object" % (rel, SEED_SCHEMA))
        for key, rec in data["episodes"].items():
            if key in merged:
                raise ParseError("%s: %s is seeded twice" % (rel, key))
            if set(rec) != {"decision_status", "kohi_ref", "release"} or set(rec["release"]) != {"milestone"}:
                raise ParseError("%s: %s needs exactly decision_status, kohi_ref and release.milestone" % (rel, key))
            merged[key] = rec
    return merged


# --- registry ---------------------------------------------------------------------------------


def build_registry(plan, plan_sha, seeds):
    deps = volume_dependencies(plan["edges"])
    volumes = []
    for v in plan["volumes"]:
        volumes.append(
            {"id": vol_id(v["number"]), "number": v["number"], "title": v["title"], "part": v["part"],
             "first": v["first"], "last": v["last"], "goal": v["goal"],
             "depends_on_volumes": deps.get(v["number"], [])}
        )
    milestones = plan["milestones"]
    episodes = []
    for e in plan["episodes"]:
        n = e["number"]
        eid = ep_id(n)
        milestone = next((m for m, a, b in milestones if a <= n <= b), None)
        arcs = plan["kohi"].get(n)
        kohi = "; ".join(arcs) if arcs else None
        decision = DEFAULT_DECISION_STATUS
        seed = seeds.get(eid)
        if seed is not None:
            if seed["kohi_ref"] != kohi:
                raise ParseError("seed %s kohi_ref %r differs from plan section 5.1 %r" % (eid, seed["kohi_ref"], kohi))
            if seed["release"]["milestone"] != milestone:
                raise ParseError(
                    "seed %s milestone %r differs from plan section 1.4 %r"
                    % (eid, seed["release"]["milestone"], milestone)
                )
            decision = seed["decision_status"]
        episodes.append(
            {"id": eid, "number": n, "volume": vol_id(e["volume_number"]), "title": e["title"], "summary": e["summary"],
             "meaning_sha256": slib.meaning_sha256(e["title"], e["summary"]), "status": "Planned",
             "decision_status": decision, "depends_on": [], "work_orders": [], "acceptance": [],
             "kohi_ref": kohi, "release": {"milestone": milestone, "platforms": []}, "documents": [],
             "completion": None, "history": [], "amendments": []}
        )
    for sid in seeds:
        if sid not in {e["id"] for e in episodes}:
            raise ParseError("seed names unknown episode %s" % sid)
    return {
        "schema": "sl-reg-002/1",
        "source": {"plan": "SL-PLAN-001", "sha256": plan_sha},
        "lifecycle": {
            "states": list(slib.STATES),
            "transitions": [list(t) for t in plan["transitions"]],
            "waivers": [dict(w) for w in WAIVERS],
        },
        "parts": plan["parts"],
        "volumes": volumes,
        "episodes": episodes,
        "amendments": [],
    }


def dumps(obj):
    return json.dumps(obj, indent=1, sort_keys=True, ensure_ascii=False) + "\n"


def lock_projection(reg):
    """The immutable projection: volumes (id, range, title) and episodes (id, volume, title)."""
    return {
        "volumes": [
            {"id": v["id"], "first": v["first"], "last": v["last"], "title": v["title"]} for v in reg["volumes"]
        ],
        "episodes": [{"id": e["id"], "volume": e["volume"], "title": e["title"]} for e in reg["episodes"]],
    }


def lock_digest(reg):
    return slib.sha256_text(dumps(lock_projection(reg)))


# --- io helpers -------------------------------------------------------------------------------


def read_plan(root):
    """(plan text, actual sha256, pinned sha256)."""
    path = os.path.join(root, PLAN_REL)
    try:
        with open(path, "rb") as fh:
            raw = fh.read()
        with open(os.path.join(root, PIN_REL), encoding="utf-8") as fh:
            pinned = fh.read().split()[0]
    except (OSError, IndexError) as exc:
        raise ParseError("cannot read the plan or its pin: %s" % exc) from exc
    import hashlib

    return raw.decode("utf-8"), hashlib.sha256(raw).hexdigest(), pinned


def load_plan(root):
    text, actual, pinned = read_plan(root)
    if actual != pinned:
        raise ParseError("%s hashes to %s but %s pins %s; the plan changed" % (PLAN_REL, actual, PIN_REL, pinned))
    return parse_plan(text), actual


# --- --check ----------------------------------------------------------------------------------

_EP_FIELDS = ("id", "number", "volume", "title", "summary", "meaning_sha256")


def compare(plan, plan_sha, reg, lock_text=None):
    """Differences between a fresh parse and the registry that amendments do not cover."""
    problems = []
    if reg["source"]["sha256"] != plan_sha:
        problems.append(
            "source sha256 %s differs from the plan sha256 %s" % (reg["source"]["sha256"][:12], plan_sha[:12])
        )
    if reg["parts"] != plan["parts"]:
        problems.append("parts differ from the plan part headings")
    fresh_vols = {vol_id(v["number"]): v for v in plan["volumes"]}
    reg_vols = {v["id"]: v for v in reg["volumes"]}
    if sorted(fresh_vols) != sorted(reg_vols):
        problems.append("volume ids differ from the plan")
    for vid in sorted(set(fresh_vols) & set(reg_vols)):
        f, r = fresh_vols[vid], reg_vols[vid]
        for key in ("number", "title", "part", "first", "last"):
            if f[key] != r[key]:
                problems.append("%s %s: plan has %r, registry has %r" % (vid, key, f[key], r[key]))
    fresh_eps = {ep_id(e["number"]): e for e in plan["episodes"]}
    reg_eps = {e["id"]: e for e in reg["episodes"]}
    for eid in sorted(set(fresh_eps) - set(reg_eps)):
        problems.append("%s is in the plan but not in the registry" % eid)
    for eid in sorted(set(reg_eps) - set(fresh_eps)):
        problems.append("%s is in the registry but not in the plan" % eid)
    for eid in sorted(set(fresh_eps) & set(reg_eps)):
        f, r = fresh_eps[eid], reg_eps[eid]
        want = {"id": eid, "number": f["number"], "volume": vol_id(f["volume_number"]), "title": f["title"],
                "summary": f["summary"], "meaning_sha256": slib.meaning_sha256(f["title"], f["summary"])}
        covered = bool(r["amendments"]) and r["amendments"][0]["from_sha256"] == want["meaning_sha256"]
        for key in _EP_FIELDS:
            if want[key] == r[key]:
                continue
            if covered and key in ("title", "summary", "meaning_sha256"):
                continue
            problems.append(
                "%s %s: plan has %r, registry has %r (no amendment covers it)" % (eid, key, want[key], r[key])
            )
    if lock_text is not None and lock_text.strip() != lock_digest(reg):
        problems.append("%s does not match the registry's immutable projection (volumes and episode titles)" % LOCK_REL)
    return problems


def run_check(root):
    try:
        plan, plan_sha = load_plan(root)
        reg = slib.load_json(os.path.join(root, REG_REL))
        with open(os.path.join(root, LOCK_REL), encoding="utf-8") as fh:
            lock_text = fh.read()
    except (ParseError, slib.LoadError, OSError) as exc:
        print("registry-gen-check: FAIL: %s" % exc)
        return 1
    problems = compare(plan, plan_sha, reg, lock_text)
    if problems:
        for p in problems[:40]:
            print("registry-gen-check: FAIL: %s" % p)
        if len(problems) > 40:
            print("registry-gen-check: ... and %d more" % (len(problems) - 40))
        return 1
    print("registry-gen-check: ok (%d episodes, %d volumes, %d parts re-parsed from %s, sha %s)"
          % (len(plan["episodes"]), len(plan["volumes"]), len(plan["parts"]), PLAN_REL, plan_sha[:12]))
    return 0


def run_generate(root, force):
    reg_path = os.path.join(root, REG_REL)
    if os.path.exists(reg_path) and not force:
        print(
            "gen_episodes: %s exists and is authoritative; refusing to overwrite (use --force, or --check)" % REG_REL,
            file=sys.stderr,
        )
        return 2
    try:
        plan, plan_sha = load_plan(root)
        reg = build_registry(plan, plan_sha, load_seeds(root))
    except (ParseError, slib.LoadError) as exc:
        print("gen_episodes: %s" % exc, file=sys.stderr)
        return 2
    os.makedirs(os.path.dirname(reg_path), exist_ok=True)
    with open(reg_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(dumps(reg))
    with open(os.path.join(root, LOCK_REL), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(lock_digest(reg) + "\n")
    accepted = os.path.join(root, ACCEPTED_REL)
    if not os.path.exists(accepted):
        with open(accepted, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("{}\n")
    print(
        "gen_episodes: wrote %s (%d episodes, %d volumes), %s"
        % (REG_REL, len(reg["episodes"]), len(reg["volumes"]), LOCK_REL)
    )
    return 0


def main(argv):
    args = argv[1:]
    root = slib.ROOT
    check = force = False
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--check":
            check = True
        elif a == "--force":
            force = True
        elif a == "--root" and i + 1 < len(args):
            i += 1
            root = os.path.abspath(args[i])
        else:
            print("usage: cmd_gen_episodes.py [--root DIR] [--force | --check]", file=sys.stderr)
            return 2
        i += 1
    if check and force:
        print("gen_episodes: --check and --force are exclusive", file=sys.stderr)
        return 2
    return run_check(root) if check else run_generate(root, force)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
