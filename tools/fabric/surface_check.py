"""fabric-surface-check: the F5 public-surface lint (WO-051, ADR-0016, SL-ARCH-002 ARCH2-R01, FAB-INV-F5-03).

Scans the public headers engine/fabric/include/sl/fabric/*.hpp (not internal/, not gen/) for any way to change stored
data that is not behind the mutation_capability passkey, and reports `path:line: RULE: message`:

  R-F5-FRIEND   a friend class or friend function (hidden friend operators excepted) that is not in the allow-list
  R-F5-MUTABLE  a `mutable` member of a class in a public header; synchronization members (std::mutex,
                std::shared_mutex, std::recursive_mutex, std::condition_variable, std::once_flag, std::atomic<...>)
                are exempt: they guard data, they do not make a const view writable
  R-F5-CAP      mutation_capability constructible or copyable: its copy and move constructors and assignments not
                all deleted, a constructor that is not private, the class missing, or the capability taken or
                returned other than as `const mutation_capability &` anywhere else in the public headers
  R-F5-SETTER   on the data classes (value, snapshot, snapshot_entry, record_view): a public non-const member function
                returning a non-const reference, pointer or std::span into the data, or any public set_* method
  R-F5-INCLUDE  a public header including an internal/ header
  R-F5-ALLOW    a malformed allow-list line, or an entry that matches nothing (stale)

and, over the consumers of Fabric rather than its headers (WO-061, SL-ARCH-001 ARCH1-R61, FAB-INV-BND-03):

  R-BND-INCLUDE a file under engine/runtime/ or engine/render/ that includes sl/fabric/internal/** or a header that
                declares the mutation capability (sl/fabric/store.hpp) or scopes it (sl/fabric/f5.hpp); reported
                with file:line, no allow-list entry can excuse it

Comments and string literals are blanked before matching. The allow-list (tools/fabric/surface-allow.txt) holds
`<path>:<rule>:<symbol> <reason>` lines, `#` starts a comment, and an entry without a reason is an error. The symbol of
a friend is `<class>:<friend as written>` (store.hpp:R-F5-FRIEND:mutation_capability:transaction_manager), of any
other finding `<class>::<member>`. A new friend of mutation_capability needs ADR-0016 amended. Stdlib only, no
subprocess (ADR-0004): `python3 -I tools/fabric/surface_check.py [--root DIR] [--allow FILE]`.
"""

import os
import posixpath
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HEADER_DIR = "engine/fabric/include/sl/fabric"
DEFAULT_ALLOW = os.path.join("tools", "fabric", "surface-allow.txt")
RULES = ("R-F5-FRIEND", "R-F5-MUTABLE", "R-F5-CAP", "R-F5-SETTER", "R-F5-INCLUDE")
DATA_CLASSES = ("value", "snapshot", "snapshot_entry", "record_view")
CAPABILITY = "mutation_capability"
SYNC_TYPES = re.compile(
    r"\bstd::(?:mutex|shared_mutex|recursive_mutex|timed_mutex|shared_timed_mutex|condition_variable(?:_any)?|"
    r"once_flag|atomic(?:_flag)?\b)")

BND_DIRS = ("engine/runtime", "engine/render")
BND_EXTENSIONS = (".hpp", ".h", ".hh", ".hxx", ".inl", ".ipp", ".cpp", ".cc", ".cxx", ".c", ".mm", ".cppm", ".ixx")
_BND_DIRECTIVE = re.compile(r"^\s*#\s*(?:include|import)\b")
_BND_PATH = re.compile(r'^\s*#\s*(?:include|import)\s*[<"]([^>"]+)[>"]')
_BND_INTERNAL = re.compile(r"(?:^|/)fabric/internal/")
_BND_CAPABILITY = re.compile(r"(?:^|/)fabric/(?:store|f5)\.hpp$")

_INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')
_CLASS = re.compile(r"\b(class|struct)\s+(?:\[\[[^\]]*\]\]\s*)?(\w+)\s*(?:final\b\s*)?(?::[^;{()]*)?\{")
_ACCESS = re.compile(r"^\s*(public|private|protected)\s*:")
_ATTR = re.compile(r"\[\[.*?\]\]")


def blank(text):
    """Return text with comments and string/char literal contents replaced by spaces; newlines and offsets kept."""
    out, i, n = [], 0, len(text)
    while i < n:
        two = text[i : i + 2]
        c = text[i]
        if two == "//":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif two == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif c == "'" and i > 0 and text[i - 1].isalnum():
            out.append(c)  # a digit separator such as 1'000
            i += 1
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(c + " " * max(j - i - 2, 0) + (c if j - i >= 2 else ""))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _blank_preprocessor(clean):
    """Blank preprocessor lines (an include path or a macro body is not a declaration)."""
    return "\n".join(" " * len(line) if line.lstrip().startswith("#") else line for line in clean.split("\n"))


def _match_brace(text, open_pos):
    depth = 0
    for j in range(open_pos, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return j
    return len(text)


def _classes(clean):
    """Yield (name, kind, body_start, body_end) for every class or struct definition (nested ones too)."""
    for m in _CLASS.finditer(clean):
        before = clean[max(0, m.start() - 12) : m.start()]
        if re.search(r"\benum\s*$", before):
            continue
        open_pos = m.end() - 1
        yield m.group(2), m.group(1), open_pos + 1, _match_brace(clean, open_pos)


def _members(clean, start, end, kind):
    """Yield (offset, access, declaration text) for the top-level member declarations of one class body.

    A declaration ends at `;` or at the `{` of an inline body (which is skipped); nested class bodies are skipped too.
    """
    access = "public" if kind == "struct" else "private"
    i, decl_start = start, start
    while i < end:
        c = clean[i]
        if c == "{":
            text = clean[decl_start:i]
            yield from _emit(clean, decl_start, text, access)
            close = _match_brace(clean, i)
            i = close + 1
            # `= default;`-style tails and the `;` after a nested class end the declaration
            while i < end and clean[i] in " \t\n":
                i += 1
            if i < end and clean[i] == ";":
                i += 1
            decl_start = i
            continue
        if c == ";":
            yield from _emit(clean, decl_start, clean[decl_start:i], access)
            decl_start = i + 1
        elif c == ":" and clean[i + 1 : i + 2] != ":" and clean[i - 1 : i] != ":":
            label = _ACCESS.match(clean[decl_start : i + 1])
            if label:
                access = label.group(1)
                decl_start = i + 1
        i += 1
    return


def _emit(clean, decl_start, text, access):
    stripped = text.strip()
    if stripped:
        offset = decl_start + (len(text) - len(text.lstrip()))
        yield offset, access, " ".join(stripped.split())


def _function_parts(decl):
    """Return (return type, name, qualifiers after the parameter list) or None when `decl` is not a function."""
    d = _ATTR.sub(" ", decl)
    d = re.sub(r"^\s*template\s*<[^{};]*?>\s*(?=[A-Za-z_~])", "", d)
    depth, open_paren = 0, -1
    for j, ch in enumerate(d):
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth = max(depth - 1, 0)
        elif ch == "(" and depth == 0:
            open_paren = j
            break
    if open_paren < 0:
        return None
    head = d[:open_paren].rstrip()
    m = re.search(r"(~?\w+|operator\s*\S+)$", head)
    if not m:
        return None
    pdepth, close = 0, len(d)
    for j in range(open_paren, len(d)):
        if d[j] == "(":
            pdepth += 1
        elif d[j] == ")":
            pdepth -= 1
            if pdepth == 0:
                close = j
                break
    return head[: m.start()].strip(), m.group(1), d[close + 1 :]


def _nonconst_view(ret):
    """True when a return type is a reference, pointer or std::span to non-const data."""
    words = re.sub(r"\b(?:virtual|inline|constexpr|consteval|explicit|static|friend)\b", " ", ret).strip()
    span = re.search(r"\bstd::span\s*<\s*(.*)>", words)
    if span:
        return not re.match(r"const\b", span.group(1).strip()) and not re.search(r"\bconst\s*$", span.group(1))
    if not re.search(r"[&*]\s*$", words):
        return False
    base = re.sub(r"[&*\s]+$", "", words)
    return not (re.match(r"const\b", base) or re.search(r"\bconst$", base))


def _line_of(starts, pos):
    lo, hi = 0, len(starts)
    while lo < hi - 1:
        mid = (lo + hi) // 2
        lo, hi = (mid, hi) if starts[mid] <= pos else (lo, mid)
    return lo + 1


def lint_text(text):
    """Return a sorted list of (line, rule, symbol, message) for one public header."""
    found = set()
    for n, raw in enumerate(text.splitlines(), start=1):
        m = _INCLUDE.match(raw)
        if m and (m.group(1).startswith("internal/") or "/internal/" in m.group(1)):
            found.add((n, "R-F5-INCLUDE", m.group(1), "public header includes internal header %s" % m.group(1)))
    clean = _blank_preprocessor(blank(text))
    starts = [0] + [m.end() for m in re.finditer("\n", clean)]
    cap_bodies = []
    for cls, kind, start, end in _classes(clean):
        if cls == CAPABILITY:
            cap_bodies.append((start, end))
            found.update(_capability_findings(clean, starts, start, end))
        for offset, access, decl in _members(clean, start, end, kind):
            line = _line_of(starts, offset)
            first = decl.split(" ", 1)[0]
            if first == "friend":
                if re.search(r"\boperator\b", decl):
                    continue  # a hidden friend operator grants no access to anyone
                name = re.sub(r"^friend\s+(?:class\s+|struct\s+)?", "", decl).strip()
                parts = _function_parts(name)
                friend = parts[1] if parts else name
                found.add((line, "R-F5-FRIEND", "%s:%s" % (cls, friend),
                           "class %s declares friend %s" % (cls, friend)))
                continue
            if re.search(r"\bmutable\b", decl) and not re.search(r"\)\s*mutable\b", decl):
                if not SYNC_TYPES.search(decl):
                    member = re.sub(r"\s*(?:[={].*)?$", "", decl).split(" ")[-1]
                    found.add((line, "R-F5-MUTABLE", "%s::%s" % (cls, member),
                               "mutable member %s of class %s makes a const view writable" % (member, cls)))
            if cls in DATA_CLASSES and access == "public":
                parts = _function_parts(decl)
                if not parts:
                    continue
                ret, name, quals = parts
                if name.startswith("set_"):
                    found.add((line, "R-F5-SETTER", "%s::%s" % (cls, name),
                               "setter %s on immutable data class %s" % (name, cls)))
                    continue
                if name == cls or name.startswith("~") or name.startswith("operator") or not ret:
                    continue
                if re.search(r"\bstatic\b", ret) or re.search(r"\bconst\b", quals.split("=")[0]):
                    continue
                if _nonconst_view(ret):
                    found.add((line, "R-F5-SETTER", "%s::%s" % (cls, name),
                               "non-const member %s of %s returns a writable view (%s) into stored data"
                               % (name, cls, ret)))
    found.update(_capability_uses(clean, starts, cap_bodies))
    return sorted(found)


def _capability_findings(clean, starts, start, end):
    """R-F5-CAP inside the class: the four copy/move members deleted and no constructor that is not private."""
    out = set()
    c = CAPABILITY
    need = {
        "copy constructor": re.compile(r"^%s\s*\(\s*const\s+%s\s*&\s*\w*\s*\)\s*=\s*delete$" % (c, c)),
        "move constructor": re.compile(r"^%s\s*\(\s*%s\s*&&\s*\w*\s*\)\s*=\s*delete$" % (c, c)),
        "copy assignment": re.compile(r"^%s\s*&\s*operator=\s*\(\s*const\s+%s\s*&\s*\w*\s*\)\s*=\s*delete$" % (c, c)),
        "move assignment": re.compile(r"^%s\s*&\s*operator=\s*\(\s*%s\s*&&\s*\w*\s*\)\s*=\s*delete$" % (c, c)),
    }
    seen = set()
    for offset, access, decl in _members(clean, start, end, "class"):
        d = re.sub(r"\b(?:constexpr|explicit|inline|noexcept)\b", " ", _ATTR.sub(" ", decl))
        d = " ".join(d.split())
        for what, rx in need.items():
            if rx.match(d):
                seen.add(what)
        parts = _function_parts(d)
        if parts and parts[1] == CAPABILITY and not parts[0] and access != "private":
            if not re.search(r"=\s*delete\s*$", d):
                out.add((_line_of(starts, offset), "R-F5-CAP", "%s::%s" % (CAPABILITY, CAPABILITY),
                         "%s has a %s constructor: anyone could make the passkey" % (CAPABILITY, access)))
    line = _line_of(starts, start)
    for what in need:
        if what not in seen:
            out.add((line, "R-F5-CAP", "%s::%s" % (CAPABILITY, what.replace(" ", "_")),
                     "%s %s is not deleted: the passkey could be copied or moved" % (CAPABILITY, what)))
    return out


def _capability_uses(clean, starts, bodies):
    """R-F5-CAP outside the class: only forward declarations and `const mutation_capability &` parameters."""
    out = set()
    for m in re.finditer(r"\b%s\b" % CAPABILITY, clean):
        if any(s <= m.start() <= e for s, e in bodies):
            continue
        before = clean[max(0, m.start() - 16) : m.start()]
        after = clean[m.end() : m.end() + 4]
        if re.search(r"\b(?:class|struct)\s+$", before) and re.match(r"\s*[;{:]", after):
            continue  # a forward declaration or the definition itself
        if re.search(r"\bconst\s+$", before) and re.match(r"\s*&(?!&)", after):
            continue
        out.add((_line_of(starts, m.start()), "R-F5-CAP", "%s:use" % CAPABILITY,
                 "%s used other than as `const %s &`" % (CAPABILITY, CAPABILITY)))
    return out


def load_allow(path):
    """Return (dict (path, rule, symbol) -> allow line number, error strings); a missing file is empty."""
    allowed, errors = {}, []
    try:
        with open(path, encoding="utf-8") as fh:
            lines = fh.read().splitlines()
    except OSError:
        return allowed, errors
    shown = os.path.basename(path)
    for n, raw in enumerate(lines, start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        head, _, reason = line.partition(" ")
        parts = head.split(":", 2)
        if len(parts) != 3 or not parts[0] or parts[1] not in RULES or not parts[2]:
            errors.append("%s:%d: R-F5-ALLOW: expected `<path>:<rule>:<symbol> <reason>` with a rule in %s, got %r"
                          % (shown, n, ", ".join(RULES), line))
        elif not reason.strip():
            errors.append("%s:%d: R-F5-ALLOW: entry %s has no reason" % (shown, n, head))
        else:
            allowed[tuple(parts)] = n
    return allowed, errors


def headers(root):
    """Relative paths of the public headers, sorted (top level only: internal/ and gen/ are not public)."""
    top = os.path.join(root, *HEADER_DIR.split("/"))
    try:
        names = sorted(os.listdir(top))
    except OSError:
        return []
    return ["%s/%s" % (HEADER_DIR, name) for name in names
            if name.endswith(".hpp") and os.path.isfile(os.path.join(top, name))]


def check_tree(root, allow_path=None):
    """Return (error lines, header count, allowed count)."""
    allow_file = allow_path or os.path.join(root, DEFAULT_ALLOW)
    allowed, errors = load_allow(allow_file)
    used = set()
    files = headers(root)
    cap_found = False
    for rel in files:
        try:
            with open(os.path.join(root, *rel.split("/")), encoding="utf-8", errors="replace") as fh:
                text = fh.read()
        except OSError as exc:
            errors.append("%s:1: R-F5-ALLOW: unreadable: %s" % (rel, exc))
            continue
        if re.search(r"\bclass\s+%s\s*(?:final\s*)?\{" % CAPABILITY, blank(text)):
            cap_found = True
        for line, rule, symbol, message in lint_text(text):
            key = (rel, rule, symbol)
            if key in allowed:
                used.add(key)
            else:
                errors.append("%s:%d: %s: %s" % (rel, line, rule, message))
    if files and not cap_found:
        errors.append("%s/store.hpp:1: R-F5-CAP: class %s is not defined in any public header"
                      % (HEADER_DIR, CAPABILITY))
    for key, n in sorted(allowed.items(), key=lambda kv: kv[1]):
        if key not in used:
            errors.append("%s:%d: R-F5-ALLOW: stale entry %s matches no finding" % (
                os.path.basename(allow_file), n, ":".join(key)))
    return errors, len(files), len(used)


def bnd_lint_text(text):
    """Return a sorted list of (line, included path, reason) for the forbidden includes of one runtime or render file."""
    found = []
    raw_lines = text.splitlines()
    # blank() keeps offsets, so the same line numbers locate a directive in the blanked text (a directive inside a
    # comment or a string is blanked away) and its path in the raw text.
    for n, blanked in enumerate(blank(text).splitlines(), start=1):
        if not _BND_DIRECTIVE.match(blanked):
            continue
        m = _BND_PATH.match(raw_lines[n - 1])
        if not m:
            continue
        path = posixpath.normpath(m.group(1).replace("\\", "/"))
        if _BND_INTERNAL.search(path):
            found.append((n, m.group(1), "an internal Fabric header"))
        elif _BND_CAPABILITY.search(path):
            found.append((n, m.group(1), "a header of the mutation capability"))
    return found


def check_boundary(root):
    """Return (error lines, files scanned): R-BND-INCLUDE over engine/runtime and engine/render under `root`."""
    found, scanned = [], 0
    for top in BND_DIRS:
        for dirpath, dirnames, filenames in os.walk(os.path.join(root, *top.split("/"))):
            dirnames.sort()
            for name in sorted(filenames):
                if not name.endswith(BND_EXTENSIONS):
                    continue
                full = os.path.join(dirpath, name)
                rel = os.path.relpath(full, root).replace(os.sep, "/")
                try:
                    with open(full, encoding="utf-8", errors="replace") as fh:
                        text = fh.read()
                except OSError as exc:
                    found.append((rel, 1, "unreadable: %s" % exc))
                    continue
                scanned += 1
                for line, path, why in bnd_lint_text(text):
                    found.append((rel, line, "includes %s, %s (ARCH1-R61: runtime and render read snapshots "
                                  "through boundary.hpp and propose patches through the transaction manager)"
                                  % (path, why)))
    return ["%s:%d: R-BND-INCLUDE: %s" % item for item in sorted(found)], scanned


def main(argv):
    root, allow, args = ROOT, None, argv[1:]
    while args:
        if args[0] == "--root" and len(args) >= 2:
            root = os.path.abspath(args[1])
        elif args[0] == "--allow" and len(args) >= 2:
            allow = os.path.abspath(args[1])
        else:
            print("usage: surface_check.py [--root DIR] [--allow FILE]", file=sys.stderr)
            return 2
        args = args[2:]
    errors, files, allowed = check_tree(root, allow)
    bnd_errors, scanned = check_boundary(root)
    errors = errors + bnd_errors
    for e in errors:
        print(e)
    if errors:
        print("fabric-surface-check: FAIL (%d finding(s) in %d public header(s) and %d runtime/render file(s))"
              % (len(errors), files, scanned))
        return 1
    print("fabric-surface-check: ok (%d public header(s), %d allowed friend(s) or exception(s), %d runtime/render "
          "file(s) scanned)" % (files, allowed, scanned))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
