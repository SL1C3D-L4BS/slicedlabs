"""fabric-ownership-check: the F2 measure over the domain registry (WO-048, ADR-0017, FAB-INV-OWN-08).

F2 (master plan section 3; SL-GOV-006): numerator = data domains with more than one registered owner, denominator =
all domains, threshold 0. Inputs: the C++ registry dump written by `sl_fabric_dump_domains` (format
sl-fabric-domains/1), the `schemas/fabric/*.sltype.json` modules (module domain and type domains, owned by the
module's `owner`) and the SL-ARCH-001 sl-data tables `domain_registry`, `fabric_domains`, `actor_kinds` and
`default_deny`. Each error line names its rule:

  R-F2-DUMP      the dump is unreadable, not sl-fabric-domains/1, or malformed
  R-F2-ARCH      SL-ARCH-001 or a schema module cannot be read
  R-F2-RESERVED  the dump's reserved prefixes differ from `domain_registry` (row by row: prefix, owner, owns,
                 state classes), or a subsystem 01..12 is missing or repeated
  R-F2-TABLES    the dump's fabric_domains, actor_kinds or default_deny differ from SL-ARCH-001
  R-F2-ORPHAN    a claimed domain is not a dotted name under a reserved prefix, or a `fabric.*` domain lies within
                 no domain the engine dump claims (Fabric's own domains are claimed in C++)
  R-F2-STATE     a dump domain declares a state class its prefix does not admit
  R-F2-DUP       a domain with more than one owner; the claimants are the dump, every schema module, and the
                 subsystem the prefix is reserved to; the line names every claimant and owner
  R-F2-REJECTED  the dump lists a claim the engine refused at start-up (it names claimant and holder)
  R-F2-COUNT     the dump's own f2 report disagrees (numerator not 0, denominator not its domain count)

The output ends with `f2 numerator N denominator D` and `fabric-ownership-check: ok` (exit 0) or `failed` (exit 1).
Stdlib only, no subprocess (ADR-0004); run as `python3 -I tools/fabric/ownership_check.py <domains.json> [--root DIR]`.
"""

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "governance"))
import cmd_constitution_check  # noqa: E402

DUMP_FORMAT = "sl-fabric-domains/1"
MODULE_DIR = "schemas/fabric"
MODULE_SUFFIX = ".sltype.json"
DOMAIN_RE = re.compile(r"^[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)+$")
ARCH_KEYS = ("domain_registry", "fabric_domains", "actor_kinds", "default_deny")
OWNERS = ["%02d" % n for n in range(1, 13)]


def load_arch(root):
    """Return (tables, errors): the SL-ARCH-001 sl-data tables named in ARCH_KEYS."""
    arch_dir = os.path.join(root, "docs", "arch")
    names = sorted(os.listdir(arch_dir)) if os.path.isdir(arch_dir) else []
    found = [n for n in names if n.startswith("SL-ARCH-001-") and n.endswith(".md")]
    if not found:
        return None, ["docs/arch/SL-ARCH-001-*.md: R-F2-ARCH: the canonical project model is missing"]
    rel = "docs/arch/" + found[0]
    try:
        with open(os.path.join(arch_dir, found[0]), encoding="utf-8") as fh:
            text = fh.read()
    except (OSError, UnicodeDecodeError) as exc:
        return None, ["%s: R-F2-ARCH: unreadable: %s" % (rel, exc)]
    blocks, bad = cmd_constitution_check.sl_data_blocks(text)
    errors = ["%s: R-F2-ARCH: %s" % (rel, b) for b in bad]
    merged = {}
    for block in blocks:
        if isinstance(block, dict):
            merged.update(block)
    for key in ARCH_KEYS:
        if key not in merged:
            errors.append("%s: R-F2-ARCH: no sl-data block holds `%s`" % (rel, key))
    if errors:
        return None, errors
    return {key: merged[key] for key in ARCH_KEYS}, []


def load_modules(root):
    """Return (modules, errors): [(rel, module dict)] for every schema module, in file name order."""
    directory = os.path.join(root, *MODULE_DIR.split("/"))
    names = sorted(n for n in os.listdir(directory) if n.endswith(MODULE_SUFFIX)) if os.path.isdir(directory) else []
    modules, errors = [], []
    for name in names:
        rel = "%s/%s" % (MODULE_DIR, name)
        try:
            with open(os.path.join(directory, name), encoding="utf-8") as fh:
                doc = json.load(fh)
        except (OSError, UnicodeDecodeError, ValueError) as exc:
            errors.append("%s: R-F2-ARCH: unreadable module: %s" % (rel, exc))
            continue
        if not isinstance(doc, dict) or not isinstance(doc.get("domain"), str) or doc.get("owner") not in OWNERS \
                or not isinstance(doc.get("types", []), list):
            errors.append("%s: R-F2-ARCH: a module needs a string domain, an owner 01..12 and a types list" % rel)
            continue
        modules.append((rel, doc))
    return modules, errors


def load_dump(path):
    """Return (dump, errors)."""
    try:
        with open(path, encoding="utf-8") as fh:
            dump = json.load(fh)
    except (OSError, UnicodeDecodeError, ValueError) as exc:
        return None, ["%s: R-F2-DUMP: unreadable: %s" % (path, exc)]
    keys = ("reserved", "fabric_domains", "actor_kinds", "default_deny", "domains", "rejected", "f2")
    if not isinstance(dump, dict) or dump.get("format") != DUMP_FORMAT:
        return None, ["%s: R-F2-DUMP: not a %s document" % (path, DUMP_FORMAT)]
    missing = [k for k in keys if k not in dump]
    if missing:
        return None, ["%s: R-F2-DUMP: missing %s" % (path, ", ".join(missing))]
    for row in dump["domains"]:
        if not isinstance(row, dict) or not isinstance(row.get("name"), str) or not isinstance(row.get("owner"), str):
            return None, ["%s: R-F2-DUMP: a domain row needs a name and an owner: %r" % (path, row)]
    return dump, []


def _reserved_rules(dump, arch, src, errors):
    want = arch["domain_registry"]
    got = dump["reserved"]
    if len(got) != len(want):
        errors.append("%s: R-F2-RESERVED: the dump has %d reserved prefixes, domain_registry has %d"
                      % (src, len(got), len(want)))
    for n, (g, w) in enumerate(zip(got, want), start=1):
        for key in ("prefix", "owner", "owns", "state_classes"):
            if g.get(key) != w.get(key):
                errors.append("%s: R-F2-RESERVED: row %d (%s) %s is %r in the dump, %r in domain_registry"
                              % (src, n, w.get("prefix"), key, g.get(key), w.get(key)))
    owners = sorted(row.get("owner") for row in got)
    if owners != OWNERS:
        errors.append("%s: R-F2-RESERVED: the reserved owners are %s, not one each of 01..12"
                      % (src, ", ".join(str(o) for o in owners)))
    for key in ("fabric_domains", "actor_kinds", "default_deny"):
        if dump[key] != arch[key]:
            errors.append("%s: R-F2-TABLES: %s is %r in the dump, %r in SL-ARCH-001" % (src, key, dump[key], arch[key]))


def _claims(dump, modules, src):
    """Return {domain: [(source, owner)]}: the dump's domains, then each module's domain and type domains."""
    claims = {}
    for row in dump["domains"]:
        claims.setdefault(row["name"], []).append((src, row["owner"]))
    for rel, module in modules:
        owner = module["owner"]
        domains = {module["domain"]}
        for t in module.get("types", []):
            if isinstance(t, dict) and isinstance(t.get("domain"), str) and t["domain"]:
                domains.add(t["domain"])
        for domain in sorted(domains):
            claims.setdefault(domain, []).append((rel, owner))
    return claims


def check(dump_path, root):
    """Return (lines, numerator, denominator): error lines (empty when clean) and the F2 measure."""
    arch, errors = load_arch(root)
    dump, dump_errors = load_dump(dump_path)
    errors += dump_errors
    modules, module_errors = load_modules(root)
    errors += module_errors
    if arch is None or dump is None:
        return errors, None, None
    src = dump_path
    if os.path.isabs(dump_path) and not os.path.relpath(dump_path, root).startswith(".."):
        src = os.path.relpath(dump_path, root)
    _reserved_rules(dump, arch, src, errors)
    registry = {row["prefix"]: row for row in arch["domain_registry"]}
    for row in dump["domains"]:
        prefix = registry.get(row["name"].split(".")[0])
        if prefix is not None:
            for sc in row.get("state_classes", []):
                if sc not in prefix["state_classes"]:
                    errors.append("%s: R-F2-STATE: domain %s declares state class %s, prefix %s admits %s"
                                  % (src, row["name"], sc, prefix["prefix"], ", ".join(prefix["state_classes"])))
    claims = _claims(dump, modules, src)
    engine = {row["name"] for row in dump["domains"]}
    numerator = 0
    for domain in sorted(claims):
        prefix = registry.get(domain.split(".")[0])
        if not DOMAIN_RE.match(domain) or prefix is None:
            errors.append("%s: R-F2-ORPHAN: domain %s is not a dotted name under a reserved prefix (%s)"
                          % (claims[domain][0][0], domain, ", ".join(sorted(registry))))
            continue
        if prefix["prefix"] == "fabric" and not any(domain == d or domain.startswith(d + ".") for d in engine):
            errors.append("%s: R-F2-ORPHAN: domain %s is under the fabric prefix but within no domain the engine "
                          "claims (%s)" % (claims[domain][0][0], domain, ", ".join(sorted(engine))))
        claimants = sorted(set(claims[domain])) + [("SL-ARCH-001 domain_registry prefix %s" % prefix["prefix"],
                                                    prefix["owner"])]
        owners = sorted({owner for _source, owner in claimants})
        if len(owners) > 1:
            numerator += 1
            errors.append("%s: R-F2-DUP: domain %s has %d owners: %s"
                          % (claims[domain][0][0], domain, len(owners),
                             "; ".join("%s owner %s" % (s, o) for s, o in claimants)))
    for r in dump["rejected"]:
        errors.append("%s: R-F2-REJECTED: claim of %s by %s refused (%s, holder %s): %s"
                      % (src, r.get("domain"), r.get("claimant"), r.get("code"), r.get("holder"), r.get("message")))
    f2 = dump["f2"]
    if f2.get("numerator") != 0 or f2.get("denominator") != len(dump["domains"]):
        errors.append("%s: R-F2-COUNT: the dump reports numerator %r denominator %r over %d domains"
                      % (src, f2.get("numerator"), f2.get("denominator"), len(dump["domains"])))
    return errors, numerator, len(claims)


def main(argv):
    args = list(argv)
    root = ROOT
    if "--root" in args:
        i = args.index("--root")
        if i + 1 >= len(args):
            print("usage: ownership_check.py <domains.json> [--root DIR]", file=sys.stderr)
            return 2
        root = os.path.abspath(args[i + 1])
        del args[i:i + 2]
    if len(args) != 1:
        print("usage: ownership_check.py <domains.json> [--root DIR]", file=sys.stderr)
        return 2
    lines, numerator, denominator = check(args[0], root)
    for line in lines:
        print(line)
    if numerator is not None:
        print("f2 numerator %d denominator %d" % (numerator, denominator))
    print("fabric-ownership-check: %s" % ("failed" if lines else "ok"))
    return 1 if lines else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
