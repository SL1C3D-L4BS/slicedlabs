"""episodes-doc-check <range>: the episode record docs/episodes/<first>-<last>.md is complete (WO-021).

Usage: python3 -I cmd_episodes_doc_check.py <NNN-MMM | NNN> [--root DIR] [--selftest]
(the document for 041-048 is docs/episodes/041-048.md; master plan section 6.1 record fields, ADR-0003)

Rules (each failure line starts with the rule id):
  R-EPDOC-RANGE     the range is NNN-MMM or NNN with 1 <= NNN <= MMM <= 840, and every episode of it is in
                    registry/SL-REG-002-episodes.json (the registry is authoritative, master plan 6.3 rule 1)
  R-EPDOC-FILE      docs/episodes/<range>.md exists
  R-EPDOC-SECTION   every episode of the range has exactly one `## SL-EP-nnn <dash> <title>` section whose
                    title equals the registry title verbatim, and the document has no section for an
                    episode outside the range
  R-EPDOC-HEADINGS  inside each section the `###` headings are exactly Done, Decisions, Acceptance, Evidence,
                    Open issues, in that order, each with some text
  R-EPDOC-ACCEPT    the Acceptance text names at least one `just <recipe>` and every named recipe exists in
                    the justfile
  R-EPDOC-DECISIONS the Decisions text names an ADR-NNNN that exists in docs/adr/ (every named id must
                    exist) or says "none"

Stdlib only, no subprocess, no network (ADR-0004).
"""

import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slgov  # noqa: E402

REGISTRY_REL = "registry/SL-REG-002-episodes.json"
SUBHEADINGS = ("Done", "Decisions", "Acceptance", "Evidence", "Open issues")
DASHES = ("—", "–", "-", ":")  # em dash, en dash, hyphen, colon

RANGE_RE = re.compile(r"^(\d{3})(?:-(\d{3}))?$")
SECTION_RE = re.compile(r"^## (SL-EP-(\d{3}))\s*(.*)$")
JUST_RE = re.compile(r"\bjust\s+([A-Za-z][\w-]*)")
ADR_RE = re.compile(r"\bADR-(\d{4})\b")
NONE_RE = re.compile(r"\bnone\b", re.IGNORECASE)


def parse_range(text):
    """'041-048' -> (41, 48); '041' -> (41, 41); None when malformed."""
    m = RANGE_RE.match(text or "")
    if not m:
        return None
    lo = int(m.group(1))
    hi = int(m.group(2)) if m.group(2) else lo
    if not 1 <= lo <= hi <= 840:
        return None
    return lo, hi


def split_sections(text):
    """Return [(ep_number, title_text, line, subs)] with subs = [(heading, body_text)], in file order."""
    sections = []
    cur = None
    sub = None
    in_fence = False
    for n, line in enumerate(text.splitlines(), start=1):
        if line.startswith("```"):
            in_fence = not in_fence
        if not in_fence and line.startswith("## "):
            m = SECTION_RE.match(line)
            sub = None
            if m:
                rest = m.group(3).strip()
                for d in DASHES:
                    if rest.startswith(d):
                        rest = rest[len(d):].strip()
                        break
                cur = (int(m.group(2)), rest, n, [])
                sections.append(cur)
            else:
                cur = None  # a non-episode section (for example the transcripts) ends the previous one
            continue
        if cur is None:
            continue
        if not in_fence and line.startswith("### "):
            sub = [line[4:].strip(), []]
            cur[3].append(sub)
            continue
        if sub is not None:
            sub[1].append(line)
    return [(n, t, ln, [(h, "\n".join(b).strip()) for h, b in subs]) for n, t, ln, subs in sections]


def check_text(text, episodes, recipes, adrs, lo, hi):
    """Pure check: episodes = {number: title}, recipes = set of justfile recipe names, adrs = set of 'NNNN'."""
    errors = []
    sections = split_sections(text)
    seen = {}
    for number, title, line, subs in sections:
        label = "SL-EP-%03d" % number
        if not lo <= number <= hi:
            errors.append("R-EPDOC-SECTION: line %d: section %s is outside the range %03d-%03d" % (line, label, lo, hi))
            continue
        if number in seen:
            errors.append(
                "R-EPDOC-SECTION: line %d: %s has a second section (first at line %d)" % (line, label, seen[number])
            )
            continue
        seen[number] = line
        want = episodes.get(number)
        if want is not None and title != want:
            errors.append(
                "R-EPDOC-SECTION: line %d: %s title is '%s', the registry title is '%s'" % (line, label, title, want)
            )
        names = tuple(h for h, _ in subs)
        if names != SUBHEADINGS:
            errors.append(
                "R-EPDOC-HEADINGS: %s: sub-headings are %s, want exactly %s" % (label, list(names), list(SUBHEADINGS))
            )
            continue
        bodies = dict(subs)
        for h in SUBHEADINGS:
            if not bodies[h]:
                errors.append("R-EPDOC-HEADINGS: %s: '### %s' has no text" % (label, h))
        used = JUST_RE.findall(bodies["Acceptance"])
        if not used:
            errors.append("R-EPDOC-ACCEPT: %s: Acceptance names no `just <recipe>`" % label)
        for r in sorted(set(used)):
            if r not in recipes:
                errors.append(
                    "R-EPDOC-ACCEPT: %s: Acceptance names `just %s`, which is not a justfile recipe" % (label, r)
                )
        ids = ADR_RE.findall(bodies["Decisions"])
        if not ids and not NONE_RE.search(bodies["Decisions"]):
            errors.append("R-EPDOC-DECISIONS: %s: Decisions names no ADR-NNNN and does not say 'none'" % label)
        for i in sorted(set(ids)):
            if i not in adrs:
                errors.append("R-EPDOC-DECISIONS: %s: ADR-%s does not exist in docs/adr/" % (label, i))
    for number in range(lo, hi + 1):
        if number not in seen:
            errors.append("R-EPDOC-SECTION: no section '## SL-EP-%03d <dash> <title>'" % number)
    return errors


def load_inputs(root):
    """(episodes {number: title}, recipes set, adrs set) read from disk; raises OSError/ValueError."""
    with open(os.path.join(root, REGISTRY_REL), encoding="utf-8") as fh:
        reg = json.load(fh)
    episodes = {int(e["number"]): e["title"] for e in reg["episodes"]}
    with open(os.path.join(root, "justfile"), encoding="utf-8") as fh:
        recipes = set(slgov.parse_justfile(fh.read()))
    adr_dir = os.path.join(root, "docs", "adr")
    adrs = set()
    for name in os.listdir(adr_dir) if os.path.isdir(adr_dir) else ():
        m = re.match(r"^ADR-(\d{4})-.*\.md$", name)
        if m:
            adrs.add(m.group(1))
    return episodes, recipes, adrs


def run(rng, root):
    parsed = parse_range(rng)
    if parsed is None:
        return ["R-EPDOC-RANGE: '%s' is not NNN-MMM or NNN within 001-840" % rng]
    lo, hi = parsed
    try:
        episodes, recipes, adrs = load_inputs(root)
    except (OSError, ValueError, KeyError) as exc:
        return ["R-EPDOC-RANGE: cannot read the registry, justfile or docs/adr: %s" % exc]
    missing = [n for n in range(lo, hi + 1) if n not in episodes]
    if missing:
        return ["R-EPDOC-RANGE: SL-EP-%03d is not in %s" % (missing[0], REGISTRY_REL)]
    rel = "docs/episodes/%03d-%03d.md" % (lo, hi) if hi != lo else "docs/episodes/%03d.md" % lo
    path = os.path.join(root, rel)
    if not os.path.isfile(path):
        return ["R-EPDOC-FILE: %s does not exist" % rel]
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    return check_text(text, episodes, recipes, adrs, lo, hi)


def selftest():
    """In-memory planted defects: each must fail with exactly its rule id; the control must pass."""
    eps = {1: "Alpha", 2: "Beta"}
    good = (
        "## SL-EP-001 — Alpha\n### Done\nx\n### Decisions\nADR-0001\n### Acceptance\n`just fast` exit 0\n"
        "### Evidence\nbuild/x\n### Open issues\nnone\n"
        "## SL-EP-002 — Beta\n### Done\nx\n### Decisions\nnone\n### Acceptance\n`just fast` exit 0\n"
        "### Evidence\nbuild/x\n### Open issues\nnone\n"
    )
    plants = {
        "R-EPDOC-SECTION": good.replace("— Beta", "— Gamma"),
        "R-EPDOC-HEADINGS": good.replace(
            "### Evidence\nbuild/x\n### Open issues", "### Open issues\nnone\n### Evidence\nbuild/x\n### Open issues", 1
        ),
        "R-EPDOC-ACCEPT": good.replace("`just fast`", "`just nope`", 1),
        "R-EPDOC-DECISIONS": good.replace("ADR-0001", "ADR-0099", 1),
    }
    errors = check_text(good, eps, {"fast"}, {"0001"}, 1, 2)
    if errors:
        return ["selftest: control failed: %s" % errors]
    out = []
    for rule, text in plants.items():
        got = {e.split(":", 1)[0] for e in check_text(text, eps, {"fast"}, {"0001"}, 1, 2)}
        if got != {rule}:
            out.append("selftest: planted %s produced %s" % (rule, sorted(got)))
    return out


def main(argv):
    args = argv[1:]
    root = slgov.ROOT
    if "--root" in args:
        i = args.index("--root")
        if i + 1 >= len(args):
            print("usage: cmd_episodes_doc_check.py <NNN-MMM> [--root DIR] [--selftest]", file=sys.stderr)
            return 2
        root = args[i + 1]
        del args[i : i + 2]
    if args == ["--selftest"]:
        return slgov.report("episodes-doc-check selftest", selftest())
    if len(args) != 1:
        print("usage: cmd_episodes_doc_check.py <NNN-MMM> [--root DIR] [--selftest]", file=sys.stderr)
        return 2
    return slgov.report("episodes-doc-check %s" % args[0], run(args[0], root))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
