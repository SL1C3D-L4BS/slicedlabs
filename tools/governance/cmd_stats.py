"""registry-stats: counts per lifecycle state and per volume from registry/SL-REG-002-episodes.json (WO-004).

    python3 -I cmd_stats.py [--root DIR] [--registry FILE]

Deterministic plain text: totals, episodes per state (master plan 6.2 order), then one line per volume with
its range, per-state counts, title-only (null summary) episodes and the volumes it depends on.
Exit 0 on success, 2 when the registry cannot be read. Stdlib only (ADR-0004).
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmd_gen_episodes as gen  # noqa: E402
import slib  # noqa: E402


def render(reg):
    eps = reg["episodes"]
    lines = ["registry-stats: %d episodes, %d volumes, %d parts (source %s %s)"
             % (len(eps), len(reg["volumes"]), len(reg["parts"]), reg["source"]["plan"], reg["source"]["sha256"][:12])]
    by_state = {s: 0 for s in slib.STATES}
    for e in eps:
        by_state[e["status"]] += 1
    lines.append("by state: " + ", ".join("%s %d" % (s, n) for s, n in by_state.items()))
    decisions = {}
    for e in eps:
        decisions[e["decision_status"]] = decisions.get(e["decision_status"], 0) + 1
    lines.append("by decision status: " + ", ".join("%s %d" % (k, decisions[k]) for k in sorted(decisions)))
    lines.append("title-only (null summary): %d; with work orders: %d; with kohi_ref: %d"
                 % (sum(1 for e in eps if e["summary"] is None), sum(1 for e in eps if e["work_orders"]),
                    sum(1 for e in eps if e["kohi_ref"])))
    lines.append("")
    lines.append("volume  episodes   " + "  ".join("%-4s" % s[:4] for s in slib.STATES) + "  null  depends_on")
    for v in reg["volumes"]:
        members = [e for e in eps if e["volume"] == v["id"]]
        counts = {s: 0 for s in slib.STATES}
        for e in members:
            counts[e["status"]] += 1
        lines.append("%s  %03d-%03d    %s  %4d  %s" % (
            v["id"], v["first"], v["last"], "  ".join("%4d" % counts[s] for s in slib.STATES),
            sum(1 for e in members if e["summary"] is None),
            ",".join(d[-2:] for d in v["depends_on_volumes"]) or "-"))
    return "\n".join(lines) + "\n"


def main(argv):
    args = argv[1:]
    root, registry = slib.ROOT, None
    i = 0
    while i < len(args):
        if args[i] == "--root" and i + 1 < len(args):
            i += 1
            root = os.path.abspath(args[i])
        elif args[i] == "--registry" and i + 1 < len(args):
            i += 1
            registry = os.path.abspath(args[i])
        else:
            print("usage: cmd_stats.py [--root DIR] [--registry FILE]", file=sys.stderr)
            return 2
        i += 1
    try:
        reg = slib.load_json(registry or os.path.join(root, gen.REG_REL))
    except slib.LoadError as exc:
        print("registry-stats: %s" % exc, file=sys.stderr)
        return 2
    sys.stdout.write(render(reg))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
