#!/usr/bin/env python3
"""Format gate for every non-C++ source kind (WO-024). Stdlib only, no subprocess, no network (ADR-0004).

Run with `python3 -I tools/format/check.py --check|--fix [--files-from-stdin] [--list shell]`.

Kinds and rules (each finding prints as `kind:path:line: message`):
  cmake        CMakeLists.txt, *.cmake, *.cmake.in: no tabs, no trailing whitespace, final newline,
               2-space indentation (outside parentheses and bracket arguments)
  python       *.py and python-shebang scripts: compiles (in-process), lines <= 120, no trailing
               whitespace, final newline
  shell        *.sh and extensionless bash scripts under scripts/ and tests/gates/: no tab indent,
               no trailing whitespace, final newline (the recipe adds `shfmt -d` when installed)
  json         registry/**, deps/**, schemas/**, tools/**: equals json.dumps(indent=2,
               ensure_ascii=False) + newline; SL-REG-002 episodes/accepted use indent 1, sort_keys
  frontmatter  docs/**/SL-*.md: the DOC.md key order, every required key present

--fix rewrites what can be fixed (trailing whitespace, tabs in CMake, final newline, JSON drift)
and then re-checks. Files are listed on stdin (NUL separated, `git ls-files -z`) with
--files-from-stdin; without it the working tree is walked. tools/format/allow.txt lists paths whose
owners have not fixed them yet (`kind path-or-glob`, `*` for every kind, `#` comments).
"""

import fnmatch
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ALLOW_FILE = os.path.join(ROOT, "tools", "format", "allow.txt")

MAX_PY_LINE = 120
SKIP_PREFIXES = ("third_party/", "build/", ".git/")
SKIP_PARTS = ("__pycache__",)
KINDS = ("cmake", "python", "shell", "json", "frontmatter")

# Keys of docs/templates/DOC.md, in order; max_lines is optional and last.
FRONT_REQUIRED = (
    "id",
    "kind",
    "title",
    "status",
    "version",
    "owner",
    "episodes",
    "depends_on",
    "supersedes",
    "decisions",
)
FRONT_OPTIONAL = ("max_lines",)
# Byte copy of the pinned master plan: carries no front matter by design.
FRONT_EXEMPT = ("docs/plan/SL-PLAN-001-master-plan.md",)

JSON_INDENT1 = ("registry/SL-REG-002-episodes.json", "registry/SL-REG-002.accepted.json")
JSON_ROOTS = ("registry/", "deps/", "schemas/", "tools/")

KEY_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_-]*)\s*:(?:\s|$)")


def load_allow(path=ALLOW_FILE):
    rules = []
    try:
        with open(path, encoding="utf-8") as fh:
            for raw in fh:
                line = raw.split("#", 1)[0].strip()
                if not line:
                    continue
                parts = line.split(None, 1)
                if len(parts) == 1:
                    rules.append(("*", parts[0]))
                else:
                    rules.append((parts[0], parts[1].strip()))
    except FileNotFoundError:
        pass
    return rules


def allowed(rules, kind, rel):
    for k, pat in rules:
        if k in ("*", kind) and (rel == pat or fnmatch.fnmatchcase(rel, pat)):
            return True
    return False


def read_text(path):
    with open(path, "rb") as fh:
        raw = fh.read()
    return raw.decode("utf-8")


def shebang(path):
    try:
        with open(path, "rb") as fh:
            first = fh.readline(200)
    except OSError:
        return ""
    if first.startswith(b"#!"):
        return first.decode("utf-8", "replace").strip()
    return ""


def classify(rel):
    """Return the list of kinds that apply to the repo-relative path."""
    kinds = []
    base = os.path.basename(rel)
    full = os.path.join(ROOT, rel)
    if base == "CMakeLists.txt" or base.endswith(".cmake") or base.endswith(".cmake.in"):
        kinds.append("cmake")
    if base.endswith(".py"):
        kinds.append("python")
    elif "." not in base and "python" in shebang(full):
        kinds.append("python")
    if base.endswith(".sh"):
        kinds.append("shell")
    elif "." not in base and rel.startswith(("scripts/", "tests/gates/")):
        sb = shebang(full)
        if "bash" in sb or re.search(r"\bsh\b", sb):
            kinds.append("shell")
    if base.endswith(".json") and rel.startswith(JSON_ROOTS):
        kinds.append("json")
    if base.startswith("SL-") and base.endswith(".md") and rel.startswith("docs/"):
        kinds.append("frontmatter")
    return kinds


def list_tree():
    out = []
    for dirpath, dirnames, filenames in os.walk(ROOT):
        rel_dir = os.path.relpath(dirpath, ROOT)
        skip = {".git", "__pycache__"} | ({"build", "third_party"} if rel_dir == "." else set())
        dirnames[:] = sorted(d for d in dirnames if d not in skip)
        for name in sorted(filenames):
            rel = os.path.normpath(os.path.join(rel_dir, name))
            out.append(rel)
    return out


def selected(rel):
    if rel.startswith(SKIP_PREFIXES) or any(p in rel.split("/") for p in SKIP_PARTS):
        return False
    return os.path.isfile(os.path.join(ROOT, rel))


# --- text rules shared by cmake, python and shell ---------------------------------------------


def text_findings(kind, rel, text, tabs):
    """Trailing whitespace, final newline and (optionally) tab indentation."""
    found = []
    if text and not text.endswith("\n"):
        found.append((kind, rel, text.count("\n") + 1, "no newline at end of file"))
    for n, line in enumerate(text.split("\n"), 1):
        if line != line.rstrip(" \t\r"):
            found.append((kind, rel, n, "trailing whitespace"))
        if tabs and re.match(r"^ *\t", line):
            found.append((kind, rel, n, "tab used for indentation"))
    return found


def fix_text(text, expand_tabs):
    lines = [ln.rstrip(" \t\r") for ln in text.split("\n")]
    if expand_tabs:
        lines = [re.sub(r"^((?: *\t)+)", lambda m: m.group(1).replace("\t", "  "), ln) for ln in lines]
    while lines and lines[-1] == "":
        lines.pop()
    return "\n".join(lines) + "\n" if lines else ""


# --- per kind ---------------------------------------------------------------------------------


def check_cmake(rel, text):
    found = text_findings("cmake", rel, text, tabs=True)
    depth = 0
    in_bracket = None
    for n, line in enumerate(text.split("\n"), 1):
        if in_bracket is not None:
            if in_bracket in line:
                in_bracket = None
            continue
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        m = re.match(r"^( *)", line)
        indent = len(m.group(1))
        if depth == 0 and indent % 2:
            found.append(("cmake", rel, n, "indentation is %d spaces, not a multiple of 2" % indent))
        code = re.sub(r'"(?:\\.|[^"\\])*"', '""', line)
        bm = re.search(r"\[(=*)\[", code)
        if bm:
            close = "]" + bm.group(1) + "]"
            if close not in code[bm.end() :]:
                in_bracket = close
            code = code[: bm.start()]
        code = code.split("#", 1)[0]
        depth = max(0, depth + code.count("(") - code.count(")"))
    return found


def check_python(rel, text):
    found = text_findings("python", rel, text, tabs=False)
    try:
        compile(text, rel, "exec", dont_inherit=True)
    except SyntaxError as exc:
        found.append(("python", rel, exc.lineno or 1, "syntax error: %s" % exc.msg))
    except (ValueError, RecursionError) as exc:
        found.append(("python", rel, 1, "does not compile: %s" % exc))
    for n, line in enumerate(text.split("\n"), 1):
        if len(line) > MAX_PY_LINE:
            found.append(("python", rel, n, "line is %d characters, limit %d" % (len(line), MAX_PY_LINE)))
    return found


def check_shell(rel, text):
    return text_findings("shell", rel, text, tabs=True)


def canonical_json(rel, data):
    if rel in JSON_INDENT1:
        return json.dumps(data, indent=1, sort_keys=True, ensure_ascii=False) + "\n"
    return json.dumps(data, indent=2, ensure_ascii=False) + "\n"


def check_json(rel, text):
    try:
        data = json.loads(text)
    except ValueError as exc:
        return [("json", rel, getattr(exc, "lineno", 1), "invalid JSON: %s" % exc)]
    want = canonical_json(rel, data)
    if want == text:
        return []
    style = "indent 1 sort_keys" if rel in JSON_INDENT1 else "indent 2"
    have_lines, want_lines = text.split("\n"), want.split("\n")
    line = next(
        (i + 1 for i, (a, b) in enumerate(zip(have_lines, want_lines)) if a != b),
        min(len(have_lines), len(want_lines)),
    )
    return [("json", rel, line, "not canonical json.dumps(%s, ensure_ascii=False): run `just fmt-all`" % style)]


def check_frontmatter(rel, text):
    if rel in FRONT_EXEMPT:
        return []
    lines = text.split("\n")
    if not lines or lines[0] != "---":
        return [("frontmatter", rel, 1, "missing front matter (the file must start with ---)")]
    try:
        end = lines.index("---", 1)
    except ValueError:
        return [("frontmatter", rel, 1, "front matter is not closed with ---")]
    keys = []
    for n in range(1, end):
        m = KEY_RE.match(lines[n])
        if m:
            keys.append((m.group(1), n + 1))
    names = [k for k, _ in keys]
    found = []
    for req in FRONT_REQUIRED:
        if req not in names:
            found.append(("frontmatter", rel, 1, "missing key '%s'" % req))
    known = FRONT_REQUIRED + FRONT_OPTIONAL
    expected = [k for k in known if k in names]
    seen = [k for k in names if k in known]
    if not found and seen != expected:
        for (k, n), want in zip([(k, n) for k, n in keys if k in known], expected):
            if k != want:
                found.append(("frontmatter", rel, n, "key '%s' is out of order, expected '%s' here" % (k, want)))
                break
    for k, n in keys:
        if k not in known:
            found.append(("frontmatter", rel, n, "unknown key '%s' (DOC.md keys: %s)" % (k, ", ".join(known))))
    return found


CHECKS = {
    "cmake": check_cmake,
    "python": check_python,
    "shell": check_shell,
    "json": check_json,
    "frontmatter": check_frontmatter,
}


def fix_file(kind, rel, text):
    """Return the fixed text, or None when this kind has no mechanical fix."""
    if kind in ("cmake", "python", "shell"):
        return fix_text(text, expand_tabs=(kind == "cmake"))
    if kind == "json":
        try:
            return canonical_json(rel, json.loads(text))
        except ValueError:
            return None
    return None


def run(files, mode, rules, out):
    findings = []
    checked = 0
    skipped = 0
    for rel in files:
        for kind in classify(rel):
            if allowed(rules, kind, rel):
                skipped += 1
                continue
            path = os.path.join(ROOT, rel)
            try:
                text = read_text(path)
            except UnicodeDecodeError:
                findings.append((kind, rel, 1, "not valid UTF-8"))
                continue
            got = CHECKS[kind](rel, text)
            if got and mode == "fix":
                fixed = fix_file(kind, rel, text)
                if fixed is not None and fixed != text:
                    with open(path, "w", encoding="utf-8", newline="") as fh:
                        fh.write(fixed)
                    out.write("fixed:%s:%s\n" % (kind, rel))
                    got = CHECKS[kind](rel, fixed)
            checked += 1
            findings.extend(got)
    return findings, checked, skipped


def list_kind(files, kind, rules, out):
    """Print NUL-separated paths of one kind that are not allowlisted (for `xargs -0 shfmt`)."""
    for rel in files:
        if kind in classify(rel) and not allowed(rules, "shfmt", rel) and not allowed(rules, kind, rel):
            out.write(rel + "\0")


def main(argv):
    mode = None
    from_stdin = False
    list_only = None
    args = list(argv[1:])
    while args:
        a = args.pop(0)
        if a == "--check":
            mode = "check"
        elif a == "--fix":
            mode = "fix"
        elif a == "--files-from-stdin":
            from_stdin = True
        elif a == "--list" and args:
            list_only = args.pop(0)
        else:
            sys.stderr.write("usage: check.py --check|--fix [--files-from-stdin] [--list shell]\n")
            return 2
    if mode is None and list_only is None:
        sys.stderr.write("usage: check.py --check|--fix [--files-from-stdin] [--list shell]\n")
        return 2
    if from_stdin:
        files = [f for f in sys.stdin.buffer.read().decode("utf-8").split("\0") if f]
    else:
        files = list_tree()
    files = sorted(set(f for f in files if selected(f)))
    rules = load_allow()
    if list_only is not None:
        if list_only not in KINDS:
            sys.stderr.write("check.py: unknown kind '%s'\n" % list_only)
            return 2
        list_kind(files, list_only, rules, sys.stdout)
        return 0
    findings, checked, skipped = run(files, mode, rules, sys.stdout)
    for kind, rel, line, msg in findings:
        print("%s:%s:%d: %s" % (kind, rel, line, msg))
    if findings:
        bad = sorted({k for k, _, _, _ in findings})
        print("format-check: FAIL: %d finding(s) in kind(s) %s" % (len(findings), ", ".join(bad)))
        return 1
    print("format-check: ok (%d file-kind checks, %d allowlisted)" % (checked, skipped))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
