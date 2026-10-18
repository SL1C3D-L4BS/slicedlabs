"""Shared helpers for the governance gates.

Stdlib only, no subprocess, no network (ADR-0004). Run the command scripts with `python3 -I`.
Every check returns a list of error strings; an empty list means the check passed.
"""

import json
import os
import re
import stat

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Forbidden in recipes and scripts: fetching from the network (agents never fetch).
# Built from pieces so this file does not trip its own scan.
FORBIDDEN_FETCH = re.compile(r"\b(?:cu" + r"rl|wg" + r"et)\b|\bgit\s+fe" + r"tch\b")

# The sandbox stub block that .gitignore must carry (lines, matched exactly).
GITIGNORE_REQUIRED = ("/.bashrc", "/.claude/*")

# Work orders known to have landed without an evidence file (genesis and this skeleton).
LANDED_BASELINE = ("WO-000", "WO-001")


class FrontMatterError(ValueError):
    """Raised when a front matter block does not parse."""


# --- front matter: the small YAML subset used by FACTORY.md and work orders -------------------


def _strip_comment(s):
    """Drop a trailing `# comment` (a # at line start or after whitespace, outside quotes)."""
    quote = None
    for i, ch in enumerate(s):
        if quote:
            if ch == quote:
                quote = None
        elif ch in "\"'":
            quote = ch
        elif ch == "#" and (i == 0 or s[i - 1] in " \t"):
            return s[:i]
    return s


def _split_flow(inner):
    items, cur, quote = [], "", None
    for ch in inner:
        if quote:
            cur += ch
            if ch == quote:
                quote = None
        elif ch in "\"'":
            quote = ch
            cur += ch
        elif ch == ",":
            items.append(cur)
            cur = ""
        else:
            cur += ch
    items.append(cur)
    return [_scalar(i) for i in items if i.strip()]


def _scalar(raw):
    v = raw.strip()
    if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
        return v[1:-1]
    if v.startswith("[") and v.endswith("]"):
        return _split_flow(v[1:-1])
    return v


def split_front_matter(text):
    """Return (front matter lines, body text). Raises FrontMatterError when absent or open."""
    lines = text.splitlines()
    if not lines or lines[0].strip() != "---":
        raise FrontMatterError("front matter must start with a '---' line")
    for i in range(1, len(lines)):
        if lines[i].strip() == "---":
            return lines[1:i], "\n".join(lines[i + 1 :])
    raise FrontMatterError("front matter is not closed by a '---' line")


_KEY_VALUE = re.compile(r"^([A-Za-z_][\w.-]*)\s*:(?:\s+(.*))?$")


def parse_front_matter(text):
    """Parse `key: value`, flow lists, and one level of nested map or `- item` list."""
    lines, _ = split_front_matter(text)
    result = {}
    entries = []  # (indent, content, lineno)
    for n, line in enumerate(lines, start=2):
        content = _strip_comment(line).rstrip()
        if not content.strip():
            continue
        entries.append((len(content) - len(content.lstrip(" ")), content.strip(), n))
    i = 0
    while i < len(entries):
        indent, content, n = entries[i]
        if indent != 0:
            raise FrontMatterError("line %d: unexpected indentation" % n)
        m = _KEY_VALUE.match(content)
        if not m:
            raise FrontMatterError("line %d: expected 'key: value'" % n)
        key, value = m.group(1), m.group(2)
        if key in result:
            raise FrontMatterError("line %d: duplicate key '%s'" % (n, key))
        i += 1
        if value:
            result[key] = _scalar(value)
            continue
        block = []
        while i < len(entries) and entries[i][0] > 0:
            block.append(entries[i])
            i += 1
        if not block:
            result[key] = ""
        elif all(c.startswith("- ") or c == "-" for _, c, _ in block):
            result[key] = [_scalar(c[1:]) for _, c, _ in block]
        else:
            nested = {}
            for _, c, bn in block:
                mm = _KEY_VALUE.match(c)
                if not mm:
                    raise FrontMatterError("line %d: expected 'key: value'" % bn)
                nested[mm.group(1)] = _scalar(mm.group(2) or "")
            result[key] = nested
    return result


# --- justfile ---------------------------------------------------------------------------------

_HEADER = re.compile(
    r"^@?([A-Za-z_][\w-]*)((?:[ \t]+[+*$]?[A-Za-z_]\w*(?:=(?:'[^']*'|\"[^\"]*\"|\S+))?)*)"
    r"[ \t]*:(?!=)[ \t]*(.*)$"
)


def parse_justfile(text):
    """Map recipe name -> {params, deps, body, line}. Settings and assignments are skipped."""
    recipes = {}
    current = None
    for n, line in enumerate(text.splitlines(), start=1):
        if line[:1] in (" ", "\t"):
            if current is not None and line.strip():
                current["body"].append(line.strip())
            continue
        if not line.strip() or line.startswith("#"):
            continue
        current = None
        m = _HEADER.match(line)
        if not m or m.group(1) == "set":
            continue
        name = m.group(1)
        deps_part = m.group(3).split("&&")[0]
        deps = [d for d in deps_part.split() if re.match(r"^[A-Za-z_][\w-]*$", d)]
        current = {"params": m.group(2).split(), "deps": deps, "body": [], "line": n}
        recipes[name] = current
    return recipes


def stub_target(recipe):
    """Return the name a stub recipe passes to `just stub`, or None if it is not a stub."""
    body = [b.lstrip("@") for b in recipe["body"]]
    if len(body) == 1 and "just" in body[0]:
        m = re.search(r"\bstub\s+([A-Za-z_][\w-]*)\s*$", body[0])
        if m:
            return m.group(1)
    return None


def closure(recipes, roots):
    """All recipes reachable from roots through dependencies (roots included)."""
    seen, todo = set(), list(roots)
    while todo:
        name = todo.pop()
        if name in seen or name not in recipes:
            continue
        seen.add(name)
        todo.extend(recipes[name]["deps"])
    return seen


# --- recipes.json -----------------------------------------------------------------------------


def load_recipes_json(path):
    with open(path, encoding="utf-8") as fh:
        data = json.load(fh)
    return data["recipes"]


def consistency_errors(recipes, manifest):
    """justfile vs recipes.json: same names, valid owner/status, stub status matches the body."""
    errors = []
    for name in sorted(set(recipes) - set(manifest)):
        errors.append("recipe '%s' is in the justfile but not in recipes.json" % name)
    for name in sorted(set(manifest) - set(recipes)):
        errors.append("recipe '%s' is in recipes.json but not in the justfile" % name)
    for name in sorted(set(recipes) & set(manifest)):
        entry = manifest[name]
        if not re.match(r"^WO-\d{3}$", str(entry.get("owner", ""))):
            errors.append("recipe '%s': owner must look like WO-001" % name)
        status = entry.get("status")
        if status not in ("stub", "implemented"):
            errors.append("recipe '%s': status must be 'stub' or 'implemented'" % name)
            continue
        target = stub_target(recipes[name])
        if status == "stub" and target != name:
            errors.append("recipe '%s' is marked stub but its body is not `just stub %s`" % (name, name))
        if status == "implemented" and target is not None:
            errors.append("recipe '%s' is marked implemented but its body is a stub" % name)
    return errors


def landed_work_orders(root):
    """WOs known to have landed: the baseline plus any evidence/WO-NNN.json file."""
    landed = set(LANDED_BASELINE)
    evidence = os.path.join(root, "evidence")
    if os.path.isdir(evidence):
        for f in os.listdir(evidence):
            m = re.match(r"^(WO-\d{3})\.json$", f)
            if m:
                landed.add(m.group(1))
    return landed


def selftest_errors(recipes, manifest, landed, roots=("fast", "check")):
    """No recipe in the closure of the gate roots may be a stub whose owner has landed."""
    errors = consistency_errors(recipes, manifest)
    for root in roots:
        if root not in recipes:
            errors.append("gate recipe '%s' is missing from the justfile" % root)
    for name in sorted(closure(recipes, roots)):
        entry = manifest.get(name, {})
        if entry.get("status") == "stub" and entry.get("owner") in landed:
            errors.append(
                "recipe '%s' is a stub in the gate closure but its owner %s has landed"
                % (name, entry.get("owner"))
            )
    return errors


# --- layout-check -----------------------------------------------------------------------------


def layout_errors(root, layout, files):
    errors = []
    for d in layout.get("dirs", []):
        path = os.path.join(root, d)
        if not os.path.isdir(path):
            errors.append("missing directory: %s" % d)
        elif not os.path.isfile(os.path.join(path, "README.md")):
            errors.append("missing README.md in: %s" % d)
    for d in layout.get("exist_only", []):
        if not os.path.isdir(os.path.join(root, d)):
            errors.append("missing directory: %s" % d)
    for rel in files:
        if os.path.basename(rel) == ".gitmodules":
            errors.append("%s: submodules are not allowed (dependencies are vendored, ADR-0002)" % rel)
            continue
        try:
            st = os.lstat(os.path.join(root, rel))
        except OSError:
            continue  # tracked but deleted in the working tree: not this check's concern
        if not stat.S_ISREG(st.st_mode):
            errors.append("%s: tracked file is not a regular file (sandbox stub?)" % rel)
        elif st.st_size == 0 and os.path.basename(rel) != "__init__.py":
            errors.append("%s: tracked 0-byte file" % rel)  # an empty __init__.py is a package marker, not a stub
    return errors


# --- gates-check ------------------------------------------------------------------------------


def gate_errors(root):
    errors = []
    try:
        with open(os.path.join(root, "FACTORY.md"), encoding="utf-8") as fh:
            meta = parse_front_matter(fh.read())
    except (OSError, FrontMatterError) as exc:
        return ["FACTORY.md: %s" % exc]
    with open(os.path.join(root, "justfile"), encoding="utf-8") as fh:
        justfile_text = fh.read()
    recipes = parse_justfile(justfile_text)
    gate = meta.get("gate")
    if not isinstance(gate, dict):
        errors.append("FACTORY.md: missing 'gate' map")
        gate = {}
    for key in ("fast", "full"):
        cmd = gate.get(key, "")
        m = re.match(r"^just\s+([A-Za-z_][\w-]*)(?:\s+\S+)*$", cmd or "")
        if not m:
            errors.append("FACTORY.md: gate.%s must be `just <recipe>`, got %r" % (key, cmd))
        elif m.group(1) not in recipes:
            errors.append(
                "FACTORY.md: gate.%s names recipe '%s' which is not in the justfile" % (key, m.group(1))
            )
    if not isinstance(meta.get("governed"), list) or not meta["governed"]:
        errors.append("FACTORY.md: 'governed' must be a non-empty list")
    try:
        with open(os.path.join(root, ".gitignore"), encoding="utf-8") as fh:
            ignored = {line.strip() for line in fh}
    except OSError:
        ignored = set()
    for entry in GITIGNORE_REQUIRED:
        if entry not in ignored:
            errors.append(".gitignore: missing sandbox stub entry '%s'" % entry)
    errors.extend(fetch_errors(root, justfile_text))
    return errors


def fetch_errors(root, justfile_text):
    """Scan the justfile, scripts/ and tests/gates/ for network fetches."""
    scanned = [("justfile", justfile_text)]
    for sub in ("scripts", os.path.join("tests", "gates")):
        for dirpath, _, names in os.walk(os.path.join(root, sub)):
            for name in sorted(names):
                path = os.path.join(dirpath, name)
                try:
                    with open(path, encoding="utf-8") as fh:
                        scanned.append((os.path.relpath(path, root), fh.read()))
                except (OSError, UnicodeDecodeError):
                    continue
    errors = []
    for rel, text in scanned:
        for n, line in enumerate(text.splitlines(), start=1):
            if FORBIDDEN_FETCH.search(line):
                errors.append("%s:%d: recipes and scripts must not fetch from the network" % (rel, n))
    return errors


# --- adr-check --------------------------------------------------------------------------------


def report(label, errors):
    """Print the outcome of a check; return the process exit code."""
    if errors:
        for e in errors:
            print("%s: FAIL: %s" % (label, e))
        return 1
    print("%s: ok" % label)
    return 0
