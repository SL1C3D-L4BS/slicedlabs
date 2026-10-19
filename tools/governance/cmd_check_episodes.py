"""registry-check: validate registry/SL-REG-002-episodes.json (WO-004).

    python3 -I cmd_check_episodes.py [--root DIR] [--registry FILE]
    python3 -I cmd_check_episodes.py --mutation-selftest      plant defects in a copy; each must be named

Runs the slib rule engine on the real registry (R-SCHEMA R-DUP-ID R-GAP R-VOLGRAPH R-DEP-ORDER R-LIFECYCLE
R-NO-EVIDENCE R-SILENT-SCOPE R-PLAN-SHA R-DEP-ACCEPTED; R-DOC-MISSING, R-WO-TRACE and R-VIEW-DRIFT need the
document register, work-order traces and views, which later work orders supply) plus golden asserts:
840 episodes, 42 volumes of 20, 7 parts, the volume graph equal to master plan 4.2, 301 title-only
episodes, the plan hash equal to its pin, the ADR-0003 and ADR-0021 waivers, the volume gates (ADR-0020,
ADR-0021), the immutable-projection lock and the governed accepted stub. R-VOL-GATE (defined here, not in slib):
an episode at or beyond a gated state while the gate's required episode range is not at its required status.
Prints the wall time. Exit 0 clean, 1 findings, 2 usage or load error. Stdlib only (ADR-0004).
"""

import copy
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmd_gen_episodes as gen  # noqa: E402
import cmd_views  # noqa: E402
import slib  # noqa: E402

GOLDEN_EPISODES = 840
GOLDEN_VOLUMES = 42
GOLDEN_VOLUME_SIZE = 20
GOLDEN_NULL_SUMMARIES = 301
GOLDEN_PARTS = ((1, 1, 100), (2, 101, 200), (3, 201, 300), (4, 301, 400), (5, 401, 500), (6, 501, 640), (7, 641, 840))


def golden(reg, plan, plan_actual, plan_pinned, lock_text, accepted):
    """Golden asserts. Returns [(tag, path, message)]; tag is GOLDEN, LOCK or ACCEPTED."""
    out = []

    def bad(tag, path, message):
        out.append((tag, path, message))

    eps, vols = reg["episodes"], reg["volumes"]
    if len(eps) != GOLDEN_EPISODES:
        bad("GOLDEN", "/episodes", "expected %d episodes, found %d" % (GOLDEN_EPISODES, len(eps)))
    if len(vols) != GOLDEN_VOLUMES:
        bad("GOLDEN", "/volumes", "expected %d volumes, found %d" % (GOLDEN_VOLUMES, len(vols)))
    counts = {}
    for e in eps:
        counts[e["volume"]] = counts.get(e["volume"], 0) + 1
    for i, v in enumerate(vols):
        size = v["last"] - v["first"] + 1
        if size != GOLDEN_VOLUME_SIZE or counts.get(v["id"], 0) != GOLDEN_VOLUME_SIZE:
            bad("GOLDEN", "/volumes/%d" % i, "%s must hold %d episodes (range %d, listed %d)"
                % (v["id"], GOLDEN_VOLUME_SIZE, size, counts.get(v["id"], 0)))
    found_parts = tuple((p["number"], p["first"], p["last"]) for p in reg["parts"])
    if found_parts != GOLDEN_PARTS:
        bad("GOLDEN", "/parts", "expected parts %s, found %s" % (list(GOLDEN_PARTS), list(found_parts)))
    nulls = sum(1 for e in eps if e["summary"] is None)
    if nulls != GOLDEN_NULL_SUMMARIES:
        bad(
            "GOLDEN",
            "/episodes",
            "expected %d title-only episodes (null summary), found %d" % (GOLDEN_NULL_SUMMARIES, nulls),
        )
    # the volume dependency graph equals master plan 4.2
    want = {}
    for src, dst in plan["edges"]:
        want.setdefault(gen.vol_id(dst), set()).add(gen.vol_id(src))
    for i, v in enumerate(vols):
        have = set(v["depends_on_volumes"])
        if have != want.get(v["id"], set()):
            bad("GOLDEN", "/volumes/%d/depends_on_volumes" % i, "%s depends on %s but master plan 4.2 says %s"
                % (v["id"], sorted(have), sorted(want.get(v["id"], set()))))
    # the plan itself
    if plan_actual != plan_pinned:
        bad(
            "GOLDEN",
            "/source/sha256",
            "%s hashes to %s, pinned %s" % (gen.PLAN_REL, plan_actual[:12], plan_pinned[:12]),
        )
    if reg["source"]["sha256"] != plan_actual:
        bad(
            "GOLDEN",
            "/source/sha256",
            "registry source sha256 %s is not the plan's %s" % (reg["source"]["sha256"][:12], plan_actual[:12]),
        )
    # the waivers: ADR-0003 (041-048 stop at Demonstrated until 020 is Accepted) and ADR-0021 (081-100 stop at
    # Demonstrated until 021-040 are Accepted)
    if list(reg["lifecycle"]["waivers"]) != list(gen.WAIVERS):
        bad(
            "GOLDEN",
            "/lifecycle/waivers",
            "waivers must be exactly the ADR-0003 waiver for SL-EP-041..SL-EP-048 and the ADR-0021 waiver for "
            "SL-EP-081..SL-EP-100 (both max_status Demonstrated)",
        )
    # the volume gates (ADR-0020 cap on 061-080, ADR-0021 on 081-100 and 121-140)
    if list(reg["lifecycle"].get("gates", [])) != list(gen.GATES):
        bad(
            "GOLDEN",
            "/lifecycle/gates",
            "gates must be exactly ADR-0020 (SL-EP-061..080 Accepted needs 041..060 Accepted) and ADR-0021 "
            "(SL-EP-081..100 Accepted and SL-EP-121..140 InProgress need 021..040 Accepted)",
        )
    # the lock and the governed accepted stub
    if lock_text.strip() != gen.lock_digest(reg):
        bad(
            "LOCK",
            gen.LOCK_REL,
            "immutable projection (volumes id/range/title, episodes id/volume/title) no longer matches the lock; "
            "a scope change needs the founder to re-lock",
        )
    if not isinstance(accepted, dict):
        bad("ACCEPTED", gen.ACCEPTED_REL, "must be a JSON object")
    return out


def load_context(root, registry_path=None):
    """Everything the checks need, loaded once."""
    text, actual, pinned = gen.read_plan(root)
    with open(os.path.join(root, gen.LOCK_REL), encoding="utf-8") as fh:
        lock_text = fh.read()
    return {
        "root": root,
        "reg": slib.load_json(registry_path or os.path.join(root, gen.REG_REL)),
        "plan": gen.parse_plan(text),
        "plan_actual": actual,
        "plan_pinned": pinned,
        "lock_text": lock_text,
        "accepted": slib.load_json(os.path.join(root, gen.ACCEPTED_REL)),
    }


def vol_gate_findings(reg):
    """R-VOL-GATE: [(tag, path, message)] for every episode at or beyond a gated state while the required range
    is not at its required status. The gates checked are the registry's own (golden asserts they equal gen.GATES)."""
    out = []
    by_number = {e["number"]: e["status"] for e in reg["episodes"]}
    for gate in reg["lifecycle"].get("gates", []):
        floor = slib.PROGRESS.index(gate["state"])
        unmet = gen.gate_unmet(gate, by_number)
        if unmet is None:
            continue
        for i, ep in enumerate(reg["episodes"]):
            if gate["first"] <= ep["number"] <= gate["last"] and gen.gate_rank(ep["status"]) >= floor:
                out.append(
                    (
                        "R-VOL-GATE",
                        "/episodes/%d/status" % i,
                        "%s is %s but %s requires SL-EP-%03d..SL-EP-%03d to be %s before %s; SL-EP-%03d is %s"
                        % (ep["id"], ep["status"], gate["adr"], gate["requires_first"], gate["requires_last"],
                           gate["requires_status"], gate["state"], unmet, by_number.get(unmet, "missing")),
                    )
                )
    return out


def check(ctx, reg=None, lock_text=None):
    """[(tag, path, message)] for the registry (default: the loaded one)."""
    reg = ctx["reg"] if reg is None else reg
    lock_text = ctx["lock_text"] if lock_text is None else lock_text
    views = cmd_views.view_hashes(ctx["root"]) if reg is ctx["reg"] and "root" in ctx else None
    findings = slib.check_registry(reg, plan_sha256=ctx["plan_pinned"], views=views)
    out = [(f["rule"], f["path"], f["message"]) for f in findings]
    if any(tag == "R-SCHEMA" for tag, _, _ in out):
        return out  # the golden asserts index into well-formed data
    out.extend(vol_gate_findings(reg))
    out.extend(golden(reg, ctx["plan"], ctx["plan_actual"], ctx["plan_pinned"], lock_text, ctx["accepted"]))
    return out


# --- mutation self-test -----------------------------------------------------------------------


def _hist(*pairs):
    return [{"from": a, "to": b, "date": "2026-10-17"} for a, b in pairs]


def _m_schema(r):
    r["episodes"][0]["unplanned_field"] = 1


def _m_dup_id(r):
    r["episodes"][5]["id"] = r["episodes"][4]["id"]


def _m_gap(r):
    del r["episodes"][10]


def _m_cycle(r):
    r["volumes"][0]["depends_on_volumes"] = ["SL-VOL-03"]


def _m_dep_order(r):
    r["episodes"][3]["depends_on"] = ["SL-EP-010"]


def _m_lifecycle(r):
    r["episodes"][0]["status"] = "Specified"


def _m_scope(r):
    r["episodes"][0]["summary"] = (r["episodes"][0]["summary"] or "") + " (quietly widened)"


def _m_plan_sha(r):
    r["source"]["sha256"] = "0" * 64


def _m_dep_accepted(r):
    ep = r["episodes"][49]  # SL-EP-050 is outside the ADR-0003 waiver
    ep["depends_on"] = ["SL-EP-049"]
    ep["status"] = "InProgress"
    ep["history"] = _hist(("Planned", "Specified"), ("Specified", "InProgress"))


def _m_no_evidence(r):
    # The first still-Planned episode with no work orders, so the result does not depend on how far the real
    # registry has advanced; WO-999 has no evidence file and the completion record lists none (WO-070).
    ep = next(e for e in r["episodes"] if e["status"] == "Planned" and not e["work_orders"])
    ep["work_orders"] = ["WO-999"]
    ep["completion"] = None
    ep["status"] = "Demonstrated"
    ep["history"] = _hist(
        ("Planned", "Specified"),
        ("Specified", "InProgress"),
        ("InProgress", "Verifying"),
        ("Verifying", "Demonstrated"),
    )


def _m_waiver_gone(r):
    r["lifecycle"]["waivers"] = []


def _m_waiver_widened(r):
    r["lifecycle"]["waivers"][0]["max_status"] = "Accepted"


def _m_waiver_adr21_widened(r):
    r["lifecycle"]["waivers"][1]["max_status"] = "Accepted"


def _m_gates_gone(r):
    r["lifecycle"]["gates"] = []


def _m_gate_weakened(r):
    r["lifecycle"]["gates"][1]["requires_status"] = "Demonstrated"


def _unready(r, number):
    """Put an episode back to Planned so a gate requiring it Accepted is unmet whatever the real registry holds."""
    ep = r["episodes"][number - 1]
    ep["status"], ep["history"] = "Planned", []


def _m_gate_adr20_cap(r):
    _unready(r, 41)
    r["episodes"][60]["status"] = "Accepted"  # SL-EP-061 Accepted while SL-EP-041 is not (ADR-0020 cap)


def _m_gate_adr21_accepted(r):
    _unready(r, 21)
    r["episodes"][80]["status"] = "Accepted"  # SL-EP-081 Accepted while SL-EP-021 is not (ADR-0021)


def _m_gate_volume_07(r):
    _unready(r, 21)
    r["episodes"][120]["status"] = "InProgress"  # SL-EP-121 started while SL-EP-021 is not Accepted (ADR-0021)


def _m_extra_episode(r):
    ep = copy.deepcopy(r["episodes"][-1])
    ep["id"], ep["number"] = "SL-EP-841", 841
    r["episodes"].append(ep)


def _m_graph_edge(r):
    r["volumes"][5]["depends_on_volumes"] = ["SL-VOL-04", "SL-VOL-05", "SL-VOL-03"]


def _m_null_summary(r):
    ep = next(e for e in r["episodes"] if e["summary"] is None)
    ep["summary"] = "an invented summary"
    ep["meaning_sha256"] = slib.meaning_sha256(ep["title"], ep["summary"])


def _m_title_rewrite(r):
    ep = r["episodes"][100]
    ep["title"] = ep["title"] + " (renamed)"
    ep["meaning_sha256"] = slib.meaning_sha256(ep["title"], ep["summary"])


def _m_part_range(r):
    r["parts"][5]["last"] = 639
    r["parts"][6]["first"] = 640


MUTATIONS = (
    ("unknown field on an episode", "R-SCHEMA", _m_schema),
    ("duplicate episode id", "R-DUP-ID", _m_dup_id),
    ("episode number missing", "R-GAP", _m_gap),
    ("volume dependency cycle", "R-VOLGRAPH", _m_cycle),
    ("dependency on a later episode", "R-DEP-ORDER", _m_dep_order),
    ("Specified with no history", "R-LIFECYCLE", _m_lifecycle),
    ("summary widened without an amendment", "R-SILENT-SCOPE", _m_scope),
    ("registry source sha256 off the pin", "R-PLAN-SHA", _m_plan_sha),
    ("started before a dependency is Accepted", "R-DEP-ACCEPTED", _m_dep_accepted),
    ("Demonstrated without evidence", "R-NO-EVIDENCE", _m_no_evidence),
    ("ADR-0003 waiver removed", "GOLDEN", _m_waiver_gone),
    ("ADR-0003 waiver widened to Accepted", "GOLDEN", _m_waiver_widened),
    ("ADR-0021 waiver widened to Accepted", "GOLDEN", _m_waiver_adr21_widened),
    ("volume gates removed", "GOLDEN", _m_gates_gone),
    ("volume gate weakened to Demonstrated", "GOLDEN", _m_gate_weakened),
    ("061 Accepted before 041 (ADR-0020 cap)", "R-VOL-GATE", _m_gate_adr20_cap),
    ("081 Accepted before 021 (ADR-0021)", "R-VOL-GATE", _m_gate_adr21_accepted),
    ("121 InProgress before 021 Accepted (ADR-0021)", "R-VOL-GATE", _m_gate_volume_07),
    ("episode 841 added", "GOLDEN", _m_extra_episode),
    ("volume edge not in plan 4.2", "GOLDEN", _m_graph_edge),
    ("null summary invented", "GOLDEN", _m_null_summary),
    ("part range moved", "GOLDEN", _m_part_range),
    ("title rewritten with a fresh hash (lock)", "LOCK", _m_title_rewrite),
)


def mutation_selftest(ctx):
    clean = check(ctx)
    if clean:
        print("registry-check mutation selftest: FAIL: the unmodified registry is not clean (%d findings)" % len(clean))
        return 1
    failed = 0
    for label, tag, mutate in MUTATIONS:
        reg = copy.deepcopy(ctx["reg"])
        mutate(reg)
        found = sorted({t for t, _, _ in check(ctx, reg)})
        if tag in found:
            print("registry-check mutation selftest: %s: %s named" % (label, tag))
        else:
            failed += 1
            print(
                "registry-check mutation selftest: FAIL: %s: expected %s, found %s" % (label, tag, found or "nothing")
            )
    if failed:
        return 1
    print(
        "registry-check mutation selftest: ok (clean registry exit 0, %d planted defects each named)" % len(MUTATIONS)
    )
    return 0


def main(argv):
    args = argv[1:]
    root, registry, selftest = slib.ROOT, None, False
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--mutation-selftest":
            selftest = True
        elif a == "--root" and i + 1 < len(args):
            i += 1
            root = os.path.abspath(args[i])
        elif a == "--registry" and i + 1 < len(args):
            i += 1
            registry = os.path.abspath(args[i])
        else:
            print("usage: cmd_check_episodes.py [--root DIR] [--registry FILE] | --mutation-selftest", file=sys.stderr)
            return 2
        i += 1
    start = time.perf_counter()
    try:
        ctx = load_context(root, registry)
    except (slib.LoadError, gen.ParseError, OSError) as exc:
        print("registry-check: %s" % exc, file=sys.stderr)
        return 2
    if selftest:
        return mutation_selftest(ctx)
    findings = check(ctx)
    wall = time.perf_counter() - start
    for tag, path, message in findings[:60]:
        print("%s %s: %s" % (tag, path, message))
    if len(findings) > 60:
        print("... and %d more findings" % (len(findings) - 60))
    reg = ctx["reg"]
    if findings:
        print("registry-check: FAIL: %d findings in %.2fs" % (len(findings), wall))
        return 1
    print("registry-check: ok (%d episodes, %d volumes, %d parts, 11 rules + golden asserts) in %.2fs wall"
          % (len(reg["episodes"]), len(reg["volumes"]), len(reg["parts"]), wall))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
